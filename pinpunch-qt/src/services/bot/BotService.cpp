#include "services/bot/BotService.h"
#include "services/bot/BotConfig.h"
#include "services/bot/AlpacaRestClient.h"
#include "datahub/DataHub.h"
// Pinpunch's existing NewsArticle type — we ALSO publish to `news:general`
// so existing widgets (NewsWidget, etc.) light up with bot data without
// needing per-widget refactoring.
#include "services/news/NewsService.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimeZone>

#include <cstdio>

Q_LOGGING_CATEGORY(botSvc, "fincept.bot.service")

// ─── Diagnostic sentinel log — writes to ~/grok-claude/.pinpunch_botservice.log
// Bypasses ALL Qt logging machinery (which on macOS may route through
// NSLog or a custom message handler installed AFTER our init runs).
// Lets the operator/Claude verify BotService is actually being called.
static void bot_diag(const QString& msg) {
    static QString diag_path;
    if (diag_path.isEmpty()) {
        QString home = QString::fromUtf8(qgetenv("HOME"));
        if (home.isEmpty()) home = QDir::homePath();
        diag_path = home + "/grok-claude/.pinpunch_botservice.log";
    }
    auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QFile f(diag_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QTextStream s(&f);
        s << stamp << "  " << msg << "\n";
    }
    // Also fprintf to stderr in case the operator's running from terminal
    fprintf(stderr, "[BotService] %s\n", msg.toUtf8().constData());
}

namespace fincept::services::bot {

// ─── Metatype registration ────────────────────────────────────────────────
void BotService::register_metatypes() {
    qRegisterMetaType<BotAccount>("BotAccount");
    qRegisterMetaType<BotPosition>("BotPosition");
    qRegisterMetaType<QVector<BotPosition>>("QVector<BotPosition>");
    qRegisterMetaType<BotTrade>("BotTrade");
    qRegisterMetaType<BotTradesToday>("BotTradesToday");
    qRegisterMetaType<BotWatchlistItem>("BotWatchlistItem");
    qRegisterMetaType<QVector<BotWatchlistItem>>("QVector<BotWatchlistItem>");
    qRegisterMetaType<BotNewsItem>("BotNewsItem");
    qRegisterMetaType<QVector<BotNewsItem>>("QVector<BotNewsItem>");
    qRegisterMetaType<BotGateBlock>("BotGateBlock");
    qRegisterMetaType<QVector<BotGateBlock>>("QVector<BotGateBlock>");
    qRegisterMetaType<BotRiskState>("BotRiskState");
    qRegisterMetaType<BotMarketState>("BotMarketState");
    qRegisterMetaType<MarketContextItem>("MarketContextItem");
}

// ─── Singleton + lifecycle ────────────────────────────────────────────────
BotService& BotService::instance() {
    static BotService svc;
    return svc;
}

BotService::BotService() : QObject(nullptr) {}

void BotService::initialize() {
    if (initialized_) return;
    initialized_ = true;
    register_metatypes();

    const auto& cfg = BotConfig::instance();
    bot_diag(QString("init — data_path=%1 alpaca=%2 paper=%3 status_json=%4 news_dir=%5")
                .arg(cfg.data_path())
                .arg(cfg.is_configured() ? "yes" : "no")
                .arg(cfg.alpaca_paper() ? "yes" : "no")
                .arg(cfg.status_json_path())
                .arg(cfg.news_dir_path()));
    bot_diag(QString("status_json exists=%1  news_dir exists=%2")
                .arg(QFile::exists(cfg.status_json_path()) ? "YES" : "NO")
                .arg(QDir(cfg.news_dir_path()).exists() ? "YES" : "NO"));

    // File watcher — pushes refresh on grok_status.json mutation so risk/
    // market/account topics update faster than the scheduler tick when the
    // bot writes a hot status (halt-state change, gate-block tick, etc.)
    file_watcher_ = new QFileSystemWatcher(this);
    if (QFile::exists(cfg.status_json_path())) {
        file_watcher_->addPath(cfg.status_json_path());
    }
    if (QDir(cfg.news_dir_path()).exists()) {
        file_watcher_->addPath(cfg.news_dir_path());
    }
    connect(file_watcher_, &QFileSystemWatcher::fileChanged,
            this, &BotService::on_status_file_changed);
    connect(file_watcher_, &QFileSystemWatcher::directoryChanged,
            this, &BotService::on_news_dir_changed);
}

// ─── Producer interface ───────────────────────────────────────────────────
//
// We claim ownership of BOTH the bot:* topics AND the existing widget-
// consumer topics (news:general, etc.) — required so the hub scheduler
// fires our refresh() when a widget subscribes to those topics. Without
// claiming the consumer topic, nothing schedules our publisher, and
// subscribers see only the stale CacheManager value from the prior
// session (old RSS news in the operator's screenshot).
QStringList BotService::topic_patterns() const {
    return {
        // Native bot topics — for any custom widget that wants the rich struct
        topics::kAccount,
        topics::kPositions,
        topics::kTradesToday,
        topics::kWatchlist,
        topics::kNews,
        topics::kRisk,
        topics::kMarket,
        // Bridges to existing widget topics — same refresh handler publishes
        // to BOTH the bot:* topic AND the widget-shaped variant. RSS-side
        // producers for these have been disabled in main.cpp.
        QStringLiteral("news:general"),
    };
}

void BotService::refresh(const QStringList& topics) {
    // Dedup by handler — multiple consumer aliases share one refresh call.
    bool did_news = false;
    for (const QString& t : topics) {
        if      (t == topics::kAccount)                     refresh_account();
        else if (t == topics::kPositions)                   refresh_positions();
        else if (t == topics::kTradesToday)                 refresh_trades_today();
        else if (t == topics::kWatchlist)                   refresh_watchlist();
        else if (t == topics::kRisk)                        refresh_risk();
        else if (t == topics::kMarket)                      refresh_market();
        else if ((t == topics::kNews || t == "news:general") && !did_news) {
            refresh_news();   // publishes to BOTH bot:news AND news:general
            did_news = true;
        }
    }
}

// ─── File watcher slots ───────────────────────────────────────────────────
void BotService::on_status_file_changed(const QString& /*path*/) {
    // Hot-fire all status-derived topics on mutation. Bot rewrites the
    // file ~1Hz so this is bounded throughput.
    refresh_account();
    refresh_risk();
    refresh_market();
    // Re-add the path — QFileSystemWatcher drops it when the file is
    // replaced (atomic write pattern: tmp + rename).
    if (file_watcher_ && QFile::exists(BotConfig::instance().status_json_path())) {
        file_watcher_->addPath(BotConfig::instance().status_json_path());
    }
}

void BotService::on_news_dir_changed(const QString& /*path*/) {
    // News jsonl file rotates at midnight ET; new file appears in the dir.
    // Reset our tail position when the file path changes.
    refresh_news();
}

// ─── Disk readers ─────────────────────────────────────────────────────────
QJsonObject BotService::read_status_json() const {
    QFile f(BotConfig::instance().status_json_path());
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(botSvc) << "status_json parse error:" << err.errorString();
        return {};
    }
    return doc.object();
}

