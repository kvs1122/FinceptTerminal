#include "core/config/AppConfig.h"

namespace fincept {

AppConfig& AppConfig::instance() {
    static AppConfig s;
    return s;
}

AppConfig::AppConfig() : settings_("Fincept", "FinceptTerminal") {}

QVariant AppConfig::get(const QString& key, const QVariant& default_val) const {
    return settings_.value(key, default_val);
}

void AppConfig::set(const QString& key, const QVariant& value) {
    settings_.setValue(key, value);
}

void AppConfig::remove(const QString& key) {
    settings_.remove(key);
}

QString AppConfig::api_base_url() const {
    // Pinpunch runs as a local-only operator console. The base URL is left
    // empty by default so the shared HttpClient short-circuits any call to
    // the Fincept-hosted account stack (login, MFA, profile, subscription,
    // session-pulse all targeted https://api.fincept.in). If a user wants
    // to point at a self-hosted compatible API they can set api/base_url
    // explicitly via QSettings.
    return settings_.value("api/base_url", "").toString();
}

bool AppConfig::dark_mode() const {
    return settings_.value("ui/dark_mode", true).toBool();
}

int AppConfig::refresh_interval_ms() const {
    return settings_.value("data/refresh_interval_ms", 30000).toInt();
}

} // namespace fincept
