#pragma once

// ─── Alpaca REST client (read-only) ──────────────────────────────────────
// Pinpunch's own thin Alpaca REST client. Independent from the bot —
// Pinpunch has its own keys (PINPUNCH_ALPACA_KEY) or shares the bot's
// (ALPACA_API_KEY) via .env parsing. NEVER subscribes to the Alpaca
// WebSocket — that's the bot's exclusive subscription. We only do REST.
//
// All calls are async; results delivered on the Qt event loop.
//
// Endpoints used:
//   GET /v2/account                          → BotAccount source-of-truth
//   GET /v2/positions                        → BotPosition[] source-of-truth
//   GET /v2/watchlists                       → list user watchlists
//   GET /v2/watchlists/{id}                  → resolve symbols in a watchlist
//   GET /v2/stocks/quotes/latest?symbols=…   → latest quote per symbol
//   GET /v2/stocks/snapshots?symbols=…       → richer per-symbol snapshot
//                                              (latestTrade + latestQuote +
//                                               minuteBar + dailyBar)

#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace fincept::services::bot {

class AlpacaRestClient : public QObject {
    Q_OBJECT
  public:
    static AlpacaRestClient& instance();

    /// Callback signature: (ok, json, error_message).
    /// ok=false implies non-empty error_message (network, auth, parse, HTTP 4xx/5xx).
    using JsonCallback = std::function<void(bool ok, const QJsonValue& json, const QString& err)>;

    void get_account(JsonCallback cb);
    void get_positions(JsonCallback cb);
    void get_watchlists(JsonCallback cb);
    void get_watchlist(const QString& id, JsonCallback cb);
    void get_latest_quotes(const QStringList& symbols, JsonCallback cb);
    void get_snapshots(const QStringList& symbols, JsonCallback cb);

    /// Crypto snapshots — pairs in "BTC/USD" form (URL-encoded automatically).
    /// Returns { snapshots: { "BTC/USD": { latestTrade, dailyBar, prevDailyBar, ... }}}
    void get_crypto_snapshots(const QStringList& pairs, JsonCallback cb);

  private:
    AlpacaRestClient();
    void run_get(const QString& full_url, JsonCallback cb);

    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace fincept::services::bot