QString BotService::today_news_jsonl_path() const {
    // Bot rotates at midnight ET. Use ET clock — fall back to local if zone unavailable.
    auto et = QTimeZone("America/New_York");
    auto now = et.isValid()
        ? QDateTime::currentDateTimeUtc().toTimeZone(et)
        : QDateTime::currentDateTime();
    return BotConfig::instance().news_dir_path()
         + "/" + now.toString("yyyy-MM-dd") + ".jsonl";
}

QString BotService::lane_log_path() const {
    // Bot rotates weekly (archive_claude_alpha_log.sh, Saturday 02:00 local).
    // Within a trading week claude_alpha.log accumulates everything so a
    // single tail position is correct for the whole week.
    return BotConfig::instance().data_path() + "/claude_alpha.log";
}

// ── Lane-filter helpers ─────────────────────────────────────────────────────
//
// READ-ONLY tail of claude_alpha.log to learn each headline's lane assignment
// (1 = IMMEDIATE, 2 = BATCH, 3 = DROP). The bot writes these lines as part of
// its normal stdout logging — Pinpunch never modifies the file. Zero impact
// on bot decision latency because the bot's classifier already ran and logged
// the decision before we ever touch the file.

QString BotService::normalize_headline_for_match(const QString& headline) {
    QString s = headline;
    // Common Benzinga HTML-entity escapes we see in the raw JSONL AND the log.
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    // Collapse whitespace runs to single spaces, lowercase, trim.
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    s = s.replace(ws, QStringLiteral(" ")).trimmed().toLower();
    if (s.size() > kHeadlineMatchPrefixLen)
        s = s.left(kHeadlineMatchPrefixLen);
    return s;
}

