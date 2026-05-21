#include "services/bot/BotIndicesService.h"
#include "services/bot/AlpacaRestClient.h"
#include "services/bot/BotConfig.h"
#include "services/bot/PolygonRestClient.h"
#include "services/markets/MarketDataService.h"
#include "datahub/DataHub.h"
#include "python/PythonRunner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QTextStream>
#include <QString>
#include <QStringList>

namespace fincept::services::bot {

// ─── Diag log ────────────────────────────────────────────────────────────
static void mq_diag(const QString& msg) {
    static QString diag_path;
    if (diag_path.isEmpty())
        diag_path = BotConfig::instance().data_path() + "/.pinpunch_botservice.log";
    auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QFile f(diag_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QTextStream s(&f);
        s << stamp << "  [mq] " << msg << "\n";
    }
}

// ─── Symbol classification ───────────────────────────────────────────────
//
// Three buckets per source symbol:
//   1. yfinance (PythonWorker daemon) — handles every Yahoo-style symbol
//      family Alpaca can't serve natively:
//        ^XXX           indices (^GSPC, ^DJI, ^N225, ^FTSE, ^NSEI, ^BSESN, …)
//        XX=F           commodity futures (GC=F, CL=F, NG=F, …)
//        XX=X           forex pairs (EURUSD=X, USDJPY=X, …)
//        XXXX.NS / .BO  NSE/BSE Indian equities
//        000001.SS      Shanghai composite & co
//        XXXX.HK / .L / .AX / .TO  other international exchanges
//      One batch_quotes call per refresh tick, persistent daemon eats the
//      ~2 s yfinance import cost once per app launch. No HTTP rate-limit
//      worry — Yahoo's per-IP limits target rapid uncached pulls; the
//      daemon's bulk yf.download is well under the threshold.
//
//   2. Alpaca crypto (XX-USD → /v1beta3/crypto/us/snapshots) — sub-second
//      USD-pair pricing for the operator's actual broker feed.
//
//   3. Alpaca stocks/ETFs (everything else, treated as a tradeable symbol)
//      — same broker feed as positions/watchlist, ~1 s freshness.
//
// Rationale: the bot trades on Alpaca, so positions/watchlist/quotes for
// US-tradeable symbols MUST come from the same feed (avoids fill-vs-mark
// inconsistency on the operator's screen). For the global-context widgets
// — Indices, Forex, Commodities, India — accuracy of the *true index value*
// matters more than freshness, and yfinance is the cheapest source that
// returns the actual SPX number rather than an SPY-proxy dollar price.
namespace {

bool is_crypto_symbol(const QString& s) {
    // BTC-USD style → Alpaca crypto. Yahoo also uses this format but
    // Alpaca returns sub-second USD-pair pricing so prefer it.
    return s.endsWith("-USD") && !s.contains('.');
}

// Anything we want to route through yfinance. Returns true for any symbol
// that's NOT a bare US-tradeable Alpaca ticker AND is not crypto. Check
// is_crypto_symbol() FIRST in routing — BTC-USD also matches '-' here.
bool is_yfinance_symbol(const QString& s) {
    if (s.startsWith('^')) return true;          // ^GSPC, ^DJI, ^N225, …
    if (s.endsWith("=F"))  return true;          // GC=F, CL=F, …
    if (s.endsWith("=X"))  return true;          // EURUSD=X, USDJPY=X, …
    if (s.contains('.'))   return true;          // .NS, .BO, .SS, .HK, .L, .AX, .TO
    if (s.contains('-'))   return true;          // BRK-B, PBR-A (Yahoo dual-class)
    return false;
}

} // namespace

// ─── Public symbol resolver (for any caller that wants the Alpaca form) ──
QString BotIndicesService::resolve_alpaca_symbol(const QString& src) {
    if (src.isEmpty()) return {};
    if (is_crypto_symbol(src))   return {};   // crypto path, not stock path
    if (is_yfinance_symbol(src)) return {};   // not Alpaca-routable
    return src;                                // direct Alpaca stock/ETF
}

BotIndicesService& BotIndicesService::instance() {
    static BotIndicesService inst;
    return inst;
}

BotIndicesService::BotIndicesService() : QObject(nullptr) {}

QStringList BotIndicesService::topic_patterns() const {
    // Wildcard: claim ownership of every market:quote:* topic. The hub
    // routes any new subscription (whatever symbol the user adds to a
    // panel) to our refresh().
    return { QStringLiteral("market:quote:*") };
}

void BotIndicesService::refresh(const QStringList& topics) {
    fetch_and_publish(topics);
}

// ─── yfinance batch fetch via PythonRunner ───────────────────────────────
// One subprocess spawn per refresh tick (yfinance + pandas import cost
// ~2 s, paid each call — acceptable at our 10 s cadence and avoids the
// PythonWorker daemon's setup dependency on the venv-numpy2 UV install).
// Calls scripts/yfinance_data.py:get_batch_quotes via the same CLI path
// MarketDataService uses for `batch_quotes`. PythonRunner also handles
// async interpreter detection and request queuing — if Python isn't
// resolved yet, the request waits instead of failing.
//
// Script CLI: python yfinance_data.py batch_quotes <sym1> <sym2> …
// Returns a JSON array; each entry has
//   {symbol, price, change, change_percent, high, low,
//    previous_close, volume, exchange, …}
static void fetch_yfinance_batch(const QStringList& symbols) {
    if (symbols.isEmpty()) return;
    QStringList args;
    args << QStringLiteral("batch_quotes");
    args.append(symbols);

    fincept::python::PythonRunner::instance().run(
        QStringLiteral("yfinance_data.py"), args,
        [symbols](fincept::python::PythonResult result) {
            if (!result.success) {
                mq_diag(QString("yfinance batch FAIL (%1 syms): %2")
                            .arg(symbols.size())
                            .arg(result.error.left(300)));
                return;
            }
            // PythonRunner returns the raw stdout in `output`. Use the
            // shared extract_json helper so any Python warnings/progress
            // lines printed before the JSON body don't break parsing.
            const QString body = fincept::python::extract_json(result.output);
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &pe);
            if (pe.error != QJsonParseError::NoError) {
                mq_diag(QString("yfinance JSON parse FAIL (%1 syms): %2")
                            .arg(symbols.size()).arg(pe.errorString()));
                return;
            }
            if (!doc.isArray()) {
                // Script returns {"error": "..."} on top-level failure.
                if (doc.isObject() && doc.object().contains("error")) {
                    mq_diag(QString("yfinance script error: %1")
                                .arg(doc.object().value("error").toString().left(300)));
                } else {
                    mq_diag(QString("yfinance batch: not an array (%1 syms)")
                                .arg(symbols.size()));
                }
                return;
            }
            const QJsonArray quotes = doc.array();
            auto& hub = fincept::datahub::DataHub::instance();
            int published = 0;
            for (const auto& v : quotes) {
                const QJsonObject q = v.toObject();
                if (q.isEmpty() || q.contains("error")) continue;
                const QString sym  = q.value("symbol").toString();
                const double  cur  = q.value("price").toDouble();
                const double  chg  = q.value("change").toDouble();
                const double  chgp = q.value("change_percent").toDouble();
                if (sym.isEmpty() || cur <= 0) continue;

                services::QuoteData qd;
                qd.symbol     = sym;
                qd.name       = sym;
                qd.price      = cur;
                qd.change     = chg;
                qd.change_pct = chgp;
                qd.high       = q.value("high").toDouble();
                qd.low        = q.value("low").toDouble();
                qd.volume     = q.value("volume").toDouble();
                hub.publish(QStringLiteral("market:quote:") + sym,
                            QVariant::fromValue(qd));
                ++published;
            }
            mq_diag(QString("yfinance batch: requested %1, published %2")
                        .arg(symbols.size()).arg(published));
        });
}

