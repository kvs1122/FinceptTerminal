#pragma once

// ─── MarketContextService — hourly "why are markets moving" narrative ────
//
// Singleton DataHub producer for topic `market:context`. Fires once per
// hour during the active window (Sun 20:00 ET → Fri 20:00 ET, skipping
// the weekend gap). Each tick:
//
//   1. Reads the latest macro snapshots from the DataHub cache (^GSPC,
//      ^TNX, ^VIX, DX-Y.NYB, GC=F, CL=F, BTC-USD) — already populated by
//      BotIndicesService, so this is a free lookup.
//   2. Pulls the most recent ~6h of general market headlines from
//      Finnhub /news?category=general using the existing finnhub_key.
//   3. If LlmService is configured (Gemini recommended), composes a
//      prompt and asks for a 3–5 sentence narrative explaining the
//      dominant driver. The LLM doesn't need real-time web access — we
//      feed it the current data + headlines.
//   4. Falls back to raw headlines when no LLM is configured.
//   5. Publishes a MarketContextItem to `market:context`.
//   6. Caches to ~/grok-claude/.pinpunch_market_context.json so a
//      Pinpunch restart shows the last narrative immediately.
//
// We deliberately do NOT subscribe to any quote topics — we just READ
// the cache via DataHub::peek(). That keeps cadence decoupled: hourly
// narrative refresh is independent of per-symbol quote refresh.

#include "datahub/Producer.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>
#include <QTimer>

namespace fincept::services::bot {

class MarketContextService : public QObject, public fincept::datahub::Producer {
    Q_OBJECT
  public:
    static MarketContextService& instance();

    // Producer interface
    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override { return 1; }

    /// Manual trigger — bypasses the hourly cadence (still respects the
    /// weekly active-window gate). Wired to the widget's refresh button.
    Q_INVOKABLE void refresh_now();

  private:
    MarketContextService();
    ~MarketContextService() override = default;

    /// Decide whether to fire a refresh on the current wall-clock tick.
    /// True when within Sun 20:00 ET → Fri 20:00 ET, false on the
    /// weekend gap (Fri 20:00 → Sun 20:00). Computed against ET because
    /// "market hours" only meaningfully follow US sessions.
    static bool in_active_window(const QDateTime& now_utc);

    /// Wall-clock seconds until the next scheduled hourly tick (local
    /// timezone), or until the next Sunday 20:00 ET if we're currently
    /// in the weekend gap.
    static qint64 secs_until_next_tick(const QDateTime& now_utc);

    void start_fetch();
    void fetch_finnhub_news(std::function<void(QJsonArray articles, QString err)> cb);
    void publish_with_llm(const QStringList& macro_lines, const QJsonArray& articles);
    void publish_headlines_only(const QStringList& macro_lines, const QJsonArray& articles,
                                const QString& reason);
    void publish_off_hours();

    /// Direct Gemini /generateContent call. Used by publish_with_llm when
    /// the user has a saved `gemini` provider in LlmConfigRepository,
    /// regardless of which provider is currently "active". Returns
    /// (model_used, narrative_text, error). model_used / narrative_text
    /// are populated on success; error is populated on failure. Bypasses
    /// LlmService entirely so AI Chat can keep using a different model.
    struct GeminiResult { QString model; QString text; QString error; };
    GeminiResult gemini_direct(const QString& api_key, const QString& model,
                               const QString& base_url, const QString& prompt);

    void persist_to_cache(const struct MarketContextItem& item);
    void restore_from_cache_if_present();

    QNetworkAccessManager* nam_  = nullptr;
    QTimer*  hourly_timer_       = nullptr;
    bool     fetch_in_flight_    = false;
};

} // namespace fincept::services::bot