void BotService::ingest_lane_log() {
    const QString path = lane_log_path();

    // Detect rotation (path change or file shrink → reset state).
    if (path != lane_log_path_cached_) {
        lane_log_path_cached_ = path;
        lane_log_pos_ = 0;
        news_lane_by_key_.clear();
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // No log yet — totally normal on a fresh machine. Bot may not have
        // started, or it may write logs elsewhere. Without log → no lane data
        // → refresh_news() falls back to "show all" so the widget isn't empty.
        return;
    }
    if (lane_log_pos_ > f.size()) {
        // File truncated (archive script ran, or operator wiped it). Re-tail
        // from start and rebuild the map.
        lane_log_pos_ = 0;
        news_lane_by_key_.clear();
    }
    f.seek(lane_log_pos_);

    // Match: "📡 WS-Macro [LANE 1 IMMEDIATE] (reason): headline..."
    //                    capture-1                    capture-2
    // The reason group can contain parentheses (e.g. "tier-a-name-resolved:LULU(lululemon), queue=1/10")
    // so we anchor on "] (" + first ")" before ": ".
    static const QRegularExpression re(
        QStringLiteral("WS-Macro \\[LANE ([123]) [A-Z]+\\] \\([^\\n]+?\\): (.+)$"));

    int added = 0;
    while (!f.atEnd()) {
        const QByteArray raw = f.readLine();
        if (raw.isEmpty())
            break;
        const QString line = QString::fromUtf8(raw);
        auto m = re.match(line);
        if (!m.hasMatch())
            continue;
        const int lane = m.captured(1).toInt();
        const QString headline_in_log = m.captured(2).trimmed();
        if (headline_in_log.isEmpty())
            continue;
        const QString key = normalize_headline_for_match(headline_in_log);
        if (key.isEmpty())
            continue;
        news_lane_by_key_.insert(key, lane);
        ++added;
    }
    lane_log_pos_ = f.pos();

    // Cap the map — claude_alpha.log accumulates ~500 lane events/day, so
    // 5000 covers ~10 days of unique headlines. Beyond that, clear half
    // (oldest are unreachable anyway because tail position has moved).
    if (news_lane_by_key_.size() > kLaneMapMaxEntries) {
        bot_diag(QString("lane map at %1 entries — clearing for fresh window")
                     .arg(news_lane_by_key_.size()));
        news_lane_by_key_.clear();
        // Tail position stays put — future lines re-populate. Items already
        // in the JSONL buffer may temporarily show as "unknown lane" until
        // the next ingest cycle re-reads from current pos forward, which is
        // empty until new log lines arrive. Acceptable: this only triggers
        // on multi-week-old data.
    }

    if (added > 0)
        bot_diag(QString("ingest_lane_log: +%1 lane decisions (map size %2)")
                     .arg(added).arg(news_lane_by_key_.size()));
}

// ─── Topic refreshers ─────────────────────────────────────────────────────
//
// First-cut implementations: read what's available from the bot's disk
// files. Alpaca REST integration (account, positions, orders) lands in a
// follow-up — for now those topics use the status_json overlay where the
// bot publishes a recent snapshot.

