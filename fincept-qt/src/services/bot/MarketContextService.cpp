#include "services/bot/MarketContextService.h"

#include "ai_chat/LlmService.h"
#include "datahub/DataHub.h"
#include "services/bot/BotConfig.h"
#include "services/bot/BotTypes.h"
#include "services/markets/MarketDataService.h"   // for QuoteData
#include "storage/repositories/LlmConfigRepository.h"

#include <QEventLoop>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTextStream>
#include <QTimeZone>
#include <QtConcurrent>
#include <QUrl>
#include <QUrlQuery>

namespace fincept::services::bot {

namespace {

// Diagnostic logger (mirrors mq_diag in BotIndicesService).
void mc_diag(const QString& msg) {
    static QString diag_path;
    if (diag_path.isEmpty())
        diag_path = BotConfig::instance().data_path() + "/.pinpunch_botservice.log";
    auto stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QFile f(diag_path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QTextStream s(&f);
        s << stamp << "  [mctx] " << msg << "\n";
    }
}

QString cache_path() {
    return BotConfig::instance().data_path() + "/.pinpunch_market_context.json";
}

// Symbols pulled from the DataHub cache. Pre-populated by BotIndicesService —
// this is just a peek, no fetch.
const QVector<QPair<const char*, const char*>>& macro_symbols() {
    static const QVector<QPair<const char*, const char*>> kSyms = {
        {"^GSPC",     "S&P 500"},
        {"^TNX",      "10Y Treasury yield"},
        {"^VIX",      "VIX"},
        {"DX-Y.NYB",  "DXY (dollar index)"},
        {"GC=F",      "Gold"},
        {"CL=F",      "WTI Oil"},
        {"BTC-USD",   "BTC-USD"},
    };
    return kSyms;
}

// Format a macro snapshot line — "S&P 500       5832.47   -1.52%"
QString fmt_macro_line(const QString& label, const services::QuoteData& q) {
    return QString::asprintf("  %-22s %10.2f   %s%.2f%%",
        label.toUtf8().constData(), q.price,
        q.change_pct >= 0 ? "+" : "", q.change_pct);
}

// Build the LLM prompt from macro lines + Finnhub headlines.
QString compose_prompt(const QStringList& macro_lines, const QJsonArray& articles) {
    QString prompt;
    prompt += "You are a senior macro strategist. In 3-5 concise sentences, "
              "explain what is driving US markets right now using ONLY the "
              "data below. The narrative should explain BOTH up and down "
              "moves. No disclaimers, no hedging, no apologies. Be specific "
              "about which data points are dominant.\n\n";
    prompt += "CURRENT MARKET (vs previous close):\n";
    for (const auto& l : macro_lines) prompt += l + "\n";
    prompt += "\nTOP HEADLINES (last 6h):\n";
    int n = 0;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto& av : articles) {
        if (n >= 8) break;
        const QJsonObject a = av.toObject();
        const QString headline = a.value("headline").toString().trimmed();
        if (headline.isEmpty()) continue;
        const QString source = a.value("source").toString();
        const qint64 dt = static_cast<qint64>(a.value("datetime").toDouble());
        const qint64 age_h = (dt > 0) ? (now - dt) / 3600 : 0;
        prompt += QString("  %1. %2 (%3, %4h ago)\n").arg(++n).arg(headline).arg(source).arg(age_h);
    }
    if (n == 0) prompt += "  (no headlines available — explain from market data alone)\n";
    prompt += "\nFocus on the dominant driver. Mention specific numbers.";
    return prompt;
}

QStringList top_headlines(const QJsonArray& articles, int max_count = 5) {
    QStringList out;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto& av : articles) {
        if (out.size() >= max_count) break;
        const QJsonObject a = av.toObject();
        const QString headline = a.value("headline").toString().trimmed();
        if (headline.isEmpty()) continue;
        const QString source = a.value("source").toString();
        const qint64 dt = static_cast<qint64>(a.value("datetime").toDouble());
        const qint64 age_h = (dt > 0) ? (now - dt) / 3600 : 0;
        out.append(QString("• %1 — %2 (%3h ago)").arg(headline, source).arg(age_h));
    }
    return out;
}

