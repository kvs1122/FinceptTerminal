#include "services/bot/BotEconCalendarService.h"
#include "services/bot/BotConfig.h"
#include "datahub/DataHub.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace fincept::services::bot {

static constexpr const char* kTopic = "econ:fincept:upcoming_events";

// Diagnostic log helper — same file as BotService writes to.
static void econ_diag(const QString& msg) {
    static QString diag_path;
    if (diag_path.isEmpty()) {
        diag_path = BotConfig::instance().data_path() + "/.pinpunch_botservice.log";
    }
    auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QFile f(diag_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QTextStream s(&f);
        s << stamp << "  [econ] " << msg << "\n";
    }
}

BotEconCalendarService& BotEconCalendarService::instance() {
    static BotEconCalendarService inst;
    return inst;
}

BotEconCalendarService::BotEconCalendarService() : QObject(nullptr) {
    // See AlpacaRestClient::AlpacaRestClient for shutdown-race rationale.
    nam_ = new QNetworkAccessManager(qApp);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (nam_) { nam_->deleteLater(); nam_ = nullptr; }
    });
}

QStringList BotEconCalendarService::topic_patterns() const {
    return { QString::fromUtf8(kTopic) };
}

void BotEconCalendarService::refresh(const QStringList& /*topics*/) {
    fetch_and_publish();
}

void BotEconCalendarService::publish_empty(const QString& reason) {
    econ_diag(QString("publishing empty array — %1").arg(reason));
    fincept::datahub::DataHub::instance().publish(
        QString::fromUtf8(kTopic), QVariant::fromValue(QJsonArray{}));
}