void BotService::refresh_account() {
    // Source of truth: Alpaca REST /v2/account. The bot's status_json view
    // is a fallback for the rare case Alpaca is unreachable (network blip,
    // credential issue) — it lets the widget show stale-but-recent data
    // instead of a blank panel.
    AlpacaRestClient::instance().get_account(
        [](bool ok, const QJsonValue& v, const QString& err) {
            auto& hub = fincept::datahub::DataHub::instance();
            BotAccount a;
            if (ok && v.isObject()) {
                auto o = v.toObject();
                a.equity          = o.value("equity").toString().toDouble();
                a.last_equity     = o.value("last_equity").toString().toDouble();
                a.day_pnl         = a.equity - a.last_equity;
                a.day_pct         = (a.last_equity > 0) ? (a.day_pnl / a.last_equity * 100.0) : 0.0;
                a.cash            = o.value("cash").toString().toDouble();
                a.buying_power    = o.value("buying_power").toString().toDouble();
                a.trading_blocked = o.value("trading_blocked").toBool();
                a.is_connected    = true;
                a.ts              = QDateTime::currentSecsSinceEpoch();
                bot_diag(QString("refresh_account [Alpaca] equity=%1 day_pnl=%2 pct=%3%")
                            .arg(a.equity, 0, 'f', 2)
                            .arg(a.day_pnl, 0, 'f', 2)
                            .arg(a.day_pct, 0, 'f', 2));
            } else {
                // Fallback: read bot's status JSON snapshot
                bot_diag(QString("refresh_account [Alpaca FAIL] %1 — falling back to status_json").arg(err));
                auto s = BotService::instance().read_status_json();
                a.equity          = s.value("account_size").toDouble();
                a.day_pnl         = s.value("daily_pnl").toDouble();
                a.last_equity     = a.equity - a.day_pnl;
                a.day_pct         = (a.last_equity > 0) ? (a.day_pnl / a.last_equity * 100.0) : 0.0;
                a.is_connected    = !s.isEmpty();
                a.ts              = QDateTime::currentSecsSinceEpoch();
            }
            hub.publish(QString::fromUtf8(topics::kAccount), QVariant::fromValue(a));
        });
}

void BotService::refresh_positions() {
    // Source of truth: Alpaca REST /v2/positions. Returns a JSON array
    // with broker-computed P&L per position (incl. fees, sign-corrected
    // for shorts). The bot's status_json positions field is a fallback
    // for unreachable-Alpaca cases.
    AlpacaRestClient::instance().get_positions(
        [](bool ok, const QJsonValue& v, const QString& err) {
            auto& hub = fincept::datahub::DataHub::instance();
            QVector<BotPosition> out;
            if (ok && v.isArray()) {
                for (const auto& el : v.toArray()) {
                    auto o = el.toObject();
                    BotPosition p;
                    p.symbol           = o.value("symbol").toString().toUpper();
                    const double qty   = o.value("qty").toString().toDouble();
                    p.side             = (qty < 0) ? "short" : "long";
                    p.qty              = std::abs(qty);
                    p.avg_entry_price  = o.value("avg_entry_price").toString().toDouble();
                    p.current_price    = o.value("current_price").toString().toDouble();
                    p.market_value     = o.value("market_value").toString().toDouble();
                    p.cost_basis       = o.value("cost_basis").toString().toDouble();
                    p.unrealized_pl    = o.value("unrealized_pl").toString().toDouble();
                    p.unrealized_plpc  = o.value("unrealized_plpc").toString().toDouble() * 100.0;
                    if (!p.symbol.isEmpty()) out.append(p);
                }
                bot_diag(QString("refresh_positions [Alpaca] published %1 positions").arg(out.size()));
            } else {
                bot_diag(QString("refresh_positions [Alpaca FAIL] %1 — falling back to status_json").arg(err));
                auto s = BotService::instance().read_status_json();
                auto pos_obj = s.value("positions").toObject();
                for (auto it = pos_obj.constBegin(); it != pos_obj.constEnd(); ++it) {
                    auto vv = it.value().toObject();
                    BotPosition p;
                    p.symbol           = it.key().toUpper();
                    p.qty              = vv.value("qty").toDouble();
                    p.side             = (p.qty < 0) ? "short" : "long";
                    p.qty              = std::abs(p.qty);
                    p.avg_entry_price  = vv.value("avg_entry_price").toDouble(vv.value("entry").toDouble());
                    p.current_price    = vv.value("current_price").toDouble(vv.value("price").toDouble());
                    p.unrealized_pl    = vv.value("unrealized_pl").toDouble(vv.value("pnl").toDouble());
                    p.unrealized_plpc  = vv.value("unrealized_plpc").toDouble();
                    if (!p.symbol.isEmpty()) out.append(p);
                }
            }
            hub.publish(QString::fromUtf8(topics::kPositions), QVariant::fromValue(out));
        });
}

void BotService::refresh_trades_today() {
    auto& hub = fincept::datahub::DataHub::instance();
    BotTradesToday t;
    auto s = read_status_json();
    t.trades_total  = s.value("daily_trades").toInt();
    t.trades_closed = t.trades_total;       // Alpaca REST will refine this
    t.trades_open   = 0;
    t.realized_pnl  = s.value("daily_pnl").toDouble();
    // wins / losses / win_rate / per-trade detail come from Alpaca REST in
    // the follow-up — the bot's status JSON has aggregate counters only.
    hub.publish(QString::fromUtf8(topics::kTradesToday), QVariant::fromValue(t));
}