// ─── ET timezone helpers ────────────────────────────────────────────────
// We want Sun 20:00 → Fri 20:00 in America/New_York. QTimeZone handles
// DST automatically.
QDateTime to_et(const QDateTime& utc) {
    static const QTimeZone ny = QTimeZone("America/New_York");
    return utc.toTimeZone(ny);
}

} // namespace

MarketContextService& MarketContextService::instance() {
    static MarketContextService inst;
    return inst;
}

MarketContextService::MarketContextService() : QObject(nullptr) {
    nam_ = new QNetworkAccessManager(qApp);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        if (nam_) { nam_->deleteLater(); nam_ = nullptr; }
    });

    hourly_timer_ = new QTimer(this);
    hourly_timer_->setInterval(60 * 60 * 1000);   // 1 hour
    QObject::connect(hourly_timer_, &QTimer::timeout, this, [this]() {
        if (!in_active_window(QDateTime::currentDateTimeUtc())) {
            publish_off_hours();
            return;
        }
        start_fetch();
    });
    hourly_timer_->start();

    // Restore last-known narrative from disk so the widget shows
    // SOMETHING immediately on app launch (instead of "loading…" for the
    // first hour).
    restore_from_cache_if_present();

    // Kick a first refresh on the next event-loop tick — gives DataHub a
    // moment to populate the macro symbol cache from BotIndicesService
    // before we read it.
    QTimer::singleShot(15'000, this, [this]() {
        if (in_active_window(QDateTime::currentDateTimeUtc())) start_fetch();
        else publish_off_hours();
    });
}

QStringList MarketContextService::topic_patterns() const {
    return { QString::fromUtf8(topics::kMarketContext) };
}

void MarketContextService::refresh(const QStringList& /*topics*/) {
    // Hub-driven refresh fires when a widget subscribes for the first
    // time. We respect the same gate as the timer.
    if (in_active_window(QDateTime::currentDateTimeUtc())) start_fetch();
    else publish_off_hours();
}

void MarketContextService::refresh_now() {
    // Manual button — bypasses the hourly timer AND clears any "in
    // flight" guard from a stuck previous fetch. Still respects the
    // weekend gate (no point asking the LLM "why are markets moving?"
    // on Saturday afternoon when the market is closed). A user click is
    // an explicit "I want fresh data NOW" signal, so we force the
    // attempt even if the deferred-startup tick is still pending.
    fetch_in_flight_ = false;
    mc_diag("refresh_now: user-triggered, clearing in_flight guard");
    if (in_active_window(QDateTime::currentDateTimeUtc())) start_fetch();
    else publish_off_hours();
}

bool MarketContextService::in_active_window(const QDateTime& now_utc) {
    const QDateTime et = to_et(now_utc);
    const int dow = et.date().dayOfWeek();   // Mon=1 … Sun=7
    const int hour = et.time().hour();
    // Active = Sun 20:00 → Fri 20:00 ET inclusive.
    if (dow == 7) return hour >= 20;          // Sunday after 8pm
    if (dow >= 1 && dow <= 4) return true;    // Mon-Thu all day
    if (dow == 5) return hour < 20;           // Fri before 8pm
    return false;                             // Saturday entirely off
}

qint64 MarketContextService::secs_until_next_tick(const QDateTime& now_utc) {
    // For the widget's "next update at HH:MM" display.
    if (in_active_window(now_utc)) {
        // Next hourly boundary in ET wall-clock.
        const QDateTime et = to_et(now_utc);
        QDateTime next_et(et.date(), QTime(et.time().hour(), 0).addSecs(3600), et.timeZone());
        return now_utc.secsTo(next_et.toUTC());
    }
    // Off-hours → next Sunday 20:00 ET.
    const QDateTime et = to_et(now_utc);
    QDate target = et.date();
    while (target.dayOfWeek() != 7) target = target.addDays(1);
    QDateTime sun_8pm_et(target, QTime(20, 0), et.timeZone());
    if (sun_8pm_et <= et) sun_8pm_et = sun_8pm_et.addDays(7);
    return now_utc.secsTo(sun_8pm_et.toUTC());
}

