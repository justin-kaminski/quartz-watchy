// Everything console commands may do to the device. Implemented by qz_app (real device and
// simulator) and by test fakes. Calls happen on the app task only.
#pragma once

#include "qz/console/protocol.hpp"
#include "qz/core/log.hpp"
#include "qz/core/result.hpp"
#include "qz/model/types.hpp"
#include "qz/settings/settings.hpp"
#include "qz/time/tz.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::console {

struct FirmwareIdentity {
    std::string_view version, git_hash, idf_version;
    bool radio_compiled = true;
};

struct TimeInfo {
    bool valid = false;
    time::UnixMicros utc_us = 0;
    time::LocalDateTime local{};
    std::string_view source; ///< "none", "manual", "sntp", "console"
    std::int32_t drift_ppb = 0;
    time::UnixSeconds last_sync_utc = 0;
};

struct WeatherInfo {
    model::WeatherReport report{};
    model::WeatherFreshness freshness = model::WeatherFreshness::kHidden;
    std::uint32_t age_s = 0;
};

struct SyncInfo {
    model::SyncIndicator indicator = model::SyncIndicator::kNone;
    time::UnixSeconds next_time_sync = 0, next_weather = 0, last_ok = 0;
    std::uint8_t fail_streak = 0;
    std::uint8_t last_error = 0; ///< Errc + 1, 0 = none
};

class DeviceApi {
public:
    virtual ~DeviceApi() = default;
    // identity / status
    [[nodiscard]] virtual FirmwareIdentity firmware() const = 0;
    /// Writes the `status` object fields (app-defined composition) into an open object.
    virtual void write_status(JsonWriter& out) = 0;
    // time
    [[nodiscard]] virtual TimeInfo time_info() const = 0;
    [[nodiscard]] virtual const time::TimeZone& timezone() const = 0;
    virtual Status set_time_utc(time::UnixSeconds utc) = 0;
    // settings (tz set == settings set tz)
    [[nodiscard]] virtual const settings::Settings& current_settings() const = 0;
    virtual Status apply_setting(settings::Key key, std::string_view value) = 0;
    virtual Status reset_settings() = 0;
    // input / screens / faces
    virtual Status inject(const model::InputEvent& event) = 0;
    [[nodiscard]] virtual std::string_view current_screen() const = 0;
    virtual Status show_screen(std::string_view name) = 0;
    [[nodiscard]] virtual std::size_t face_count() const = 0;
    [[nodiscard]] virtual std::uint8_t face_id_at(std::size_t index) const = 0;
    [[nodiscard]] virtual std::string_view face_name(std::uint8_t id) const = 0;
    // steps / battery / weather
    [[nodiscard]] virtual model::StepsSummary steps() const = 0;
    virtual Status inject_steps(std::int32_t delta) = 0;
    virtual Status reset_steps_today() = 0;
    [[nodiscard]] virtual model::BatteryStatus battery() const = 0;
    virtual Status fake_battery_mv(std::optional<std::uint16_t> mv) = 0;
    [[nodiscard]] virtual WeatherInfo weather() const = 0;
    virtual Status fake_weather(const model::WeatherReport& report) = 0;
    virtual Status clear_weather() = 0;
    // radio (kUnsupported when compiled out)
    [[nodiscard]] virtual std::optional<FixedString<32>>
    wifi_ssid() const = 0; ///< never the password
    virtual Status set_wifi(std::string_view ssid, std::string_view password) = 0;
    virtual Status clear_wifi() = 0;
    virtual Status sync_now(bool time, bool weather) = 0; ///< blocking, bounded by session budget
    [[nodiscard]] virtual SyncInfo sync_info() const = 0;
    virtual Status start_provisioning(FixedString<32>& ssid_out, std::uint16_t& expires_s_out) = 0;
    virtual Status stop_provisioning() = 0;
    // display
    virtual Status refresh_display(bool full) = 0;
    [[nodiscard]] virtual std::span<const std::uint8_t>
    framebuffer() const = 0; ///< 5000 B, 1bpp MSB-first, 1 = black
    // logs / diagnostics / self-test
    [[nodiscard]] virtual std::size_t wake_record_count() const = 0;
    [[nodiscard]] virtual model::WakeRecord wake_record(std::size_t index_oldest_first) const = 0;
    virtual void clear_wake_log() = 0;
    /// page: "info", "power", "radio", "rtc", "nvs", "clock", "sensors". kNotFound for others.
    virtual Status write_diag(std::string_view page, JsonWriter& out) = 0;
    virtual void list_selftests(JsonWriter& out) = 0;
    virtual Status run_selftests(std::string_view filter, JsonWriter& out) = 0;
    virtual Status set_log_level(LogLevel level) = 0;
    // actuators / lifecycle (deferred until the response line is sent)
    virtual Status vibrate(std::uint16_t ms) = 0;
    virtual Status request_sleep(std::uint32_t seconds) = 0;
    virtual Status request_reboot() = 0;
    virtual Status factory_reset() = 0;
};

} // namespace qz::console