void BotService::refresh_watchlist() {
    // Source of symbols: bot's LIVE state.watchlist field (in grok_status.json),
    // which the bot updates every ~1s with intraday adds/drops. Each entry
    // also has the bot's metadata — score, direction, catalyst, strategy.
    //
    // Source of prices: Alpaca /v2/stocks/snapshots — independent broker
    // truth, latest trade + prev close → % change.
    //
    // Falls back to watchlist_tomorrow.json if state.watchlist is empty
    // (e.g., dashboard launched before bot has populated overnight prep).
    auto& cfg_self = BotService::instance();
    auto s = cfg_self.read_status_json();

    QStringList symbols;
    QHash<QString, QJsonObject> meta_by_sym;
    auto wl_json = s.value("watchlist");
    if (wl_json.isArray()) {
        for (const auto& v : wl_json.toArray()) {
            QString sym;
            QJsonObject meta;
            if (v.isString()) {
                sym = v.toString().toUpper();
            } else if (v.isObject()) {
                meta = v.toObject();
                sym = meta.value("ticker").toString(meta.value("symbol").toString()).toUpper();
            }
            if (!sym.isEmpty() && !meta_by_sym.contains(sym)) {
                symbols.append(sym);
                meta_by_sym.insert(sym, meta);
            }
        }
    }

    // Fallback: read watchlist_tomorrow.json (overnight-curated candidates)
    if (symbols.isEmpty()) {
        QFile tom(BotConfig::instance().watchlist_tomorrow_path());
        if (tom.open(QIODevice::ReadOnly)) {
            auto doc = QJsonDocument::fromJson(tom.readAll());
            if (doc.isObject()) {
                for (const auto& v : doc.object().value("candidates").toArray()) {
                    auto o = v.toObject();
                    QString sym = o.value("ticker").toString().toUpper();
                    if (!sym.isEmpty() && !meta_by_sym.contains(sym)) {
                        symbols.append(sym);
                        meta_by_sym.insert(sym, o);
                    }
                }
            }
        }
    }

    if (symbols.isEmpty()) {
        bot_diag("refresh_watchlist — no symbols (bot not running or pre-RTH)");
        fincept::datahub::DataHub::instance().publish(
            QString::fromUtf8(topics::kWatchlist),
            QVariant::fromValue(QVector<BotWatchlistItem>{}));
        return;
    }

    // Enrich with latest snapshots from Alpaca (live price + prev-close).
    // Lambda captures `symbols` and `meta_by_sym` by value — both QHash and
    // QStringList are copy-on-write so the cost is negligible.
    AlpacaRestClient::instance().get_snapshots(symbols,
        [symbols, meta_by_sym](bool ok, const QJsonValue& v, const QString& /*err*/) {
            QVector<BotWatchlistItem> out;
            QJsonObject snap = (ok && v.isObject()) ? v.toObject() : QJsonObject{};
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            for (const QString& sym : symbols) {
                BotWatchlistItem w;
                w.symbol = sym;
                // Bot's metadata for this symbol (catalyst, score, etc.)
                if (meta_by_sym.contains(sym)) {
                    auto m = meta_by_sym.value(sym);
                    w.score      = m.value("score").toDouble(m.value("ai_score").toDouble());
                    w.direction  = m.value("direction").toString(m.value("ai_direction").toString("long"));
                    w.catalyst   = m.value("catalyst").toString(m.value("reason").toString());
                    w.strategy   = m.value("strategy").toString();
                    w.subscribed = m.value("subscribed").toBool();
                }
                // Alpaca snapshot enrichment
                if (snap.contains(sym)) {
                    auto so = snap.value(sym).toObject();
                    auto lt = so.value("latestTrade").toObject();
                    auto pdb = so.value("prevDailyBar").toObject();
                    auto db = so.value("dailyBar").toObject();
                    w.price      = lt.value("p").toDouble();
                    w.prev_close = pdb.value("c").toDouble(db.value("o").toDouble());
                    if (w.price > 0 && w.prev_close > 0) {
                        w.change_pct = (w.price - w.prev_close) / w.prev_close * 100.0;
                    }
                }
                w.ts = now;
                out.append(w);
            }
            bot_diag(QString("refresh_watchlist — %1 symbols, %2 with prices (Alpaca snap_ok=%3)")
                        .arg(symbols.size()).arg(snap.size()).arg(ok ? "yes" : "no"));
            fincept::datahub::DataHub::instance().publish(
                QString::fromUtf8(topics::kWatchlist),
                QVariant::fromValue(out));
        });
}