void MarketContextService::start_fetch() {
    if (fetch_in_flight_) {
        mc_diag("start_fetch: skip (in flight)");
        return;
    }
    fetch_in_flight_ = true;

    // 1) Pull macro snapshots from DataHub cache (peek — no fetch cost).
    QStringList macro_lines;
    auto& hub = fincept::datahub::DataHub::instance();
    for (const auto& kv : macro_symbols()) {
        const QString sym = QString::fromUtf8(kv.first);
        const QString label = QString::fromUtf8(kv.second);
        const QVariant v = hub.peek(QStringLiteral("market:quote:") + sym);
        if (v.isValid() && v.canConvert<services::QuoteData>())
            macro_lines.append(fmt_macro_line(label, v.value<services::QuoteData>()));
    }
    if (macro_lines.isEmpty()) {
        // Macro cache hasn't populated yet — try again on next hourly
        // tick. Don't publish a partial narrative.
        fetch_in_flight_ = false;
        mc_diag("start_fetch: macro cache empty — deferring to next tick");
        return;
    }

    // 2) Fetch Finnhub headlines, then dispatch to LLM (or fallback).
    QPointer<MarketContextService> self = this;
    fetch_finnhub_news([self, macro_lines](QJsonArray articles, QString err) {
        if (!self) return;
        if (!err.isEmpty()) {
            mc_diag(QString("finnhub news fetch failed: %1").arg(err));
            // Continue WITHOUT headlines — the LLM can still synthesize
            // from macro data alone.
        }
        // Decide LLM vs fallback.
        auto& llm = fincept::ai_chat::LlmService::instance();
        if (llm.is_configured()) {
            self->publish_with_llm(macro_lines, articles);
        } else {
            self->publish_headlines_only(macro_lines, articles,
                "No LLM configured (Settings → LLM Configuration). "
                "Set up Gemini for free narrative generation.");
        }
    });
}

void MarketContextService::fetch_finnhub_news(
        std::function<void(QJsonArray, QString)> cb) {
    const QString key = BotConfig::instance().finnhub_key();
    if (key.isEmpty()) {
        cb({}, QStringLiteral("FINNHUB_API_KEY not set"));
        return;
    }
    if (!nam_) { cb({}, QStringLiteral("nam destroyed")); return; }
    QUrl url(QStringLiteral("https://finnhub.io/api/v1/news"));
    QUrlQuery q;
    q.addQueryItem("category", "general");
    q.addQueryItem("token", key);
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Pinpunch/4.0.3 (MarketContext)");
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QNetworkReply* reply = nam_->get(req);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto err_kind = reply->error();
        reply->deleteLater();
        if (err_kind != QNetworkReply::NoError && status == 0) {
            cb({}, QString("network: %1").arg(reply->errorString()));
            return;
        }
        if (status >= 400) {
            cb({}, QString("HTTP %1: %2").arg(status).arg(QString::fromUtf8(body.left(160))));
            return;
        }
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
            cb({}, QString("parse: %1").arg(pe.errorString()));
            return;
        }
        cb(doc.array(), QString());
    });
}

