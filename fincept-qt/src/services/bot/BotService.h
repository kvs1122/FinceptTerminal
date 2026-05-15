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

    bool initialized_ = false;
    QFileSystemWatcher* file_watcher_ = nullptr;

    // Cached news jsonl tail position so we only read new entries.
    qint64 news_file_pos_ = 0;
    QString news_file_path_cached_;
    QVector<BotNewsItem> news_buffer_;   // last N entries
    static constexpr int kNewsBufferMax = 100;
};

} // namespace fincept::services::bot