void BotService::refresh_news() {
    auto& hub = fincept::datahub::DataHub::instance();
    QString path = today_news_jsonl_path();

    // Detect file rotation at midnight ET — reset tail position.
    if (path != news_file_path_cached_) {
        news_file_path_cached_ = path;
        news_file_pos_ = 0;
        news_buffer_.clear();
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // No file yet today — publish empty buffer
        hub.publish(QString::fromUtf8(topics::kNews), QVariant::fromValue(news_buffer_));
        return;
    }
    if (news_file_pos_ > f.size()) {
        // File shrank (rotated mid-tick) — re-tail from start
        news_file_pos_ = 0;
        news_buffer_.clear();
    }
    bot_diag(QString("refresh_news — reading %1 from pos %2 size %3")
                .arg(path).arg(news_file_pos_).arg(f.size()));
    f.seek(news_file_pos_);
    while (!f.atEnd()) {
        QByteArray line = f.readLine();
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError err;
        auto doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        auto o = doc.object();

        BotNewsItem n;
        n.id            = QString::number((qint64)o.value("news_id").toDouble());
        n.headline      = o.value("headline").toString().left(500);
        n.body          = o.value("body").toString().left(500);
        n.source        = o.value("source").toString("benzinga");
        n.published_ms  = (qint64)o.value("published").toDouble();
        n.received_ts   = (qint64)o.value("ts").toDouble();
        for (auto t : o.value("tickers").toArray()) {
            n.tickers.append(t.toString().toUpper());
        }
        news_buffer_.append(n);
    }
    news_file_pos_ = f.pos();

    // Cap buffer size — keep most recent N
    if (news_buffer_.size() > kNewsBufferMax) {
        news_buffer_.remove(0, news_buffer_.size() - kNewsBufferMax);
    }
    hub.publish(QString::fromUtf8(topics::kNews), QVariant::fromValue(news_buffer_));

    // ── Pull latest LANE classifications from claude_alpha.log ──────────
    // READ-ONLY tail; the bot's classifier already ran. We just learn what
    // it decided so we can drop Lane 3 noise before bridging to the widget.
    ingest_lane_log();

    // ── Bridge to existing NewsWidget via `news:general` ────────────────
    // Map our BotNewsItem schema to Pinpunch's NewsArticle so the existing
    // dashboard widget (and anything else subscribed to news:general)
    // shows bot data with zero per-widget changes. RSS NewsService is
    // disabled in main.cpp so we're the sole publisher to this topic.
    //
    // Lane filter: only emit Lane 1 (IMMEDIATE catalysts) + Lane 2 (BATCH
    // name-resolved). Lane 3 (DROP / tier-a-no-ticker noise) is suppressed.
    // Items the bot hasn't classified yet — or items that arrived before
    // the log tailer caught up — are NOT shown, so the widget stays clean.
    // Fallback: if the lane map is completely empty (claude_alpha.log not
    // available, e.g. bot never started on this machine), show everything
    // so the widget isn't blank — better to fall open than fall closed.
    //
    // Iteration order matters: NewsWidget renders the QVector in array
    // order (top to bottom). We walk the buffer NEWEST→OLDEST so the
    // freshest item lands at the top of the widget. Buffer itself is
    // appended chronologically (oldest first) by the file-tail reader.
    const bool lane_map_available = !news_lane_by_key_.isEmpty();
    QVector<fincept::services::NewsArticle> bridged;
    bridged.reserve(news_buffer_.size());
    int dropped_l3 = 0;
    int dropped_unknown = 0;
    for (auto it = news_buffer_.rbegin(); it != news_buffer_.rend(); ++it) {
        const BotNewsItem& n = *it;

        // Apply lane filter unless the map is empty (fail-open mode).
        if (lane_map_available) {
            const QString key = normalize_headline_for_match(n.headline);
            const int lane = news_lane_by_key_.value(key, 0); // 0 = unknown
            if (lane == 3) { ++dropped_l3; continue; }
            if (lane == 0) { ++dropped_unknown; continue; }
            // lane 1 or 2 → fall through and emit
        }

        fincept::services::NewsArticle a;
        a.id        = n.id;
        a.headline  = n.headline;
        a.summary   = n.body;
        a.source    = n.source.isEmpty() ? QStringLiteral("benzinga") : n.source;
        a.tickers   = n.tickers;
        a.tier      = 1;   // wire-grade — Benzinga is institutional feed
        a.region    = QStringLiteral("US");
        a.lang      = QStringLiteral("en");
        a.category  = QStringLiteral("general");
        a.sort_ts   = n.published_ms > 0 ? (n.published_ms / 1000) : n.received_ts;
        a.time      = QDateTime::fromSecsSinceEpoch(a.sort_ts).toString("hh:mm:ss");
        // Sentiment from polarity sign if news_scorer ran on this headline.
        // Otherwise NEUTRAL.
        if (n.has_polarity) {
            if      (n.polarity >  0.15) a.sentiment = fincept::services::Sentiment::BULLISH;
            else if (n.polarity < -0.15) a.sentiment = fincept::services::Sentiment::BEARISH;
            else                          a.sentiment = fincept::services::Sentiment::NEUTRAL;
            // Magnitude → priority: high-mag = BREAKING, low = ROUTINE
            if      (n.magnitude >= 0.85) a.priority = fincept::services::Priority::BREAKING;
            else if (n.magnitude >= 0.60) a.priority = fincept::services::Priority::URGENT;
            else                           a.priority = fincept::services::Priority::ROUTINE;
        }
        bridged.append(a);
    }
    bot_diag(QString("published %1 news items to news:general "
                     "(buffer %2, lane-filter: dropped %3 L3 + %4 unknown, lane_map=%5)")
                .arg(bridged.size())
                .arg(news_buffer_.size())
                .arg(dropped_l3)
                .arg(dropped_unknown)
                .arg(lane_map_available ? "ACTIVE" : "EMPTY-FALLOPEN"));
    hub.publish(QStringLiteral("news:general"), QVariant::fromValue(bridged));
}