void BotEconCalendarService::fetch_and_publish() {
    if (!nam_) return;   // shutting down
    const QString today_str = QDate::currentDate().toString(Qt::ISODate);
    const QString end_str   = QDate::currentDate().addDays(14).toString(Qt::ISODate);

    // Source priority: Finnhub free tier (works out of the box for any
    // operator who has a Finnhub key, which the bot already requires).
    // FMP fallback supports legacy paid keys only since their free
    // calendar tier was deprecated Aug 2025.
    const QString fh_key  = BotConfig::instance().finnhub_key();
    const QString fmp_key = BotConfig::instance().fmp_key();

    QUrl url;
    bool using_finnhub = false;
    if (!fh_key.isEmpty()) {
        url = QUrl("https://finnhub.io/api/v1/calendar/economic");
        QUrlQuery q;
        q.addQueryItem("from",  today_str);
        q.addQueryItem("to",    end_str);
        q.addQueryItem("token", fh_key);
        url.setQuery(q);
        using_finnhub = true;
    } else if (!fmp_key.isEmpty()) {
        url = QUrl("https://financialmodelingprep.com/api/v3/economic_calendar");
        QUrlQuery q;
        q.addQueryItem("from",   today_str);
        q.addQueryItem("to",     end_str);
        q.addQueryItem("apikey", fmp_key);
        url.setQuery(q);
    } else {
        publish_empty("no calendar API key — set FINNHUB_API_KEY in ~/grok-claude/.env");
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Pinpunch/4.0.3 (BotEconCalendar)");
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QNetworkReply* reply = nam_->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this,
        [this, reply, today_str, using_finnhub]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto err_kind = reply->error();
        reply->deleteLater();

        if (err_kind != QNetworkReply::NoError && status == 0) {
            publish_empty(QString("network error: %1").arg(reply->errorString()));
            return;
        }
        if (status >= 400) {
            publish_empty(QString("HTTP %1: %2").arg(status).arg(QString::fromUtf8(body.left(150))));
            return;
        }
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError) {
            publish_empty(QString("JSON parse: %1").arg(pe.errorString()));
            return;
        }

        // Both providers map to the widget's expected shape:
        //   { event, country, date (YYYY-MM-DD), time (HH:MM),
        //     importance (int 0-3), actual, forecast, previous }
        QJsonArray out;
        QJsonArray in_arr;

        if (using_finnhub) {
            // Finnhub: { economicCalendar: [{ country, event, time
            //   ("YYYY-MM-DD HH:MM:SS"), actual, estimate, prev,
            //   impact ("low"/"medium"/"high"), unit, scale }] }
            if (!doc.isObject()) { publish_empty("Finnhub: not an object"); return; }
            in_arr = doc.object().value("economicCalendar").toArray();
            for (const auto& v : in_arr) {
                const auto e = v.toObject();
                QJsonObject mapped;
                mapped["event"]   = e.value("event").toString();
                mapped["country"] = e.value("country").toString();

                QString raw = e.value("time").toString();
                QString date_part, time_part;
                int sep = raw.indexOf(' ');
                if (sep < 0) sep = raw.indexOf('T');
                if (sep > 0) {
                    date_part = raw.left(sep);
                    time_part = raw.mid(sep + 1).left(5);
                } else { date_part = raw; }
                mapped["date"] = date_part;
                mapped["time"] = time_part;

                const QString imp = e.value("impact").toString().toLower();
                int imp_int = 0;
                if      (imp == "high")   imp_int = 3;
                else if (imp == "medium") imp_int = 2;
                else if (imp == "low")    imp_int = 1;
                mapped["importance"] = imp_int;

                auto fmt = [&](const char* k) -> QString {
                    auto vv = e.value(k);
                    if (vv.isNull() || vv.isUndefined()) return QString();
                    if (vv.isString()) return vv.toString();
                    if (vv.isDouble())  return QString::number(vv.toDouble());
                    return QString();
                };
                mapped["actual"]   = fmt("actual");
                mapped["forecast"] = fmt("estimate");
                mapped["previous"] = fmt("prev");
                out.append(mapped);
            }
        } else {
            // FMP: top-level array of { event, date (ISO), country,
            //   actual, previous, estimate, impact }
            if (!doc.isArray()) { publish_empty("FMP: not an array"); return; }
            in_arr = doc.array();
            for (const auto& v : in_arr) {
                const auto e = v.toObject();
                QJsonObject mapped;
                mapped["event"]   = e.value("event").toString();
                mapped["country"] = e.value("country").toString();

                QString raw = e.value("date").toString();
                QString date_part, time_part;
                int sep = raw.indexOf('T');
                if (sep < 0) sep = raw.indexOf(' ');
                if (sep > 0) {
                    date_part = raw.left(sep);
                    time_part = raw.mid(sep + 1).left(5);
                } else { date_part = raw; }
                mapped["date"] = date_part;
                mapped["time"] = time_part;

                const QString imp = e.value("impact").toString().toLower();
                int imp_int = 0;
                if      (imp == "high")   imp_int = 3;
                else if (imp == "medium") imp_int = 2;
                else if (imp == "low")    imp_int = 1;
                mapped["importance"] = imp_int;

                auto fmt = [&](const char* k) -> QString {
                    auto vv = e.value(k);
                    if (vv.isNull() || vv.isUndefined()) return QString();
                    if (vv.isString()) return vv.toString();
                    if (vv.isDouble())  return QString::number(vv.toDouble());
                    return QString();
                };
                mapped["actual"]   = fmt("actual");
                mapped["forecast"] = fmt("estimate");
                mapped["previous"] = fmt("previous");
                out.append(mapped);
            }
        }

        // ── Filter noise + sort by operator relevance ─────────────────
        // Finnhub free tier returns hundreds of low-value events
        // (Ascension Day across 30 EU countries, religious holidays,
        // generic "speech" entries with no figures). Operator wants the
        // *signal* — CPI, PPI, FOMC, NFP, retail sales, GDP, jobless
        // claims, central bank decisions. Filter + sort accordingly.
        //
        // 1. Drop low-quality rows: importance=0 AND no figures at all
        // 2. Drop known holiday/non-econ noise by event-name pattern
        // 3. Sort: importance DESC, then US-first, then date ASC
        // 4. Cap at 60 (widget renders first 25; extra buffer for theme)

        // Common noise patterns — religious holidays + day-of-rest markers.
        // (Finnhub tags these as low/medium impact across many countries.)
        static const QSet<QString> kNoiseWords = {
            "Ascension", "Christmas", "Easter", "Pentecost", "Eid", "Diwali",
            "Holiday", "Day off", "Bank Holiday", "Memorial Day",
            "Labor Day", "Independence Day", "Thanksgiving", "Boxing Day",
        };

        struct Row { QJsonObject o; int imp; bool is_us; QString date; };
        QVector<Row> rows;
        rows.reserve(out.size());
        for (const auto& v : out) {
            const auto o = v.toObject();
            const int imp = o.value("importance").toInt(0);
            const QString event_name = o.value("event").toString();
            const QString country = o.value("country").toString().toUpper();
            const QString actual = o.value("actual").toString();
            const QString forecast = o.value("forecast").toString();
            const QString previous = o.value("previous").toString();

            // Skip rows that are clearly noise: 0 importance AND no numeric data.
            const bool has_any_figure = !actual.isEmpty() || !forecast.isEmpty() || !previous.isEmpty();
            if (imp == 0 && !has_any_figure) continue;

            // Skip known holiday/calendar-marker patterns.
            bool noise = false;
            for (const QString& word : kNoiseWords) {
                if (event_name.contains(word, Qt::CaseInsensitive)) {
                    noise = true;
                    break;
                }
            }
            if (noise) continue;

            Row r;
            r.o = o;
            r.imp = imp;
            r.is_us = (country == "US" || country == "USA");
            r.date = o.value("date").toString();
            rows.append(r);
        }

        // Sort: highest impact first, then US-first within same impact,
        // then earliest date first. Same-day events stay in their original
        // (chronological) order via stable sort.
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.imp != b.imp) return a.imp > b.imp;        // 3>2>1>0
            if (a.is_us != b.is_us) return a.is_us;          // US first
            return a.date < b.date;                          // earliest first
        });

        // Rebuild out QJsonArray, cap at 60
        QJsonArray sorted;
        const int cap = std::min<int>(60, rows.size());
        for (int i = 0; i < cap; ++i) sorted.append(rows[i].o);

        econ_diag(QString("fetched %1 events from %2 (%3 → +14d); after filter+sort = %4")
                    .arg(out.size())
                    .arg(using_finnhub ? "Finnhub" : "FMP")
                    .arg(today_str)
                    .arg(sorted.size()));
        fincept::datahub::DataHub::instance().publish(
            QString::fromUtf8(kTopic), QVariant::fromValue(sorted));
    });
}

} // namespace fincept::services::bot
