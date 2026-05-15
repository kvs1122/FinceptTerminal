#pragma once

// ─── Economic Calendar Producer (FMP-backed) ──────────────────────────────
//
// Polls Financial Modeling Prep's free `/api/v3/economic_calendar` endpoint
// for upcoming macro events (PPI, CPI, FOMC, NFP, retail sales, GDP, …)
// and publishes a normalized QJsonArray to `econ:fincept:upcoming_events`.
//
// Pinpunch's existing EconomicCalendarWidget already subscribes to that
// topic and consumes the QJsonArray shape this producer emits — so adding
// this service makes the widget light up with zero widget-side code change.
//
// Auth: requires FMP_API_KEY in env or ~/grok-claude/.env (free signup at
// financialmodelingprep.com — 250 req/day, we use 1 per hour).
//
// Coverage: today + next 14 days, all countries. Widget filters/sorts.

#include "datahub/Producer.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QStringList>

namespace fincept::services::bot {

class BotEconCalendarService : public QObject, public fincept::datahub::Producer {
    Q_OBJECT
  public:
    static BotEconCalendarService& instance();

    /// Producer interface — Hub calls these.
    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override { return 1; }

  private:
    BotEconCalendarService();
    ~BotEconCalendarService() override = default;

    void fetch_and_publish();
    void publish_empty(const QString& reason);

    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace fincept::services::bot
