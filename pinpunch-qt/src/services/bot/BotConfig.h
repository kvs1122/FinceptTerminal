#pragma once

// ─── BotService configuration ──────────────────────────────────────────────
// Reads from environment variables at startup, falls back to sensible
// defaults. All values overridable via Settings panel later (TODO).
//
// Env vars consulted:
//   PINPUNCH_BOT_DATA_PATH   default ~/grok-claude
//                            location of grok_status.json, trades.csv, logs/
//   PINPUNCH_ALPACA_KEY      Alpaca API key (Pinpunch can use a separate
//                            READ-ONLY key from the bot's; safer)
//                            falls back to ALPACA_API_KEY
//   PINPUNCH_ALPACA_SECRET   Alpaca API secret
//                            falls back to ALPACA_SECRET / ALPACA_SECRET_KEY
//   PINPUNCH_ALPACA_PAPER    "true" (default) or "false" — paper vs live
//                            falls back to ALPACA_PAPER

#include <QDir>
#include <QFile>
#include <QHash>
#include <QString>
#include <QTextStream>

namespace fincept::services::bot {

class BotConfig {
  public:
    static const BotConfig& instance() {
        static BotConfig cfg;
        return cfg;
    }

    QString data_path() const { return data_path_; }
    QString status_json_path() const { return data_path_ + "/grok_status.json"; }
    QString trades_csv_path() const { return data_path_ + "/trades.csv"; }
    QString news_dir_path() const { return data_path_ + "/logs/alpaca_news_raw"; }
    QString watchlist_tomorrow_path() const { return data_path_ + "/watchlist_tomorrow.json"; }

    QString alpaca_key() const { return alpaca_key_; }
    QString alpaca_secret() const { return alpaca_secret_; }
    bool    alpaca_paper() const { return alpaca_paper_; }

    /// Financial Modeling Prep (https://financialmodelingprep.com) — paid
    /// only since Aug 2025. Kept here for users who have a legacy free key.
    QString fmp_key() const { return fmp_key_; }

    /// Finnhub (https://finnhub.io) — free 60-call/min tier covers
    /// `/calendar/economic` for upcoming PPI/CPI/FOMC/NFP/etc. Bot
    /// already uses this key for news scoring; we share it for the
    /// economic calendar producer.
    QString finnhub_key() const { return finnhub_key_; }

    /// Polygon (https://polygon.io) — Stocks Developer plan. When set,
    /// US stocks/ETFs/ADRs (e.g. AAPL, SPY, BABA) route through Polygon's
    /// /v3/snapshot REST endpoint at 1s cadence (sub-second freshness)
    /// instead of Alpaca's snapshot endpoint. We deliberately do NOT use
    /// Polygon's WebSocket — the bot's predictive layer reserves that
    /// connection (see grok-claude/predictive/polygon_ws.py).
    QString polygon_key() const { return polygon_key_; }

    QString alpaca_base_url() const {
        return alpaca_paper_
            ? QStringLiteral("https://paper-api.alpaca.markets")
            : QStringLiteral("https://api.alpaca.markets");
    }

    bool is_configured() const { return !alpaca_key_.isEmpty() && !alpaca_secret_.isEmpty(); }

