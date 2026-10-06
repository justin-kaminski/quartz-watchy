// Test double for the command catalog: a DeviceApi with real (small) state behind it, so a command
// that mutates something can be checked by asking for it again, plus call records for the actuators
// and a switch that makes every mutating call fail with a chosen error.
#pragma once

#include "qz/console/device_api.hpp"
#include "qz/core/fixed_string.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qz::console::test {

class FakeDeviceApi final : public DeviceApi {
public:
    FakeDeviceApi() {
        steps_.today = 4321;
        steps_.goal = 8000;
        steps_.history[0] = {20'000, 9000}; // 2024-10-04
        steps_.history[1] = {19'999, 7500};
        steps_.history[2] = {19'998, 0};
        steps_.history_count = 3;
        battery_.mv = 3900;
        battery_.percent = 70;
        battery_.valid = true;
        for (std::size_t i = 0; i < frame_.size(); ++i) {
            frame_[i] = static_cast<std::uint8_t>(i * 7U + 3U);
        }
        for (std::uint8_t i = 0; i < 3; ++i) {
            model::WakeRecord record;
            record.start_utc_s = 1'000'000U + i;
            record.awake_ms = static_cast<std::uint16_t>(120 + i);
            record.cause = i == 1 ? model::WakeCause::kButton : model::WakeCause::kTimer;
            record.battery_mv = 3900;
            record.error =
                i == 2 ? static_cast<std::uint8_t>(static_cast<unsigned>(Errc::kTimeout) + 1U) : 0U;
            wakes.push_back(record);
        }
    }

    // ---- knobs and records, public for the tests ----
    FirmwareIdentity identity{"1.2.3", "abc1234", "v6.1", true};
    std::optional<Error> fail; ///< every mutating call returns this error while it is set
    std::vector<model::InputEvent> injected;
    std::vector<model::WakeRecord> wakes;
    std::vector<std::string> screens{"face", "menu", "about"};
    std::string password_seen; ///< what set_wifi received, so tests can look for it in the output
    std::vector<std::uint16_t> vibrations;
    std::vector<std::uint32_t> sleeps;
    std::vector<std::pair<bool, bool>> syncs; ///< (time, weather) per sync_now call
    std::vector<bool> refreshes;              ///< `full` per refresh_display call
    std::optional<LogLevel> log_level_set;
    std::size_t frame_bytes = 5000; ///< framebuffer() size (shrink it to simulate a broken panel)
    bool reboot_requested = false;
    bool factory_reset_done = false;
    bool provisioning = false;
    bool wake_log_cleared = false;
    bool wifi_password_stored = false;

    // ---- DeviceApi ----
    [[nodiscard]] FirmwareIdentity firmware() const override { return identity; }
    void write_status(JsonWriter& out) override {
        out.field_bool("valid", time_.valid);
        out.field("steps", static_cast<std::int64_t>(steps_.today));
        out.key("battery").begin_object();
        out.field("mv", battery_.mv);
        out.end_object();
        out.field("screen", screen_);
    }
    [[nodiscard]] TimeInfo time_info() const override { return time_; }
    [[nodiscard]] const time::TimeZone& timezone() const override { return zone_; }
    Status set_time_utc(time::UnixSeconds utc) override {
        QZ_RETURN_IF_ERROR(gate());
        time_.valid = true;
        time_.utc_us = utc * time::kUsPerSecond;
        time_.local = zone_.to_local(utc);
        time_.source = "console";
        return ok();
    }
    [[nodiscard]] const settings::Settings& current_settings() const override { return settings_; }
    Status apply_setting(settings::Key key, std::string_view value) override {
        QZ_RETURN_IF_ERROR(gate());
        settings::Settings next = settings_;
        QZ_RETURN_IF_ERROR(settings::set_from_string(next, key, value));
        settings_ = next;
        const Result<time::TimeZone> zone = time::TimeZone::parse(settings_.tz_posix.view());
        if (zone) {
            zone_ = *zone;
        }
        return ok();
    }
    Status reset_settings() override {
        QZ_RETURN_IF_ERROR(gate());
        settings_ = settings::defaults();
        return ok();
    }
    Status inject(const model::InputEvent& event) override {
        QZ_RETURN_IF_ERROR(gate());
        injected.push_back(event);
        if (event.button == model::Button::kMenu) {
            screen_ = "menu";
        } else if (event.button == model::Button::kBack) {
            screen_ = "face";
        }
        return ok();
    }
    [[nodiscard]] std::string_view current_screen() const override { return screen_; }
    Status show_screen(std::string_view name) override {
        QZ_RETURN_IF_ERROR(gate());
        for (const std::string& known : screens) {
            if (known == name) {
                screen_ = known;
                return ok();
            }
        }
        return Errc::kNotFound;
    }
    [[nodiscard]] std::size_t screen_count() const override { return screens.size(); }
    [[nodiscard]] std::string_view screen_name_at(std::size_t index) const override {
        return index < screens.size() ? std::string_view(screens[index]) : std::string_view{};
    }
    [[nodiscard]] std::size_t face_count() const override { return 2; }
    [[nodiscard]] std::uint8_t face_id_at(std::size_t index) const override {
        return index == 0 ? 0 : 3; // ids need not be dense
    }
    [[nodiscard]] std::string_view face_name(std::uint8_t id) const override {
        return id == 0 ? "classic" : id == 3 ? "minimal" : "";
    }
    [[nodiscard]] model::StepsSummary steps() const override { return steps_; }
    Status inject_steps(std::int32_t delta) override {
        QZ_RETURN_IF_ERROR(gate());
        const std::int64_t next = static_cast<std::int64_t>(steps_.today) + delta;
        steps_.today = next < 0 ? 0U : static_cast<std::uint32_t>(next);
        return ok();
    }
    Status reset_steps_today() override {
        QZ_RETURN_IF_ERROR(gate());
        steps_.today = 0;
        return ok();
    }
    [[nodiscard]] model::BatteryStatus battery() const override { return battery_; }
    Status fake_battery_mv(std::optional<std::uint16_t> mv) override {
        QZ_RETURN_IF_ERROR(gate());
        battery_.faked = mv.has_value();
        battery_.mv = mv.value_or(3900);
        return ok();
    }
    [[nodiscard]] WeatherInfo weather() const override { return weather_; }
    Status fake_weather(const model::WeatherReport& report) override {
        QZ_RETURN_IF_ERROR(gate());
        weather_.report = report;
        weather_.freshness = model::WeatherFreshness::kFresh;
        weather_.age_s = 0;
        return ok();
    }
    Status clear_weather() override {
        QZ_RETURN_IF_ERROR(gate());
        weather_ = WeatherInfo{};
        return ok();
    }
    [[nodiscard]] std::optional<FixedString<32>> wifi_ssid() const override { return ssid_; }
    [[nodiscard]] bool wifi_has_password() const override { return wifi_password_stored; }
    Status set_wifi(std::string_view ssid, std::string_view password) override {
        QZ_RETURN_IF_ERROR(gate());
        FixedString<32> stored;
        if (!stored.assign(ssid)) {
            return Errc::kBadArgs;
        }
        ssid_ = stored;
        password_seen = std::string(password);
        wifi_password_stored = !password.empty();
        return ok();
    }
    Status clear_wifi() override {
        QZ_RETURN_IF_ERROR(gate());
        ssid_.reset();
        wifi_password_stored = false;
        return ok();
    }
    Status sync_now(bool time, bool weather) override {
        QZ_RETURN_IF_ERROR(gate());
        syncs.emplace_back(time, weather);
        sync_.indicator = model::SyncIndicator::kOk;
        sync_.last_ok = 1'700'000'000;
        sync_.next_time_sync = 1'700'086'400;
        sync_.next_weather = 1'700'003'600;
        return ok();
    }
    [[nodiscard]] SyncInfo sync_info() const override { return sync_; }
    Status start_provisioning(FixedString<32>& ssid_out, std::uint16_t& expires_s_out) override {
        QZ_RETURN_IF_ERROR(gate());
        provisioning = true;
        ssid_out = FixedString<32>("QZ-A1B2");
        expires_s_out = 300;
        return ok();
    }
    Status stop_provisioning() override {
        QZ_RETURN_IF_ERROR(gate());
        if (!provisioning) {
            return Errc::kInvalidState;
        }
        provisioning = false;
        return ok();
    }
    Status refresh_display(bool full) override {
        QZ_RETURN_IF_ERROR(gate());
        refreshes.push_back(full);
        return ok();
    }
    [[nodiscard]] std::span<const std::uint8_t> framebuffer() const override {
        return {frame_.data(), frame_bytes};
    }
    [[nodiscard]] std::size_t wake_record_count() const override { return wakes.size(); }
    [[nodiscard]] model::WakeRecord wake_record(std::size_t index) const override {
        return index < wakes.size() ? wakes[index] : model::WakeRecord{};
    }
    void clear_wake_log() override {
        wake_log_cleared = true;
        wakes.clear();
    }
    Status write_diag(std::string_view page, JsonWriter& out) override {
        for (const std::string_view known :
             {"info", "power", "radio", "rtc", "nvs", "clock", "sensors"}) {
            if (known == page) {
                out.field("page", page);
                out.field_bool("ok", true);
                return ok();
            }
        }
        return Errc::kNotFound;
    }
    void list_selftests(JsonWriter& out) override {
        out.begin_array("tests");
        out.str("battery.thresholds");
        out.str("power.policy");
        out.end_array();
    }
    Status run_selftests(std::string_view filter, JsonWriter& out) override {
        QZ_RETURN_IF_ERROR(gate());
        if (!filter.empty() && filter != "battery" && filter != "battery.thresholds") {
            return Errc::kNotFound;
        }
        out.begin_array("results");
        out.begin_object();
        out.field("name", "battery.thresholds");
        out.field("status", "pass");
        out.field("ms", 3);
        out.field("detail", "");
        out.end_object();
        out.end_array();
        return ok();
    }
    Status set_log_level(LogLevel level) override {
        QZ_RETURN_IF_ERROR(gate());
        log_level_set = level;
        return ok();
    }
    Status vibrate(std::uint16_t ms) override {
        QZ_RETURN_IF_ERROR(gate());
        vibrations.push_back(ms);
        return ok();
    }
    Status request_sleep(std::uint32_t seconds) override {
        QZ_RETURN_IF_ERROR(gate());
        sleeps.push_back(seconds);
        return ok();
    }
    Status request_reboot() override {
        QZ_RETURN_IF_ERROR(gate());
        reboot_requested = true;
        return ok();
    }
    Status factory_reset() override {
        QZ_RETURN_IF_ERROR(gate());
        factory_reset_done = true;
        return ok();
    }

    // ---- state setters for the tests ----
    void set_clock(time::UnixSeconds utc, std::string_view source = "sntp") {
        time_.valid = true;
        time_.utc_us = utc * time::kUsPerSecond;
        time_.local = zone_.to_local(utc);
        time_.source = source;
    }
    void set_weather(const WeatherInfo& weather) { weather_ = weather; }
    void set_time_info(const TimeInfo& info) { time_ = info; }
    void set_sync_info(const SyncInfo& info) { sync_ = info; }
    void set_steps(const model::StepsSummary& steps) { steps_ = steps; }
    void set_battery(const model::BatteryStatus& battery) { battery_ = battery; }
    void set_ssid(const FixedString<32>& ssid, bool has_password) {
        ssid_ = ssid;
        wifi_password_stored = has_password;
    }

private:
    [[nodiscard]] Status gate() const {
        if (fail) {
            return *fail;
        }
        return ok();
    }

    TimeInfo time_{};
    time::TimeZone zone_;
    settings::Settings settings_;
    model::StepsSummary steps_{};
    model::BatteryStatus battery_{};
    WeatherInfo weather_{};
    SyncInfo sync_{};
    std::optional<FixedString<32>> ssid_;
    std::string screen_{"face"};
    std::array<std::uint8_t, 5000> frame_{};
};

} // namespace qz::console::test