void BotIndicesService::fetch_and_publish(const QStringList& topics) {
    // Extract source symbols from topic strings: "market:quote:^GSPC" → "^GSPC"
    QStringList src_syms;
    for (const QString& t : topics) {
        if (t.startsWith("market:quote:")) src_syms.append(t.mid(13));
    }
    if (src_syms.isEmpty()) return;

    // Partition by source. Crypto check first because BTC-USD also matches
    // is_yfinance_symbol() (the dash trigger covers Yahoo dual-class shares
    // BRK-B etc.) — without ordering, crypto would land in the wrong bucket.
    //
    // For bare-alpha US tickers we prefer Polygon when a Polygon API key is
    // configured: it returns sub-second-precise snapshots and the Stocks
    // Developer plan has unlimited API calls, so 1s polling is essentially
    // free. Falls back to Alpaca direct snapshots when no key is set.
    const bool polygon_ok = PolygonRestClient::instance().is_configured();
    QStringList yf_syms;
    QStringList stock_alpaca_syms;
    QStringList stock_polygon_syms;
    QStringList crypto_pairs;
    QHash<QString, QString> crypto_pair_to_src;
    for (const QString& src : src_syms) {
        if (is_crypto_symbol(src)) {
            const QString base = src.left(src.size() - 4);
            const QString pair = base + "/USD";
            if (!crypto_pairs.contains(pair)) {
                crypto_pairs.append(pair);
                crypto_pair_to_src.insert(pair, src);
            }
        } else if (is_yfinance_symbol(src)) {
            if (!yf_syms.contains(src)) yf_syms.append(src);
        } else if (polygon_ok) {
            // Direct US stock / ETF / ADR → Polygon (sub-second snapshot)
            if (!stock_polygon_syms.contains(src)) stock_polygon_syms.append(src);
        } else {
            // Direct Alpaca stock/ETF (fallback when no Polygon key)
            if (!stock_alpaca_syms.contains(src)) stock_alpaca_syms.append(src);
        }
    }

    // ── yfinance batch (indices, futures, forex, international stocks) ─
    if (!yf_syms.isEmpty()) {
        mq_diag(QString("yfinance batch: requesting %1 syms (e.g. %2)")
                    .arg(yf_syms.size())
                    .arg(yf_syms.mid(0, 6).join(",")));
        fetch_yfinance_batch(yf_syms);
    }

    // ── Stock/ETF batch via Polygon /v3/snapshot ──────────────────────
    // Sub-second-precise prices, single batched HTTP call per refresh.
    // Used when POLYGON_API_KEY is configured; falls through to Alpaca
    // path otherwise. Response contains `results: [{ticker, session: {
    // price, change, change_percent, high, low, volume, ...}}]`.
    if (!stock_polygon_syms.isEmpty()) {
        PolygonRestClient::instance().get_snapshots(stock_polygon_syms,
            [stock_polygon_syms](bool ok, const QJsonValue& v, const QString& err) {
                if (!ok || !v.isObject()) {
                    mq_diag(QString("polygon snapshots FAIL: %1").arg(err));
                    return;
                }
                const QJsonObject root = v.toObject();
                const QJsonArray results = root.value("results").toArray();
                auto& hub = fincept::datahub::DataHub::instance();
                int count = 0;
                for (const auto& rv : results) {
                    const QJsonObject r = rv.toObject();
                    const QString sym = r.value("ticker").toString();
                    const QJsonObject sess = r.value("session").toObject();
                    const double last = sess.value("price").toDouble();
                    if (sym.isEmpty() || last <= 0) continue;

                    services::QuoteData q;
                    q.symbol     = sym;
                    q.name       = sym;
                    q.price      = last;
                    q.change     = sess.value("change").toDouble();
                    q.change_pct = sess.value("change_percent").toDouble();
                    q.high       = sess.value("high").toDouble();
                    q.low        = sess.value("low").toDouble();
                    q.volume     = sess.value("volume").toDouble();
                    hub.publish(QStringLiteral("market:quote:") + sym,
                                QVariant::fromValue(q));
                    ++count;
                }
                mq_diag(QString("polygon snapshots: requested %1, published %2")
                            .arg(stock_polygon_syms.size()).arg(count));
            });
    }

    // ── Stock/ETF batch via Alpaca /v2/stocks/snapshots ───────────────
    // Bot's actual broker feed. Same source as positions/watchlist so
    // intraday marks line up across the dashboard. Used when no Polygon
    // key is configured (Polygon path above takes precedence when keyed).
    if (!stock_alpaca_syms.isEmpty()) {
        AlpacaRestClient::instance().get_snapshots(stock_alpaca_syms,
            [stock_alpaca_syms](bool ok, const QJsonValue& v, const QString& err) {
                if (!ok || !v.isObject()) {
                    mq_diag(QString("stock snapshots FAIL: %1").arg(err));
                    return;
                }
                auto snap = v.toObject();
                auto& hub = fincept::datahub::DataHub::instance();
                int count = 0;
                for (const QString& src : stock_alpaca_syms) {
                    if (!snap.contains(src)) continue;
                    auto so   = snap.value(src).toObject();
                    auto lt   = so.value("latestTrade").toObject();
                    auto pdb  = so.value("prevDailyBar").toObject();
                    auto db   = so.value("dailyBar").toObject();
                    const double last = lt.value("p").toDouble();
                    const double prev = pdb.value("c").toDouble(db.value("o").toDouble());
                    if (last <= 0) continue;
                    services::QuoteData q;
                    q.symbol     = src;
                    q.name       = src;
                    q.price      = last;
                    q.change     = (prev > 0) ? (last - prev) : 0.0;
                    q.change_pct = (prev > 0) ? ((last - prev) / prev * 100.0) : 0.0;
                    q.high       = db.value("h").toDouble();
                    q.low        = db.value("l").toDouble();
                    q.volume     = db.value("v").toDouble();
                    hub.publish(QStringLiteral("market:quote:") + src,
                                QVariant::fromValue(q));
                    ++count;
                }
                mq_diag(QString("alpaca stock snapshots: requested %1, published %2")
                            .arg(stock_alpaca_syms.size()).arg(count));
            });
    }

    // ── Crypto batch via Alpaca /v1beta3/crypto/us/snapshots ──────────
    if (!crypto_pairs.isEmpty()) {
        AlpacaRestClient::instance().get_crypto_snapshots(crypto_pairs,
            [crypto_pair_to_src](bool ok, const QJsonValue& v, const QString& err) {
                if (!ok || !v.isObject()) {
                    mq_diag(QString("crypto snapshots FAIL: %1").arg(err));
                    return;
                }
                // Response shape: { snapshots: { "BTC/USD": { latestTrade, … } } }
                auto root = v.toObject();
                auto snap = root.contains("snapshots")
                                ? root.value("snapshots").toObject()
                                : root;
                auto& hub = fincept::datahub::DataHub::instance();
                int count = 0;
                for (auto it = crypto_pair_to_src.constBegin();
                     it != crypto_pair_to_src.constEnd(); ++it) {
                    const QString pair = it.key();
                    const QString src  = it.value();
                    if (!snap.contains(pair)) continue;
                    auto so   = snap.value(pair).toObject();
                    auto lt   = so.value("latestTrade").toObject();
                    auto pdb  = so.value("prevDailyBar").toObject();
                    auto db   = so.value("dailyBar").toObject();
                    const double last = lt.value("p").toDouble();
                    const double prev = pdb.value("c").toDouble(db.value("o").toDouble());
                    if (last <= 0) continue;
                    services::QuoteData q;
                    q.symbol     = src;
                    q.name       = src;
                    q.price      = last;
                    q.change     = (prev > 0) ? (last - prev) : 0.0;
                    q.change_pct = (prev > 0) ? ((last - prev) / prev * 100.0) : 0.0;
                    q.high       = db.value("h").toDouble();
                    q.low        = db.value("l").toDouble();
                    q.volume     = db.value("v").toDouble();
                    hub.publish(QStringLiteral("market:quote:") + src,
                                QVariant::fromValue(q));
                    ++count;
                }
                mq_diag(QString("alpaca crypto: requested %1, published %2")
                            .arg(crypto_pair_to_src.size()).arg(count));
            });
    }
}

} // namespace fincept::services::bot