  private:
    BotConfig() {
        // ── Bot data path ────────────────────────────────────────────────
        QByteArray dp = qgetenv("PINPUNCH_BOT_DATA_PATH");
        if (dp.isEmpty()) {
            data_path_ = QDir::homePath() + "/grok-claude";
        } else {
            data_path_ = QString::fromUtf8(dp);
        }

        // ── Parse <data_path>/.env as a fallback source ──────────────────
        // macOS apps launched via Finder / `open` DON'T inherit shell env
        // vars. So if the operator has ALPACA_API_KEY in their bot's .env
        // file (the typical setup), Pinpunch can't see it from process env.
        // Read the .env file directly so Pinpunch picks up the same creds
        // the bot uses, with zero per-OS-launcher config.
        QHash<QString, QString> dotenv = parse_env_file(data_path_ + "/.env");

        // Resolution order for each var:
        //   1. process env (PINPUNCH_*)
        //   2. process env (legacy ALPACA_*)
        //   3. .env file (PINPUNCH_*)
        //   4. .env file (legacy ALPACA_*)
        alpaca_key_    = first_nonempty({
            QString::fromUtf8(qgetenv("PINPUNCH_ALPACA_KEY")),
            QString::fromUtf8(qgetenv("ALPACA_API_KEY")),
            dotenv.value("PINPUNCH_ALPACA_KEY"),
            dotenv.value("ALPACA_API_KEY"),
        });
        alpaca_secret_ = first_nonempty({
            QString::fromUtf8(qgetenv("PINPUNCH_ALPACA_SECRET")),
            QString::fromUtf8(qgetenv("ALPACA_SECRET_KEY")),
            QString::fromUtf8(qgetenv("ALPACA_SECRET")),
            dotenv.value("PINPUNCH_ALPACA_SECRET"),
            dotenv.value("ALPACA_SECRET_KEY"),
            dotenv.value("ALPACA_SECRET"),
        });

        QString paper_str = first_nonempty({
            QString::fromUtf8(qgetenv("PINPUNCH_ALPACA_PAPER")),
            QString::fromUtf8(qgetenv("ALPACA_PAPER")),
            dotenv.value("PINPUNCH_ALPACA_PAPER"),
            dotenv.value("ALPACA_PAPER"),
        }).toLower().trimmed();
        alpaca_paper_ = (paper_str != "false" && paper_str != "0"
                       && paper_str != "no"   && paper_str != "off");

        // FMP API key (paid only since Aug 2025; legacy free keys still accepted)
        fmp_key_ = first_nonempty({
            QString::fromUtf8(qgetenv("FMP_API_KEY")),
            QString::fromUtf8(qgetenv("FINANCIAL_MODELING_PREP_KEY")),
            dotenv.value("FMP_API_KEY"),
            dotenv.value("FINANCIAL_MODELING_PREP_KEY"),
        });
        // Finnhub API key (free /calendar/economic — primary calendar source).
        finnhub_key_ = first_nonempty({
            QString::fromUtf8(qgetenv("FINNHUB_API_KEY")),
            QString::fromUtf8(qgetenv("FINNHUB_TOKEN")),
            dotenv.value("FINNHUB_API_KEY"),
            dotenv.value("FINNHUB_TOKEN"),
        });
        // Polygon Stocks Dev API key — bot already uses POLYGON_API_KEY for
        // its REST trade/NBBO fetches, so we share the same env name and
        // .env entry. When unset, BotIndicesService transparently falls
        // back to Alpaca for US stocks/ETFs.
        polygon_key_ = first_nonempty({
            QString::fromUtf8(qgetenv("PINPUNCH_POLYGON_KEY")),
            QString::fromUtf8(qgetenv("POLYGON_API_KEY")),
            dotenv.value("PINPUNCH_POLYGON_KEY"),
            dotenv.value("POLYGON_API_KEY"),
        });
    }

    static QString first_nonempty(std::initializer_list<QString> candidates) {
        for (const QString& s : candidates) if (!s.isEmpty()) return s;
        return QString();
    }

    /// Minimal .env parser — KEY=value per line, ignores blanks + #comments,
    /// strips surrounding quotes. Doesn't support multi-line values.
    static QHash<QString, QString> parse_env_file(const QString& path) {
        QHash<QString, QString> out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            QString key = line.left(eq).trimmed();
            QString val = line.mid(eq + 1).trimmed();
            // Strip wrapping quotes if any
            if (val.size() >= 2
                && ((val.startsWith('"') && val.endsWith('"'))
                 || (val.startsWith('\'') && val.endsWith('\'')))) {
                val = val.mid(1, val.size() - 2);
            }
            // Drop trailing inline comment if unquoted
            int hash = val.indexOf(" #");
            if (hash >= 0) val = val.left(hash).trimmed();
            out.insert(key, val);
        }
        return out;
    }

    QString data_path_;
    QString alpaca_key_;
    QString alpaca_secret_;
    bool    alpaca_paper_ = true;
    QString fmp_key_;
    QString finnhub_key_;
    QString polygon_key_;
};

// ─── Topic name constants (the public DataHub contract) ───────────────────
namespace topics {
    inline constexpr const char* kAccount        = "bot:account";
    inline constexpr const char* kPositions      = "bot:positions";
    inline constexpr const char* kTradesToday    = "bot:trades_today";
    inline constexpr const char* kWatchlist      = "bot:watchlist";
    inline constexpr const char* kNews           = "bot:news";
    inline constexpr const char* kRisk           = "bot:risk";
    inline constexpr const char* kMarket         = "bot:market";
    inline constexpr const char* kMarketContext  = "market:context";
}

} // namespace fincept::services::bot
