#include "services/economics/MacroCalendarService.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "network/http/HttpClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>

namespace fincept::services {

namespace {
constexpr const char* kTopic = "econ:fincept:upcoming_events";
// Local-only mode: macro events feed disabled. URL intentionally empty so refresh() exits.
constexpr const char* kUrl = "";

QJsonArray parse_events(const QJsonDocument& doc) {
    if (doc.isArray())
        return doc.array();
    if (!doc.isObject())
        return {};

    const auto root = doc.object();
    // {"success":true,"data":{"events":[...]}}
    if (root.contains(QStringLiteral("data")) && root.value(QStringLiteral("data")).isObject()) {
        const auto data = root.value(QStringLiteral("data")).toObject();
        if (data.contains(QStringLiteral("events")) && data.value(QStringLiteral("events")).isArray())
            return data.value(QStringLiteral("events")).toArray();
    }
    // {"data":[...]}
    if (root.contains(QStringLiteral("data")) && root.value(QStringLiteral("data")).isArray())
        return root.value(QStringLiteral("data")).toArray();
    // {"events":[...]}
    if (root.contains(QStringLiteral("events")) && root.value(QStringLiteral("events")).isArray())
        return root.value(QStringLiteral("events")).toArray();
    return {};
}
} // namespace

MacroCalendarService& MacroCalendarService::instance() {
    static MacroCalendarService s;
    return s;
}

MacroCalendarService::MacroCalendarService(QObject* parent) : QObject(parent) {}

void MacroCalendarService::ensure_registered_with_hub() {
    if (hub_registered_)
        return;
    auto& hub = fincept::datahub::DataHub::instance();
    hub.register_producer(this);

    fincept::datahub::TopicPolicy policy;
    policy.ttl_ms = 5 * 60 * 1000;   // 5 min — macro events refresh slowly
    policy.min_interval_ms = 60 * 1000; // 60 s
    policy.refresh_timeout_ms = 30 * 1000;
    hub.set_policy(QString::fromLatin1(kTopic), policy);

    hub_registered_ = true;
    LOG_INFO("MacroCalendarService", "Registered with DataHub (econ:fincept:upcoming_events)");
}

QStringList MacroCalendarService::topic_patterns() const {
    return {QString::fromLatin1(kTopic)};
}

void MacroCalendarService::refresh(const QStringList& topics) {
    // Single-topic producer — the hub may pass the topic list redundantly,
    // but there's only ever one fetch to do.
    if (!topics.contains(QString::fromLatin1(kTopic)))
        return;

    // Local-only mode: no remote macro feed. Publish empty array so consumers
    // get a clean response instead of an error spinner.
    const QString url = QString::fromLatin1(kUrl);
    if (url.isEmpty()) {
        fincept::datahub::DataHub::instance().publish(
            QString::fromLatin1(kTopic), QVariant::fromValue(QJsonArray{}));
        return;
    }

    QPointer<MacroCalendarService> self = this;
    fincept::HttpClient::instance().get(url,
        [self](fincept::Result<QJsonDocument> result) {
            if (!self)
                return;
            auto& hub = fincept::datahub::DataHub::instance();
            if (!result.is_ok()) {
                hub.publish_error(QString::fromLatin1(kTopic),
                                  QString::fromStdString(result.error()));
                return;
            }
            const QJsonArray events = parse_events(result.value());
            hub.publish(QString::fromLatin1(kTopic), QVariant::fromValue(events));
        });
}

} // namespace fincept::services
