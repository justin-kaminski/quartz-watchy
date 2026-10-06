// console::DeviceApi on top of the Core (ARCHITECTURE.md section 16). Every mutating call commits
// the RTC state so the simulator and tests may drive it between wakes.
#include "app_core.hpp"
#include "qz/console/protocol.hpp"
#include "qz/core/log.hpp"
#include "qz/power/power.hpp"
#include "qz/time/civil.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace qz::app {

namespace {

constexpr time::UnixSeconds kMinPlausibleUtc = 1'600'000'000; // 2020-09
constexpr time::UnixSeconds kMaxPlausibleUtc = 7'258'118'400; // 2200-01-01

std::string_view source_name(time::TimeSource source) noexcept {
    switch (source) {
        case time::TimeSource::kNone:
            return "none";
        case time::TimeSource::kManual:
            return "manual";
        case time::TimeSource::kSntp:
            return "sntp";
        case time::TimeSource::kConsole:
            return "console";
    }
    return "none";
}

} // namespace

console::FirmwareIdentity Core::firmware() const {
    const hal::FirmwareInfo fw = p_.system.firmware();
    return console::FirmwareIdentity{fw.version, fw.git_hash, fw.idf_version, radio_compiled()};
}

console::TimeInfo Core::time_info() const {
    console::TimeInfo info;
    info.valid = keeper_.valid();
    info.source = source_name(rtc_.time.source);
    info.drift_ppb = rtc_.time.drift_ppb;
    info.last_sync_utc = rtc_.time.last_sync_utc_us / time::kUsPerSecond;
    if (info.valid) {
        info.utc_us = keeper_.now_utc_us();
        info.local = tz_.to_local(info.utc_us / time::kUsPerSecond);
    }
    return info;
}

const time::TimeZone& Core::timezone() const {
    return tz_;
}

const settings::Settings& Core::current_settings() const {
    return rtc_.settings;
}

void Core::write_status(console::JsonWriter& out) {
    const console::TimeInfo ti = time_info();
    out.field_bool("valid", ti.valid);
    if (ti.valid) {
        std::array<char, 32> text{};
        const std::size_t n = time::format_iso8601_utc(text, ti.utc_us / time::kUsPerSecond);
        out.field("utc", std::string_view(text.data(), n));
        const int m = std::snprintf(text.data(),
                                    text.size(),
                                    "%04d-%02u-%02uT%02u:%02u:%02u",
                                    static_cast<int>(ti.local.date.year),
                                    unsigned{ti.local.date.month},
                                    unsigned{ti.local.date.day},
                                    unsigned{ti.local.time.hour},
                                    unsigned{ti.local.time.minute},
                                    unsigned{ti.local.time.second});
        out.field("local", std::string_view(text.data(), m > 0 ? static_cast<std::size_t>(m) : 0));
    }
    out.field("tz", rtc_.settings.tz_name.view());
    out.field("steps", static_cast<std::int64_t>(steps_.summary(rtc_.settings.step_goal).today));

    const model::BatteryStatus bat = battery();
    out.key("battery").begin_object();
    out.field("mv", static_cast<std::int64_t>(bat.mv));
    out.field("pct", static_cast<std::int64_t>(bat.percent));
    out.field("state", level_name(bat.level));
    out.field_bool("usb", bat.usb_present);
    out.field_bool("charging", bat.charging);
    out.end_object();

    const console::SyncInfo sync = sync_info();
    out.key("conn").begin_object();
    out.field(
        "mode",
        mode_name(radio_compiled() ? rtc_.settings.connectivity : model::ConnectivityMode::kOff));
    out.field("last_sync", ti.last_sync_utc);
    out.field("indicator", indicator_name(sync.indicator));
    out.end_object();

    const console::WeatherInfo wx = weather();
    out.key("weather").begin_object();
    out.field("age_s", static_cast<std::int64_t>(wx.age_s));
    out.field("state", freshness_name(wx.freshness));
    out.end_object();

    out.field("screen", current_screen());
    out.field("power_state", level_name(power_.level()));
}

Status Core::set_time_utc(time::UnixSeconds utc) {
    if (utc < kMinPlausibleUtc || utc >= kMaxPlausibleUtc) {
        return Errc::kBadArgs;
    }
    set_time_impl(utc, time::TimeSource::kConsole);
    (void)render_and_present(false, 0); // the face must show the new time
    commit_all();
    return ok();
}