void BotService::refresh_risk() {
    auto& hub = fincept::datahub::DataHub::instance();
    auto s = read_status_json();

    BotRiskState r;
    r.drawdown_tier      = s.value("drawdown_tier").toString("green");
    r.trading_halted     = s.value("trading_halted").toBool();
    r.halted_by          = s.value("halted_by").toString();
    r.soft_blocked_count = s.value("soft_blocked_count").toInt();
    for (auto v : s.value("soft_blocked_top").toArray()) {
        if (v.isString()) r.soft_blocked_top.append(v.toString());
        else if (v.isArray()) {
            auto pair = v.toArray();
            if (pair.size() >= 1) r.soft_blocked_top.append(pair.at(0).toString());
        }
    }
    auto gb = s.value("gate_blocks_today").toObject();
    for (auto it = gb.constBegin(); it != gb.constEnd(); ++it) {
        BotGateBlock g;
        g.gate     = it.key();
        if (it.value().isObject()) {
            auto o = it.value().toObject();
            g.window   = o.value("window").toInt();
            g.distinct = o.value("distinct").toInt();
            g.ts       = (qint64)o.value("ts").toDouble();
        } else {
            g.distinct = it.value().toInt();
        }
        r.gate_blocks.append(g);
    }
    r.ts = QDateTime::currentSecsSinceEpoch();
    hub.publish(QString::fromUtf8(topics::kRisk), QVariant::fromValue(r));
}

void BotService::refresh_market() {
    auto& hub = fincept::datahub::DataHub::instance();
    auto s = read_status_json();

    BotMarketState m;
    m.regime    = s.value("regime").toString("unknown");
    m.vol_env   = s.value("vol_env").toString("normal");
    m.spy_price = s.value("spy_price").toDouble();
    m.spy_pct   = s.value("spy_pct").toDouble();
    m.why_text  = s.value("why_text").toString();
    m.why_ts    = (qint64)s.value("why_ts").toDouble();
    m.ts        = QDateTime::currentSecsSinceEpoch();
    hub.publish(QString::fromUtf8(topics::kMarket), QVariant::fromValue(m));
}

} // namespace fincept::services::bot