void MarketContextService::publish_with_llm(const QStringList& macro_lines,
                                            const QJsonArray& articles) {
    // Resolution order (top-down, first-match wins):
    //   1. Saved `gemini` provider in LlmConfigRepository — pinned regardless
    //      of which provider is currently "active". Lets the user keep
    //      Cerebras (or whatever) for AI Chat while routing the hourly
    //      market narrative specifically to Gemini's free tier.
    //   2. Active LlmService provider — generic fallback (Cerebras, Grok,
    //      OpenAI, etc., whatever has the ✓ in Settings).
    //   3. Headlines-only — if no LLM is configured at all, hand the
    //      operator the raw headlines and a setup hint.
    const QString prompt = compose_prompt(macro_lines, articles);

    // Try (1) — direct Gemini, looked up from the provider DB.
    QString gemini_key, gemini_model, gemini_base_url;
    {
        auto rows = fincept::LlmConfigRepository::instance().list_providers();
        if (rows.is_ok()) {
            for (const auto& r : rows.value()) {
                if (r.provider.toLower() == "gemini" && !r.api_key.isEmpty()) {
                    gemini_key      = r.api_key;
                    gemini_model    = r.model.isEmpty()
                                          ? QStringLiteral("gemini-2.5-flash")
                                          : r.model;
                    gemini_base_url = r.base_url;
                    break;
                }
            }
        }
    }

    if (!gemini_key.isEmpty()) {
        mc_diag(QString("LLM call → gemini / %1 (prompt %2 chars, pinned)")
                    .arg(gemini_model).arg(prompt.size()));
        QPointer<MarketContextService> self = this;
        const QString key = gemini_key;
        const QString model = gemini_model;
        const QString base_url = gemini_base_url;
        (void)QtConcurrent::run([self, prompt, key, model, base_url, articles]() {
            if (!self) return;
            const auto res = self->gemini_direct(key, model, base_url, prompt);
            QMetaObject::invokeMethod(qApp, [self, res, articles]() {
                if (!self) return;
                self->fetch_in_flight_ = false;
                const qint64 now = QDateTime::currentSecsSinceEpoch();
                MarketContextItem out;
                out.in_active_window  = true;
                out.generated_at_secs = now;
                out.next_update_secs  = now + secs_until_next_tick(QDateTime::currentDateTimeUtc());
                out.llm_provider      = QStringLiteral("gemini");
                out.llm_model         = res.model;
                out.top_headlines     = top_headlines(articles);
                if (res.error.isEmpty() && !res.text.trimmed().isEmpty()) {
                    out.narrative = res.text.trimmed();
                } else {
                    out.error = res.error.isEmpty()
                                    ? QStringLiteral("empty Gemini response") : res.error;
                    out.narrative = QStringLiteral("(Gemini failed: %1)\n\nHeadlines:\n%2")
                        .arg(out.error.left(160), out.top_headlines.join("\n"));
                }
                mc_diag(QString("publish (gemini): ok=%1 narrative=%2 chars err=%3")
                            .arg(out.error.isEmpty()).arg(out.narrative.size())
                            .arg(out.error.left(80)));
                fincept::datahub::DataHub::instance().publish(
                    QString::fromUtf8(topics::kMarketContext),
                    QVariant::fromValue(out));
                self->persist_to_cache(out);
            }, Qt::QueuedConnection);
        });
        return;
    }

    // Try (2) — fall back to whichever provider is active in LlmService.
    auto& llm = fincept::ai_chat::LlmService::instance();
    if (!llm.is_configured()) {
        publish_headlines_only(macro_lines, articles,
            "No Gemini provider saved AND no active LLM configured. "
            "Add Gemini in Settings → LLM Configuration for narrative.");
        return;
    }
    const QString provider = llm.active_provider();
    const QString model    = llm.active_model();
    mc_diag(QString("LLM call → %1 / %2 (prompt %3 chars, fallback to active)")
                .arg(provider, model).arg(prompt.size()));

    QPointer<MarketContextService> self = this;
    (void)QtConcurrent::run([self, prompt, provider, model, articles]() {
        const fincept::ai_chat::LlmResponse resp =
            fincept::ai_chat::LlmService::instance().chat(prompt, {}, /*use_tools=*/false);
        QMetaObject::invokeMethod(qApp, [self, resp, provider, model, articles]() {
            if (!self) return;
            self->fetch_in_flight_ = false;
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            MarketContextItem out;
            out.in_active_window  = true;
            out.generated_at_secs = now;
            out.next_update_secs  = now + secs_until_next_tick(QDateTime::currentDateTimeUtc());
            out.llm_provider      = provider;
            out.llm_model         = model;
            out.top_headlines     = top_headlines(articles);
            if (resp.success && !resp.content.trimmed().isEmpty()) {
                out.narrative = resp.content.trimmed();
            } else {
                out.error = resp.error.isEmpty() ? QStringLiteral("empty LLM response") : resp.error;
                out.narrative = QStringLiteral("(LLM failed: %1)\n\nHeadlines:\n%2")
                    .arg(out.error.left(160), out.top_headlines.join("\n"));
            }
            mc_diag(QString("publish (active=%1): ok=%2 narrative=%3 chars err=%4")
                        .arg(provider).arg(resp.success).arg(out.narrative.size())
                        .arg(out.error.left(80)));
            fincept::datahub::DataHub::instance().publish(
                QString::fromUtf8(topics::kMarketContext),
                QVariant::fromValue(out));
            self->persist_to_cache(out);
        }, Qt::QueuedConnection);
    });
}