Status Core::apply_setting(settings::Key key, std::string_view value) {
    const Status s = apply_setting_impl(key, value);
    if (s) {
        (void)render_and_present(false, 0);
    }
    commit_all();
    return s;
}

Status Core::reset_settings() {
    const Status s = apply_settings_object(settings::defaults());
    if (s) {
        (void)render_and_present(false, 0);
    }
    commit_all();
    return s;
}

Status Core::inject(const model::InputEvent& event) {
    handle_event(event);
    commit_all();
    return ok();
}

std::string_view Core::current_screen() const {
    return ui::screen_name(ui_.current());
}

Status Core::show_screen(std::string_view name) {
    const Result<ui::ScreenId> id = ui::screen_from_name(name);
    if (!id) {
        return id.error();
    }
    QZ_RETURN_IF_ERROR(ui_.show(*id, build_state(display_utc_s())));
    (void)render_and_present(false, 0);
    commit_all();
    return ok();
}

std::size_t Core::screen_count() const {
    return static_cast<std::size_t>(ui::ScreenId::kCount);
}

std::string_view Core::screen_name_at(std::size_t index) const {
    if (index >= screen_count()) {
        return {};
    }
    return ui::screen_name(static_cast<ui::ScreenId>(index));
}

std::size_t Core::face_count() const {
    return faces_.count();
}

std::uint8_t Core::face_id_at(std::size_t index) const {
    return faces_.id_at(index);
}

std::string_view Core::face_name(std::uint8_t id) const {
    return faces_.name_of(id);
}

model::StepsSummary Core::steps() const {
    return steps_.summary(rtc_.settings.step_goal);
}

Status Core::inject_steps(std::int32_t delta) {
    steps_.inject(delta);
    commit_all();
    return ok();
}

Status Core::reset_steps_today() {
    steps_.reset_today();
    commit_all();
    return ok();
}

model::BatteryStatus Core::battery() const {
    return power_.status(p_.io.usb_present(), p_.io.charging());
}

Status Core::fake_battery_mv(std::optional<std::uint16_t> mv) {
    const model::PowerLevel before = power_.level();
    if (mv.has_value()) {
        if (*mv == 0) {
            return Errc::kBadArgs;
        }
        rtc_.power.fake_mv = *mv;
        take_fake_sample();
    } else {
        rtc_.power.fake_mv = 0;
        adc_sample(); // back to the real cell right away
    }
    if (before != model::PowerLevel::kCritical && power_.level() == model::PowerLevel::kCritical) {
        flush_steps(); // entering Critical flushes the steps (ARCHITECTURE.md section 11)
    }
    commit_all();
    return ok();
}

console::WeatherInfo Core::weather() const {
    console::WeatherInfo info;
    info.report = rtc_.weather;
    const time::UnixSeconds now = keeper_.valid() ? now_utc_s() : 0;
    info.freshness = weather_freshness(now);
    if (keeper_.valid() && rtc_.weather.valid != 0 && now >= rtc_.weather.fetched_utc) {
        info.age_s = static_cast<std::uint32_t>(
            std::min<std::int64_t>(now - rtc_.weather.fetched_utc, UINT32_MAX));
    }
    return info;
}

Status Core::fake_weather(const model::WeatherReport& report) {
    rtc_.weather = report;
    rtc_.weather.valid = 1;
    rtc_.weather.faked = 1;
    if (rtc_.weather.fetched_utc == 0 && keeper_.valid()) {
        rtc_.weather.fetched_utc = now_utc_s();
    }
    commit_all();
    return ok();
}

Status Core::clear_weather() {
    rtc_.weather = model::WeatherReport{};
    commit_all();
    return ok();
}

std::optional<FixedString<32>> Core::wifi_ssid() const {
    if (!radio_compiled()) {
        return std::nullopt;
    }
    const Result<hal::WifiCredentials> creds = cred_store_.load();
    if (!creds) {
        return std::nullopt;
    }
    return creds->ssid;
}

bool Core::wifi_has_password() const {
    if (!radio_compiled()) {
        return false;
    }
    const Result<hal::WifiCredentials> creds = cred_store_.load();
    return creds && !creds->password.empty();
}

