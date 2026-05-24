#pragma once

// ─── BotService: DataHub Producer for grok-claude bot data ────────────────
//
// Owns the bot:* topic family. On each refresh tick from the hub scheduler,
// dispatches to the appropriate disk reader (grok_status.json, trades.csv,
// alpaca_news_raw/*.jsonl) or Alpaca REST call (account, positions, orders),
// then publishes typed structs back to the hub.
//
// Lifetime: singleton — `BotService::instance()`. Registered with the hub
// once in main.cpp during app startup.
//
// Threading: refresh() is called on the Qt main thread; disk reads are fast
// (<5 ms) and Alpaca REST calls go through Pinpunch's HttpClient which
// fires async network ops on QNetworkAccessManager's internal thread. So
// no blocking on the main thread.

#include "datahub/Producer.h"
#include "services/bot/BotTypes.h"

#include <QObject>
#include <QStringList>
#include <QFileSystemWatcher>

namespace fincept::services::bot {

class BotService : public QObject, public fincept::datahub::Producer {
    Q_OBJECT
  public:
    static BotService& instance();

    /// Call ONCE at app startup, before any subscribers register. Sets up
    /// metatype registration, file watcher, optional Alpaca REST client.
    /// Idempotent — safe to call multiple times.
    void initialize();

    /// Producer interface — called by DataHub.
    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override { return 10; }

    /// Public for fallback callers — reads grok_status.json. Returns {} on any error.
    QJsonObject read_status_json() const;

  private slots:
    void on_status_file_changed(const QString& path);
    void on_news_dir_changed(const QString& path);

  private:
    BotService();
    ~BotService() override = default;
    BotService(const BotService&) = delete;
    BotService& operator=(const BotService&) = delete;

    // Topic-specific refresh implementations (each publishes to hub on success)
    void refresh_account();
    void refresh_positions();
    void refresh_trades_today();
    void refresh_watchlist();
    void refresh_news();
    void refresh_risk();
    void refresh_market();

    // Helpers
    static void register_metatypes();
    QString     today_news_jsonl_path() const;     // logs/alpaca_news_raw/YYYY-MM-DD.jsonl
    QString     lane_log_path() const;             // claude_alpha.log (bot stdout log)

    /// Tail claude_alpha.log from `lane_log_pos_` forward, parse any new
    /// "📡 WS-Macro [LANE N ...] (...): headline" lines, and record the
    /// {normalized-headline-prefix → lane-number} mapping into
    /// `news_lane_by_key_`. Strictly read-only — never touches the bot.
    /// Called from refresh_news() right before the JSONL → news:general bridge.
    void ingest_lane_log();

    /// Normalize a headline for log↔jsonl matching.
    /// - lowercase
    /// - HTML-decode common Benzinga entities (&#39; → ', &amp; → &, &quot; → ", &lt;/gt;)
    /// - collapse all whitespace runs to a single space
    /// - trim
    /// - take first kHeadlineMatchPrefixLen chars
    /// The log truncates headlines at ~80 chars; we use a shorter prefix to
    /// give a margin against truncation midword.
    static QString normalize_headline_for_match(const QString& headline);

    bool initialized_ = false;
    QFileSystemWatcher* file_watcher_ = nullptr;

    // Cached news jsonl tail position so we only read new entries.
    qint64 news_file_pos_ = 0;
    QString news_file_path_cached_;
    QVector<BotNewsItem> news_buffer_;   // last N entries
    static constexpr int kNewsBufferMax = 100;

    // ─── Lane-filter state (claude_alpha.log tailer) ─────────────────────
    // Populated by ingest_lane_log(); consumed by refresh_news() when
    // building the bridged news:general payload. Lane 1 = IMMEDIATE,
    // Lane 2 = BATCH (both shown), Lane 3 = DROP (filtered out).
    qint64  lane_log_pos_ = 0;
    QString lane_log_path_cached_;
    QHash<QString, int> news_lane_by_key_;
    static constexpr int kHeadlineMatchPrefixLen = 50;
    static constexpr int kLaneMapMaxEntries      = 5000;
};

} // namespace fincept::services::bot
