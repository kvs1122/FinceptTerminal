#include "services/bot/AlpacaRestClient.h"
#include "services/bot/BotConfig.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

namespace fincept::services::bot {

// Note: the trading endpoints (account, positions, watchlists) live on
// `paper-api.alpaca.markets` or `api.alpaca.markets` depending on paper/live.
// The market-data endpoints (quotes, snapshots) ALWAYS live on
// `data.alpaca.markets` regardless of paper/live — same data feed for both.
static QString trading_base_url() {
    return BotConfig::instance().alpaca_base_url();
}
static QString data_base_url() {
    return QStringLiteral("https://data.alpaca.markets");
}

AlpacaRestClient& AlpacaRestClient::instance() {
    static AlpacaRestClient inst;
    return inst;
}

AlpacaRestClient::AlpacaRestClient() : QObject(nullptr) {
    // Parent the QNAM to qApp (not `this`) so it gets deleted DURING
    // QCoreApplication shutdown — before the network worker threads are
    // torn down. Without this, a connection-attempt timer fires after
    // QNAM is dead → SIGSEGV in QAbstractSocketEngine. Was crashing
    // every clean quit until 2026-05-14 fix.
    nam_ = new QNetworkAccessManager(qApp);
    // Belt-and-braces: when aboutToQuit fires (still on main event loop),
    // explicitly delete the QNAM. Its destructor blocks until the
    // network thread is fully drained, so no late-firing timers can race
    // against destroyed state.
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (nam_) { nam_->deleteLater(); nam_ = nullptr; }
    });
}

void AlpacaRestClient::run_get(const QString& full_url, JsonCallback cb) {
    if (!nam_) { cb(false, {}, QStringLiteral("nam destroyed (shutting down)")); return; }
    const auto& cfg = BotConfig::instance();
    if (!cfg.is_configured()) {
        cb(false, {}, QStringLiteral("Alpaca credentials missing (check PINPUNCH_ALPACA_KEY/SECRET or bot's .env)"));
        return;
    }
    QNetworkRequest req((QUrl(full_url)));
    // Alpaca uses two custom headers (NOT Bearer auth)
    req.setRawHeader("APCA-API-KEY-ID",     cfg.alpaca_key().toUtf8());
    req.setRawHeader("APCA-API-SECRET-KEY", cfg.alpaca_secret().toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "Pinpunch/4.0.3 (BotService)");
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
            cb(false, {}, QString("HTTP %1: %2").arg(status).arg(QString::fromUtf8(body.left(200))));
            return;
        }
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError) {
            cb(false, {}, QString("JSON parse: %1").arg(pe.errorString()));
            return;
        }
        // Wrap as QJsonValue so callers can branch on isArray/isObject
        cb(true, doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), QString());
    });
}

void AlpacaRestClient::get_account(JsonCallback cb) {
    run_get(trading_base_url() + "/v2/account", cb);
}

void AlpacaRestClient::get_positions(JsonCallback cb) {
    run_get(trading_base_url() + "/v2/positions", cb);
}

void AlpacaRestClient::get_watchlists(JsonCallback cb) {
    run_get(trading_base_url() + "/v2/watchlists", cb);
}

void AlpacaRestClient::get_watchlist(const QString& id, JsonCallback cb) {
    run_get(trading_base_url() + "/v2/watchlists/" + QUrl::toPercentEncoding(id), cb);
}

void AlpacaRestClient::get_latest_quotes(const QStringList& symbols, JsonCallback cb) {
    if (symbols.isEmpty()) { cb(true, QJsonObject(), QString()); return; }
    QUrl url(data_base_url() + "/v2/stocks/quotes/latest");
    QUrlQuery q;
    q.addQueryItem("symbols", symbols.join(','));
    url.setQuery(q);
    run_get(url.toString(), cb);
}

void AlpacaRestClient::get_snapshots(const QStringList& symbols, JsonCallback cb) {
    if (symbols.isEmpty()) { cb(true, QJsonObject(), QString()); return; }
    QUrl url(data_base_url() + "/v2/stocks/snapshots");
    QUrlQuery q;
    q.addQueryItem("symbols", symbols.join(','));
    url.setQuery(q);
    run_get(url.toString(), cb);
}

void AlpacaRestClient::get_crypto_snapshots(const QStringList& pairs, JsonCallback cb) {
    if (pairs.isEmpty()) { cb(true, QJsonObject(), QString()); return; }
    QUrl url(data_base_url() + "/v1beta3/crypto/us/snapshots");
    QUrlQuery q;
    // Join with comma; QUrlQuery handles URL-encoding the slash in BTC/USD.
    q.addQueryItem("symbols", pairs.join(','));
    url.setQuery(q);
    run_get(url.toString(), cb);
}

} // namespace fincept::services::bot