Status Core::set_wifi(std::string_view ssid, std::string_view password) {
    if (!radio_compiled()) {
        return Errc::kUnsupported;
    }
    hal::WifiCredentials creds;
    if (!creds.ssid.assign(ssid) || !creds.password.assign(password)) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(settings::validate_credentials(creds));
    QZ_RETURN_IF_ERROR(cred_store_.save(creds));
    set_creds_flag(true);
    sched_.on_config_changed();
    commit_all();
    return ok();
}

Status Core::clear_wifi() {
    if (!radio_compiled()) {
        return Errc::kUnsupported;
    }
    QZ_RETURN_IF_ERROR(cred_store_.clear());
    set_creds_flag(false);
    commit_all();
    return ok();
}

Status Core::sync_now(bool time, bool weather) {
    const Status s = run_sync(time, weather);
    if (!s) {
        commit_all();
        return s;
    }
    refresh_after_radio(); // SNTP may have moved the clock; the indicator changed
    commit_all();
    return s;
}

console::SyncInfo Core::sync_info() const {
    console::SyncInfo info;
    const std::int64_t last_sync_s = rtc_.time.last_sync_utc_us / time::kUsPerSecond;
    info.indicator = radio_compiled() ? sched_.indicator(make_inputs(false), last_sync_s)
                                      : model::SyncIndicator::kNone;
    info.next_time_sync = rtc_.conn.next_time_sync;
    info.next_weather = rtc_.conn.next_weather;
    info.last_ok = rtc_.conn.last_ok_utc;
    info.fail_streak = rtc_.conn.fail_streak;
    info.last_error = rtc_.conn.last_error;
    return info;
}

Status Core::start_provisioning(FixedString<32>& ssid_out, std::uint16_t& expires_s_out) {
    return start_provisioning_impl(ssid_out, expires_s_out);
}

Status Core::stop_provisioning() {
    stop_provisioning_impl();
    return ok();
}

Status Core::refresh_display(bool full) {
    const Status s = render_and_present(full, 0, true);
    commit_all();
    return s;
}

std::span<const std::uint8_t> Core::framebuffer() const {
    return shown_.bytes();
}

std::size_t Core::wake_record_count() const {
    return rtc_.wake_log.size();
}

model::WakeRecord Core::wake_record(std::size_t index_oldest_first) const {
    if (index_oldest_first >= rtc_.wake_log.size()) {
        return model::WakeRecord{};
    }
    return rtc_.wake_log[index_oldest_first];
}

void Core::clear_wake_log() {
    rtc_.wake_log.clear();
    commit_all();
}

