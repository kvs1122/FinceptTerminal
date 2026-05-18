#include "services/bot/PolygonRestClient.h"
#include "services/bot/BotConfig.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace fincept::services::bot {

static QString polygon_base_url() {
    return QStringLiteral("https://api.polygon.io");
}

PolygonRestClient& PolygonRestClient::instance() {
    static PolygonRestClient inst;
    return inst;
}

PolygonRestClient::PolygonRestClient() : QObject(nullptr) {
    // Same shutdown-race fix as AlpacaRestClient/BotEconCalendarService:
    // parent the QNAM to qApp so Qt deletes it during aboutToQuit (on the
    // main event loop, before the network worker thread is torn down).
    // Without this, in-flight Polygon requests can fire their callbacks
    // after QNAM teardown → SIGSEGV.
    nam_ = new QNetworkAccessManager(qApp);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (nam_) { nam_->deleteLater(); nam_ = nullptr; }
    });
}

bool PolygonRestClient::is_configured() const {
    return !BotConfig::instance().polygon_key().isEmpty();
}

void PolygonRestClient::run_get(const QString& full_url, JsonCallback cb) {
    if (!nam_) { cb(false, {}, QStringLiteral("nam destroyed (shutting down)")); return; }
    const QString key = BotConfig::instance().polygon_key();
    if (key.isEmpty()) {
        cb(false, {}, QStringLiteral("Polygon key missing (set POLYGON_API_KEY in ~/grok-claude/.env)"));
        return;
    }
    QNetworkRequest req((QUrl(full_url)));
    // Polygon accepts both `?apiKey=…` and `Authorization: Bearer …`. Use
    // the header form so the key never appears in proxy access logs or
    // QNetworkReply::url() error messages.
    req.setRawHeader("Authorization", ("Bearer " + key).toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "Pinpunch/4.0.3 (PolygonRest)");
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QNetworkReply* reply = nam_->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto err_kind = reply->error();
        reply->deleteLater();

        if (err_kind != QNetworkReply::NoError && status == 0) {
            cb(false, {}, QString("Network error: %1").arg(reply->errorString()));
            return;
        }
        if (status >= 400) {
            // Polygon returns helpful error JSON ({"status":"NOT_AUTHORIZED",
            // "message":"..."}) — surface the first 200 chars so the diag
            // log shows actionable detail.
            cb(false, {}, QString("HTTP %1: %2").arg(status).arg(QString::fromUtf8(body.left(200))));
            return;
        }
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError) {
            cb(false, {}, QString("JSON parse: %1").arg(pe.errorString()));
            return;
        }
        // Polygon snapshot responses are objects with a `results` array.
        // Pass the whole object through so callers can read both `results`
        // and `status` if they need to.
        cb(true, doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), QString());
    });
}

void PolygonRestClient::get_snapshots(const QStringList& symbols, JsonCallback cb) {
    if (symbols.isEmpty()) { cb(true, QJsonObject(), QString()); return; }
    QUrl url(polygon_base_url() + "/v3/snapshot");
    QUrlQuery q;
    // Polygon accepts a comma-separated list for ticker.any_of. We don't
    // chunk here — callers should keep their request universe under 250.
    q.addQueryItem("ticker.any_of", symbols.join(','));
    // Limit defaults to 10; bump it so all our tickers come back in one
    // page (max 250 per Polygon docs).
    q.addQueryItem("limit", "250");
    url.setQuery(q);
    run_get(url.toString(), cb);
}

void PolygonRestClient::search_ticker(const QString& query, int limit, JsonCallback cb) {
    if (query.trimmed().isEmpty()) {
        cb(false, {}, QStringLiteral("empty search query"));
        return;
    }
    QUrl url(polygon_base_url() + "/v3/reference/tickers");
    QUrlQuery q;
    q.addQueryItem("search", query.trimmed());
    q.addQueryItem("active", "true");
    q.addQueryItem("market", "stocks");        // limit to US stocks (skip crypto/forex/options)
    q.addQueryItem("limit", QString::number(std::clamp(limit, 1, 100)));
    url.setQuery(q);
    run_get(url.toString(), cb);
}

void PolygonRestClient::get_ticker_details(const QString& ticker, JsonCallback cb) {
    if (ticker.trimmed().isEmpty()) {
        cb(false, {}, QStringLiteral("empty ticker"));
        return;
    }
    // Path-encode the ticker so symbols like BRK.B don't break the URL.
    const QString safe = QString::fromUtf8(QUrl::toPercentEncoding(ticker.trimmed()));
    run_get(polygon_base_url() + "/v3/reference/tickers/" + safe, cb);
}

void PolygonRestClient::get_aggregates(const QString& ticker, const QString& from_date,
                                       const QString& to_date, const QString& sort, int limit,
                                       JsonCallback cb) {
    if (ticker.trimmed().isEmpty()) {
        cb(false, {}, QStringLiteral("empty ticker"));
        return;
    }
    const QString safe = QString::fromUtf8(QUrl::toPercentEncoding(ticker.trimmed()));
    // Daily aggregates: /v2/aggs/ticker/<sym>/range/1/day/<from>/<to>?adjusted=true&sort=…&limit=…
    QUrl url(polygon_base_url() + "/v2/aggs/ticker/" + safe +
             "/range/1/day/" + from_date + "/" + to_date);
    QUrlQuery q;
    q.addQueryItem("adjusted", "true");
    q.addQueryItem("sort", sort.isEmpty() ? QStringLiteral("asc") : sort);
    q.addQueryItem("limit", QString::number(std::clamp(limit, 1, 50000)));
    url.setQuery(q);
    run_get(url.toString(), cb);
}

void PolygonRestClient::get_financials(const QString& ticker, JsonCallback cb) {
    if (ticker.trimmed().isEmpty()) {
        cb(false, {}, QStringLiteral("empty ticker"));
        return;
    }
    // Polygon's financials endpoint lives at /vX/reference/financials.
    // Query the most recent quarter only — TICKER LOOKUP just needs the
    // latest EPS + period_of_report_date for the "last earnings" line.
    QUrl url(polygon_base_url() + "/vX/reference/financials");
    QUrlQuery q;
    q.addQueryItem("ticker", ticker.trimmed());
    q.addQueryItem("limit", "1");
    q.addQueryItem("order", "desc");
    q.addQueryItem("sort", "period_of_report_date");
    url.setQuery(q);
    run_get(url.toString(), cb);
}

void PolygonRestClient::get_top_movers(const QString& direction, JsonCallback cb) {
    // Snapshot endpoint for top gainers / losers / most active. Polygon
    // returns up to 20 tickers per direction. We use this to feed the
    // top-of-window ticker bar — refreshed every few minutes so the
    // running tape always shows what's actually moving today.
    const QString dir = (direction == "losers" || direction == "most_active"
                            || direction == "most-active")
        ? direction
        : QStringLiteral("gainers");
    // Polygon's URL uses `most_actives` (plural with an "s") historically,
    // but `most-active` works on newer accounts. Normalise to the form
    // Polygon's docs currently show.
    const QString path = (dir == "most_active" || dir == "most-active")
        ? QStringLiteral("/v2/snapshot/locale/us/markets/stocks/most_actives")
        : (dir == "losers"
               ? QStringLiteral("/v2/snapshot/locale/us/markets/stocks/losers")
               : QStringLiteral("/v2/snapshot/locale/us/markets/stocks/gainers"));
    run_get(polygon_base_url() + path, cb);
}

} // namespace fincept::services::bot
