// Catalog part 3: Wi-Fi credentials, sync and provisioning.
//
// The Wi-Fi password is never written to the response and never logged: `wifi set` copies it only
// into a qz::Secret (zeroed on destruction) for validation, and every wifi command carries
// kFlagSensitive so the dispatcher hides its arguments and scrubs the request line.
#include "commands_internal.hpp"
#include "qz/settings/settings.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::console::detail {
namespace {

using Args = std::span<const std::string_view>;

// ---- wifi ----

void write_wifi(JsonWriter& out, const DeviceApi& api) {
    const std::optional<FixedString<32>> ssid = api.wifi_ssid();
    out.field_bool("configured", ssid.has_value());
    out.key("ssid");
    if (ssid) {
        out.str(ssid->view());
    } else {
        out.null();
    }
    out.field_bool("has_password", ssid.has_value() && api.wifi_has_password());
}

Status wifi_status(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_wifi(out, api);
    return ok();
}

Status wifi_set(DeviceApi& api, Args args, JsonWriter& out) {
    hal::WifiCredentials creds;
    if (!creds.ssid.assign(args[0]) || !creds.password.assign(args[1])) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(settings::validate_credentials(creds));
    QZ_RETURN_IF_ERROR(api.set_wifi(args[0], args[1]));
    write_wifi(out, api);
    return ok();
}

Status wifi_clear(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.clear_wifi());
    write_wifi(out, api);
    return ok();
}

// ---- sync ----

void write_optional_time(JsonWriter& out, std::string_view key, time::UnixSeconds t) {
    out.key(key);
    if (t == 0) {
        out.null();
        return;
    }
    out.num(t);
}

void write_sync(JsonWriter& out, const SyncInfo& info) {
    out.field("indicator", sync_indicator_name(info.indicator));
    write_optional_time(out, "last_ok", info.last_ok);
    out.field("fail_streak", info.fail_streak);
    out.key("last_error");
    const std::string_view token = stored_error_token(info.last_error);
    if (token.empty()) {
        out.null();
    } else {
        out.str(token);
    }
    write_optional_time(out, "next_time_sync", info.next_time_sync);
    write_optional_time(out, "next_weather", info.next_weather);
}

Status sync_now(DeviceApi& api, Args args, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(require_radio(api));
    bool time_job = true;
    bool weather_job = true;
    if (!args.empty()) {
        if (args[0] == "time") {
            weather_job = false;
        } else if (args[0] == "weather") {
            time_job = false;
        } else if (args[0] != "all") {
            return Errc::kBadArgs;
        }
    }
    QZ_RETURN_IF_ERROR(api.sync_now(time_job, weather_job));
    out.field_bool("time", time_job);
    out.field_bool("weather", weather_job);
    write_sync(out, api.sync_info());
    return ok();
}

Status sync_status(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_sync(out, api.sync_info());
    return ok();
}

// ---- provisioning ----

Status provision_start(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(require_radio(api));
    FixedString<32> ssid;
    std::uint16_t expires_s = 0;
    QZ_RETURN_IF_ERROR(api.start_provisioning(ssid, expires_s));
    out.field("ssid", ssid.view()); // the access-point password is shown on the watch only
    out.field("expires_s", expires_s);
    return ok();
}

Status provision_stop(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.stop_provisioning());
    out.field_bool("stopped", true);
    return ok();
}

constexpr std::uint8_t kSensitive = kFlagSensitive;
constexpr std::uint8_t kSensitiveDestructive = kFlagSensitive | kFlagDestructive;

constexpr auto kCommands = std::to_array<Command>({
    {"wifi status",
     "wifi status",
     "stored network name (never the password)",
     0,
     0,
     kSensitive,
     wifi_status},
    {"wifi set",
     "wifi set <ssid> <password>",
     "store credentials; \"\" as password for an open network",
     2,
     2,
     kSensitive,
     wifi_set},
    {"wifi clear",
     "wifi clear",
     "forget the stored credentials",
     0,
     0,
     kSensitiveDestructive,
     wifi_clear},
    {"sync now",
     "sync now [time|weather|all]",
     "run a sync session immediately",
     0,
     1,
     kFlagNeedsRadio,
     sync_now},
    {"sync status",
     "sync status",
     "last result and next scheduled syncs",
     0,
     0,
     kFlagNone,
     sync_status},
    {"provision start",
     "provision start",
     "start the phone provisioning access point",
     0,
     0,
     kFlagNeedsRadio,
     provision_start},
    {"provision stop", "provision stop", "stop provisioning", 0, 0, kFlagNone, provision_stop},
});

} // namespace

std::span<const Command> radio_commands() noexcept {
    return kCommands;
}

} // namespace qz::console::detail