Status Core::write_diag(std::string_view page, console::JsonWriter& out) {
    if (page == "info") {
        const hal::HeapInfo heap = p_.system.heap();
        out.field("reset", reset_name(w_.reason));
        out.field("wake", cause_name(w_.cause));
        out.field("boots", static_cast<std::int64_t>(rtc_.header.boot_count));
        out.field("face", static_cast<std::int64_t>(rtc_.face_id));
        out.field("heap_free", static_cast<std::int64_t>(heap.free_bytes));
        out.field("heap_min", static_cast<std::int64_t>(heap.min_free_bytes));
        out.field_bool("safe_mode", rtc_.wake.safe_mode != 0);
        out.field("tether", static_cast<std::int64_t>(tether_.state()));
        return ok();
    }
    if (page == "power") {
        const power::PowerState& ps = rtc_.power;
        out.field("level", level_name(power_.level()));
        out.field("filtered_mv", static_cast<std::int64_t>(ps.filtered_mv));
        out.field_bool("faked", ps.fake_mv != 0);
        out.field("awake_ms_today", static_cast<std::int64_t>(ps.awake_ms_today));
        out.begin_array("wakes_today");
        for (const std::uint16_t n : ps.wakes_today) {
            out.num(n);
        }
        out.end_array();
        power::EstimateInputs est;
        est.awake_ms_per_day = ps.awake_ms_today;
        out.field("estimate_h", static_cast<std::int64_t>(power::estimate_hours(est)));
        return ok();
    }
    if (page == "radio") {
        out.field_bool("compiled", radio_compiled());
        out.field("inits",
                  static_cast<std::int64_t>(radio_compiled() ? p_.net->radio_init_count() : 0));
        out.field("sessions", static_cast<std::int64_t>(rtc_.conn.sessions));
        out.field("fail_streak", static_cast<std::int64_t>(rtc_.conn.fail_streak));
        out.field("last_error", static_cast<std::int64_t>(rtc_.conn.last_error));
        return ok();
    }
    if (page == "rtc") {
        out.field("version", static_cast<std::int64_t>(kRtcStateVersion));
        out.field("size", static_cast<std::int64_t>(sizeof(RtcState)));
        out.field("boots", static_cast<std::int64_t>(rtc_.header.boot_count));
        out.field("scheduled_wake_rtc_us", rtc_.wake.scheduled_wake_rtc_us);
        out.field("latency_us", static_cast<std::int64_t>(rtc_.wake.ewma_latency_us));
        out.field("crashes", static_cast<std::int64_t>(rtc_.wake.crash_count_window));
        out.field_bool("safe_mode", rtc_.wake.safe_mode != 0);
        out.field_bool("frame_valid", rtc_.display.frame_valid != 0);
        out.field("partials", static_cast<std::int64_t>(rtc_.display.partials_since_full));
        return ok();
    }
    if (page == "nvs") {
        out.field_bool("settings_cached", rtc_.settings_valid != 0);
        out.field_bool("creds", has_creds_flag());
        out.field("steps_flush_day", static_cast<std::int64_t>(rtc_.steps.last_flush_day));
        out.field("wear", "writes only on change; none on the minute path");
        return ok();
    }
    if (page == "clock") {
        const hal::SlowClockInfo slow = p_.system.slow_clock();
        out.field("rtc_us", p_.clock.rtc_us());
        out.field_bool("valid", keeper_.valid());
        out.field("utc_us", keeper_.valid() ? keeper_.now_utc_us() : 0);
        out.field("drift_ppb", static_cast<std::int64_t>(rtc_.time.drift_ppb));
        out.field("source", source_name(rtc_.time.source));
        out.field_bool("degraded", rtc_.time.clock_degraded != 0);
        out.field_bool("crystal", slow.external_crystal);
        out.field("slow_hz", static_cast<std::int64_t>(slow.measured_hz));
        return ok();
    }
    if (page == "sensors") {
        const Result<std::uint32_t> count = accel_.step_count();
        if (count) {
            out.field("step_counter", static_cast<std::int64_t>(*count));
        } else {
            out.key("step_counter").null();
        }
        out.field("panel_temp_dc", static_cast<std::int64_t>(rtc_.display.panel_temp_dc));
        out.field("buttons", static_cast<std::int64_t>(p_.io.pressed_buttons()));
        out.field_bool("usb", p_.io.usb_present());
        out.field_bool("charging", p_.io.charging());
        out.field_bool("accel_ok", accel_ok_);
        return ok();
    }
    return Errc::kNotFound;
}

void Core::list_selftests(console::JsonWriter& out) {
    out.begin_array("tests");
    const std::span<const selftest::TestCase> tests = selftest::all_tests();
    for (const selftest::TestCase& test : tests) {
        out.begin_object();
        out.field("suite", test.suite);
        out.field("name", test.name);
        out.field_bool("interactive", test.interactive);
        out.end_object();
    }
    out.end_array();
}

Status Core::run_selftests(std::string_view filter, console::JsonWriter& out) {
    const Status s = run_selftests_impl(filter, &out);
    commit_all();
    return s;
}

Status Core::set_log_level(LogLevel level) {
    ::qz::set_log_level(level);
    return ok();
}

Status Core::vibrate(std::uint16_t ms) {
    return vibrate_impl(ms);
}

Status Core::request_sleep(std::uint32_t seconds) {
    if (seconds == 0 || seconds > wiring::kSleepMaxS) {
        return Errc::kBadArgs;
    }
    pending_sleep_s_ = seconds; // honoured by the tethered loop after the response is sent
    return ok();
}

Status Core::request_reboot() {
    pending_reboot_ = true;
    return ok();
}

Status Core::factory_reset() {
    const Status s = factory_reset_impl();
    (void)render_and_present(true, 0);
    commit_all();
    return s;
}

} // namespace qz::app
