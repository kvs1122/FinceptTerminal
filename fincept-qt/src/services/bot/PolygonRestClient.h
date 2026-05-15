#pragma once

// ─── Polygon.io Stocks REST client (read-only) ────────────────────────────
//
// Pinpunch's Polygon adapter for US stocks/ETFs/ADRs. We DELIBERATELY do
// not use Polygon's WebSocket — the bot's predictive layer reserves that
// connection (see grok-claude/predictive/polygon_ws.py + feeds.py). REST
// snapshots return sub-second-precise prices and the Stocks Developer
// plan ($79/mo) has unlimited API calls, so a 1s polling cadence yields
// effectively the same UI freshness as a WebSocket without the conflict.
//
// Endpoint used:
//   GET /v3/snapshot?ticker.any_of=AAPL,MSFT,SPY,...&apiKey=…
//
// Response shape (per result):
//   {
//     "ticker": "AAPL",
//     "type": "stocks",
//     "name": "Apple Inc.",
//     "market_status": "open"|"closed"|...,
//     "session": {
//       "price":            <last price>,
//       "change":           <abs $ change vs previous_close>,
//       "change_percent":   <% change>,
//       "open", "high", "low", "close",
//       "previous_close":   <prev session close>,
//       "volume":           <session volume>,
//       "vwap":             <volume-weighted avg price>,
//       "last_updated":     <epoch nanoseconds>,
//       ... (early/regular/late trading splits)
//     },
//     "last_trade": { "price", "size", "exchange", "last_updated", ... },
//     "last_minute": { "open", "high", "low", "close", "transactions", ... }
//   }
//
// Auth: API key sent as Authorization: Bearer header (Polygon also accepts
// ?apiKey=... query param but the header avoids leaking the key in proxy
// access logs).

#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace fincept::services::bot {

class PolygonRestClient : public QObject {
    Q_OBJECT
  public:
    static PolygonRestClient& instance();

    /// Callback signature: (ok, json, error_message).
    /// ok=false implies non-empty error_message (network, auth, parse, HTTP 4xx/5xx).
    using JsonCallback = std::function<void(bool ok, const QJsonValue& json, const QString& err)>;

    /// True iff a Polygon API key is configured. Callers should fall back
    /// to Alpaca / yfinance when this returns false.
    bool is_configured() const;

    /// Batch snapshot fetch for US stocks/ETFs/ADRs. Symbols MUST be in
    /// Polygon's canonical form (e.g. BRK.B not BRK-B). Caller is
    /// responsible for any vendor-format conversion before calling.
    /// Polygon caps `ticker.any_of` at 250 entries per call; we don't
    /// enforce here, but expect callers to chunk if their universe grows.
    void get_snapshots(const QStringList& symbols, JsonCallback cb);

    /// Search for tickers by symbol or company name (e.g. "TSLA" or
    /// "Tesla"). Calls /v3/reference/tickers?search=…&active=true&limit=N.
    /// Result: { results: [{ticker, name, market, locale, type, ...}] }.
    void search_ticker(const QString& query, int limit, JsonCallback cb);

    /// Reference details for a single ticker. Returns market_cap, share_class
    /// outstanding, sic_description, primary_exchange, and other static data.
    /// Calls /v3/reference/tickers/{ticker}.
    void get_ticker_details(const QString& ticker, JsonCallback cb);

    /// Daily aggregates for a ticker over a date range. Used for 52-week
    /// high/low computation. Dates in YYYY-MM-DD format, sort="asc"|"desc",
    /// limit caps result count (Polygon max 50000). Returns
    /// { results: [{t, o, h, l, c, v}] }.
    void get_aggregates(const QString& ticker, const QString& from_date,
                        const QString& to_date, const QString& sort, int limit,
                        JsonCallback cb);

    /// Reported financials for a ticker — used to surface the most recent
    /// EPS and report date. Calls /vX/reference/financials?ticker=…&limit=1
    /// &sort=period_of_report_date.desc. Returns:
    ///   { results: [{ period_of_report_date, filing_date, fiscal_period,
    ///     financials: { income_statement: {
    ///       basic_earnings_per_share: { value, unit }, … }}}] }
    void get_financials(const QString& ticker, JsonCallback cb);

  private:
    PolygonRestClient();
    void run_get(const QString& full_url, JsonCallback cb);

    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace fincept::services::bot