MarketContextService::GeminiResult MarketContextService::gemini_direct(
        const QString& api_key, const QString& model,
        const QString& base_url, const QString& prompt) {
    GeminiResult r;
    r.model = model;

    // Compose endpoint. Allow user-overridden base_url (for proxy/self-host)
    // but default to Google's public Gemini endpoint.
    QString endpoint;
    if (!base_url.trimmed().isEmpty()) {
        QString b = base_url.trimmed();
        if (b.endsWith('/')) b.chop(1);
        endpoint = b + "/v1beta/models/" + model + ":generateContent";
    } else {
        endpoint = "https://generativelanguage.googleapis.com/v1beta/models/"
                   + model + ":generateContent";
    }

    // Gemini request shape — minimal, no system prompt slot needed; we
    // keep the prompt monolithic so the model treats it as a user turn.
    QJsonObject body;
    QJsonArray contents;
    QJsonObject content;
    content["role"] = "user";
    QJsonArray parts;
    QJsonObject part;
    part["text"] = prompt;
    parts.append(part);
    content["parts"] = parts;
    contents.append(content);
    body["contents"] = contents;
    QJsonObject gen_cfg;
    gen_cfg["temperature"] = 0.4;          // narrative needs a touch of creativity but stay grounded
    // 4096 budget so the model can think + produce. gemini-2.5-flash bills
    // hidden "thinking" tokens against the same maxOutputTokens cap, and a
    // dense macro + headlines prompt was burning 800+ of those before any
    // visible text — narratives kept getting cut off mid-sentence at 1024.
    gen_cfg["maxOutputTokens"] = 4096;
    body["generationConfig"] = gen_cfg;

    // Background-thread blocking POST via QEventLoop. We're already on
    // a QtConcurrent worker, so this is fine. QNAM is created here on
    // the worker thread because Qt's QNAM is thread-affine to its
    // creating thread — using nam_ (qApp-parented) from a worker would
    // schedule signals back on the main thread and we can't QEventLoop
    // wait for those without blocking the main loop.
    QNetworkAccessManager local_nam;
    QUrl url(endpoint);
    QNetworkRequest req(url);
    req.setRawHeader("x-goog-api-key", api_key.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, "Pinpunch/4.0.3 (MarketContext)");
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    QNetworkReply* reply = local_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(60'000, &loop, &QEventLoop::quit);  // 60s hard cap
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray bytes = reply->readAll();
    const auto err_kind = reply->error();
    reply->deleteLater();

    if (err_kind != QNetworkReply::NoError && status == 0) {
        r.error = QString("network: %1").arg(reply->errorString());
        return r;
    }
    if (status >= 400) {
        r.error = QString("HTTP %1: %2").arg(status).arg(QString::fromUtf8(bytes.left(220)));
        return r;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError) {
        r.error = QString("parse: %1").arg(pe.errorString());
        return r;
    }
    // Gemini response shape:
    //   { candidates: [{ content: { parts: [{text: "..."}], role:"model"},
    //                    finishReason: "STOP"|"MAX_TOKENS"|"SAFETY"|..., ...}], ...}
    const QJsonArray cands = doc.object().value("candidates").toArray();
    if (cands.isEmpty()) {
        r.error = QStringLiteral("no candidates in response");
        return r;
    }
    const QJsonObject cand_obj  = cands.first().toObject();
    const QString finish_reason = cand_obj.value("finishReason").toString();
    const QJsonObject parts_obj = cand_obj.value("content").toObject();
    const QJsonArray parts_arr  = parts_obj.value("parts").toArray();
    QString text;
    for (const auto& p : parts_arr) {
        text += p.toObject().value("text").toString();
    }
    // Surface a non-STOP finish reason in the diag log so truncations
    // (MAX_TOKENS) or content filtering (SAFETY) are obvious post-hoc
    // rather than discovered visually mid-sentence on the dashboard.
    if (!finish_reason.isEmpty() && finish_reason != "STOP") {
        mc_diag(QString("gemini finish_reason=%1 (text len=%2)")
                    .arg(finish_reason).arg(text.size()));
    }
    if (text.trimmed().isEmpty()) {
        r.error = QStringLiteral("empty text in candidates (finish_reason=%1)").arg(finish_reason);
        return r;
    }
    r.text = text;
    return r;
}

void MarketContextService::publish_headlines_only(const QStringList& macro_lines,
                                                  const QJsonArray& articles,
                                                  const QString& reason) {
    fetch_in_flight_ = false;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    MarketContextItem out;
    out.in_active_window  = true;
    out.generated_at_secs = now;
    out.next_update_secs  = now + secs_until_next_tick(QDateTime::currentDateTimeUtc());
    out.top_headlines     = top_headlines(articles);
    out.error             = reason;
    QString body = reason + "\n\n";
    body += "Macro snapshot:\n" + macro_lines.join("\n");
    if (!out.top_headlines.isEmpty())
        body += "\n\nRecent headlines:\n" + out.top_headlines.join("\n");
    out.narrative = body;
    mc_diag(QString("publish (headlines-only): %1").arg(reason.left(120)));
    fincept::datahub::DataHub::instance().publish(
        QString::fromUtf8(topics::kMarketContext),
        QVariant::fromValue(out));
    persist_to_cache(out);
}

void MarketContextService::publish_off_hours() {
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    MarketContextItem out;
    out.in_active_window  = false;
    out.generated_at_secs = now;
    out.next_update_secs  = now + secs_until_next_tick(QDateTime::currentDateTimeUtc());
    out.narrative = QStringLiteral(
        "Weekly close — market context updates are paused until Sunday "
        "20:00 ET (Asia open). Use this gap to review the week's trade log.");
    mc_diag("publish (off-hours)");
    fincept::datahub::DataHub::instance().publish(
        QString::fromUtf8(topics::kMarketContext),
        QVariant::fromValue(out));
}

void MarketContextService::persist_to_cache(const MarketContextItem& item) {
    QJsonObject o;
    o["narrative"]         = item.narrative;
    o["generated_at_secs"] = static_cast<qint64>(item.generated_at_secs);
    o["next_update_secs"]  = static_cast<qint64>(item.next_update_secs);
    o["llm_provider"]      = item.llm_provider;
    o["llm_model"]         = item.llm_model;
    o["in_active_window"]  = item.in_active_window;
    o["error"]             = item.error;
    QJsonArray heads;
    for (const auto& h : item.top_headlines) heads.append(h);
    o["top_headlines"] = heads;

    QFile f(cache_path());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
}

void MarketContextService::restore_from_cache_if_present() {
    QFile f(cache_path());
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject o = doc.object();
    MarketContextItem item;
    item.narrative         = o.value("narrative").toString();
    item.generated_at_secs = static_cast<qint64>(o.value("generated_at_secs").toDouble());
    item.next_update_secs  = static_cast<qint64>(o.value("next_update_secs").toDouble());
    item.llm_provider      = o.value("llm_provider").toString();
    item.llm_model         = o.value("llm_model").toString();
    item.in_active_window  = o.value("in_active_window").toBool(true);
    item.error             = o.value("error").toString();
    for (const auto& h : o.value("top_headlines").toArray())
        item.top_headlines.append(h.toString());
    if (item.narrative.isEmpty()) return;
    fincept::datahub::DataHub::instance().publish(
        QString::fromUtf8(topics::kMarketContext),
        QVariant::fromValue(item));
    mc_diag("restored last narrative from cache");
}

} // namespace fincept::services::bot
