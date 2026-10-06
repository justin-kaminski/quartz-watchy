// Test double: a DeviceApi that does nothing, for exercising the registry and the dispatcher
// without a device. (The catalog tests bring their own, fuller fake.) vibrate() is the one call
// tests observe: it proves that a handler received this very DeviceApi instance.
#pragma once

#include "qz/console/device_api.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::console::test {

class NullDeviceApi final : public DeviceApi {
public:
    [[nodiscard]] FirmwareIdentity firmware() const override { return {}; }
    void write_status(JsonWriter&) override {}
    [[nodiscard]] TimeInfo time_info() const override { return {}; }
    [[nodiscard]] const time::TimeZone& timezone() const override { return timezone_; }
    Status set_time_utc(time::UnixSeconds) override { return Errc::kUnsupported; }
    [[nodiscard]] const settings::Settings& current_settings() const override { return settings_; }
    Status apply_setting(settings::Key, std::string_view) override { return Errc::kUnsupported; }
    Status reset_settings() override { return Errc::kUnsupported; }
    Status inject(const model::InputEvent&) override { return Errc::kUnsupported; }
    [[nodiscard]] std::string_view current_screen() const override { return {}; }
    Status show_screen(std::string_view) override { return Errc::kUnsupported; }
    [[nodiscard]] std::size_t face_count() const override { return 0; }
    [[nodiscard]] std::uint8_t face_id_at(std::size_t) const override { return 0; }
    [[nodiscard]] std::string_view face_name(std::uint8_t) const override { return {}; }
    [[nodiscard]] model::StepsSummary steps() const override { return {}; }
    Status inject_steps(std::int32_t) override { return Errc::kUnsupported; }
    Status reset_steps_today() override { return Errc::kUnsupported; }
    [[nodiscard]] model::BatteryStatus battery() const override { return {}; }
    Status fake_battery_mv(std::optional<std::uint16_t>) override { return Errc::kUnsupported; }
    [[nodiscard]] WeatherInfo weather() const override { return {}; }
    Status fake_weather(const model::WeatherReport&) override { return Errc::kUnsupported; }
    Status clear_weather() override { return Errc::kUnsupported; }
    [[nodiscard]] std::optional<FixedString<32>> wifi_ssid() const override { return std::nullopt; }
    Status set_wifi(std::string_view, std::string_view) override { return Errc::kUnsupported; }
    Status clear_wifi() override { return Errc::kUnsupported; }
    Status sync_now(bool, bool) override { return Errc::kUnsupported; }
    [[nodiscard]] SyncInfo sync_info() const override { return {}; }
    Status start_provisioning(FixedString<32>&, std::uint16_t&) override {
        return Errc::kUnsupported;
    }
    Status stop_provisioning() override { return Errc::kUnsupported; }
    Status refresh_display(bool) override { return Errc::kUnsupported; }
    [[nodiscard]] std::span<const std::uint8_t> framebuffer() const override { return {}; }
    [[nodiscard]] std::size_t wake_record_count() const override { return 0; }
    [[nodiscard]] model::WakeRecord wake_record(std::size_t) const override { return {}; }
    void clear_wake_log() override {}
    Status write_diag(std::string_view, JsonWriter&) override { return Errc::kNotFound; }
    void list_selftests(JsonWriter&) override {}
    Status run_selftests(std::string_view, JsonWriter&) override { return Errc::kUnsupported; }
    Status set_log_level(LogLevel) override { return Errc::kUnsupported; }
    Status vibrate(std::uint16_t ms) override {
        ++vibrate_calls;
        last_vibrate_ms = ms;
        return ok();
    }
    Status request_sleep(std::uint32_t) override { return Errc::kUnsupported; }
    Status request_reboot() override { return Errc::kUnsupported; }
    Status factory_reset() override { return Errc::kUnsupported; }

    std::uint32_t vibrate_calls = 0;
    std::uint16_t last_vibrate_ms = 0;

private:
    time::TimeZone timezone_;
    settings::Settings settings_;
};

} // namespace qz::console::test
