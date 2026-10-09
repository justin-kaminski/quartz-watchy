// WatchState snapshot builder (ARCHITECTURE.md section 15): everything a screen or face may show,
// derived from the RTC state and services once per render.
#include "app_core.hpp"

#include <algorithm>

namespace qz::app {

std::string_view level_name(model::PowerLevel level) noexcept {
    switch (level) {
        case model::PowerLevel::kNormal:
            return "normal";
        case model::PowerLevel::kLow:
            return "low";
        case model::PowerLevel::kSaver:
            return "saver";
        case model::PowerLevel::kCritical:
            return "critical";
    }
    return "normal";
}

std::string_view indicator_name(model::SyncIndicator indicator) noexcept {
    switch (indicator) {
        case model::SyncIndicator::kNone:
            return "none";
        case model::SyncIndicator::kNeverSynced:
            return "never";
        case model::SyncIndicator::kLastFailed:
            return "failed";
        case model::SyncIndicator::kStale:
            return "stale";
        case model::SyncIndicator::kOk:
            return "ok";
    }
    return "none";
}

std::string_view freshness_name(model::WeatherFreshness freshness) noexcept {
    switch (freshness) {
        case model::WeatherFreshness::kFresh:
            return "fresh";
        case model::WeatherFreshness::kStale:
            return "stale";
        case model::WeatherFreshness::kHidden:
            return "hidden";
    }
    return "hidden";
}

std::string_view mode_name(model::ConnectivityMode mode) noexcept {
    switch (mode) {
        case model::ConnectivityMode::kOff:
            return "off";
        case model::ConnectivityMode::kTimeOnly:
            return "time";
        case model::ConnectivityMode::kTimeWeather:
            return "time+weather";
    }
    return "off";
}

std::string_view reset_name(hal::ResetReason reason) noexcept {
    switch (reason) {
        case hal::ResetReason::kPowerOn:
            return "poweron";
        case hal::ResetReason::kBrownout:
            return "brownout";
        case hal::ResetReason::kDeepSleep:
            return "deepsleep";
        case hal::ResetReason::kSoftware:
            return "software";
        case hal::ResetReason::kPanic:
            return "panic";
        case hal::ResetReason::kWatchdog:
            return "watchdog";
        case hal::ResetReason::kUsbJtag:
            return "usbjtag";
        case hal::ResetReason::kOther:
            return "other";
    }
    return "other";
}

std::string_view cause_name(model::WakeCause cause) noexcept {
    switch (cause) {
        case model::WakeCause::kColdBoot:
            return "cold";
        case model::WakeCause::kReset:
            return "reset";
        case model::WakeCause::kTimer:
            return "timer";
        case model::WakeCause::kButton:
            return "button";
        case model::WakeCause::kAccel:
            return "accel";
        case model::WakeCause::kUsb:
            return "usb";
        case model::WakeCause::kTetheredTick:
            return "tick";
        case model::WakeCause::kUnknown:
            break;
    }
    return "unknown";
}

bool Core::radio_compiled() const noexcept {
    return f_.radio && p_.net != nullptr;
}

bool Core::phone_compiled() const noexcept {
    return f_.phone && p_.phone != nullptr;
}

time::UnixSeconds Core::now_utc_s() const noexcept {
    return keeper_.now_utc_us() / time::kUsPerSecond;
}

time::UnixSeconds Core::display_utc_s() const noexcept {
    return keeper_.valid() ? now_utc_s() : 0;
}

std::optional<time::DayNumber> Core::local_day(time::UnixSeconds utc_s) const noexcept {
    if (!keeper_.valid()) {
        return std::nullopt;
    }
    return time::days_from_civil(tz_.to_local(utc_s).date);
}

void Core::refresh_tz() noexcept {
    const Result<time::TimeZone> zone = time::TimeZone::parse(rtc_.settings.tz_posix.view());
    tz_ = zone ? *zone : time::TimeZone{};
}

model::WeatherFreshness Core::weather_freshness(time::UnixSeconds now_s) const noexcept {
    const model::WeatherReport& report = rtc_.weather;
    if (report.valid == 0) {
        return model::WeatherFreshness::kHidden;
    }
    // "Time only" means the owner turned weather off: hide real reports. Off only means no Wi-Fi;
    // a report pushed by the phone (section 13a) still shows, and ages out on its own.
    if (rtc_.settings.connectivity == model::ConnectivityMode::kTimeOnly && report.faked == 0) {
        return model::WeatherFreshness::kHidden;
    }
    return weather::freshness(report, now_s, rtc_.settings.weather_interval_min, keeper_.valid());
}

conn::Inputs Core::make_inputs(bool manual) const noexcept {
    conn::Inputs in;
    in.mode = rtc_.settings.connectivity;
    in.radio_compiled = radio_compiled();
    in.has_credentials = has_creds_flag();
    in.location_set = rtc_.settings.location_set;
    in.power = power_.level();
    in.time_valid = keeper_.valid();
    in.now_utc = in.time_valid ? now_utc_s() : 0;
    in.sync_interval_h = rtc_.settings.sync_interval_h;
    in.weather_interval_min = rtc_.settings.weather_interval_min;
    in.manual_request = manual;
    return in;
}

const ui::WatchState& Core::build_state(time::UnixSeconds utc_s) noexcept {
    ui::WatchState& st = state_;
    st = ui::WatchState{};
    const settings::Settings& cfg = rtc_.settings;
    const bool valid = keeper_.valid();

    st.time_valid = valid;
    if (valid) {
        st.local = tz_.to_local(utc_s);
    }
    st.hour_format = cfg.hour_format;
    st.tz_label = tz_.abbreviation(st.local.is_dst);

    st.steps = steps_.summary(cfg.step_goal);
    st.battery = power_.status(p_.io.usb_present(), p_.io.charging());
    st.weather = rtc_.weather;
    st.weather_freshness = weather_freshness(utc_s);
    st.temp_unit = cfg.temp_unit;
    st.weather_high_low = cfg.weather_high_low;
    if (valid && rtc_.weather.valid != 0 && utc_s >= rtc_.weather.fetched_utc) {
        const std::int64_t age = utc_s - rtc_.weather.fetched_utc;
        st.weather_age_s = static_cast<std::uint32_t>(std::min<std::int64_t>(age, UINT32_MAX));
    }

    const bool radio = radio_compiled();
    st.sync = sched_.indicator(make_inputs(false), rtc_.time.last_sync_utc_us / time::kUsPerSecond);
    st.conn_mode = radio ? cfg.connectivity : model::ConnectivityMode::kOff;
    st.radio_available = radio;
    st.has_credentials = has_creds_flag();
    st.last_sync_utc = rtc_.time.last_sync_utc_us / time::kUsPerSecond;
    st.op_phase = op_phase_;
    st.op_error = op_error_;

    if (prov_active_) {
        st.prov_ssid = prov_.ssid();
        st.prov_password = prov_.password().reveal(); // shown on the watch only
        const std::int64_t left_us =
            prov_started_rtc_us_ + (wiring::kProvisioningTtlS * wiring::kUs) - p_.clock.rtc_us();
        st.prov_seconds_left = static_cast<std::uint16_t>(
            std::clamp<std::int64_t>(left_us / wiring::kUs, 0, wiring::kProvisioningTtlS));
    }

    st.phone_available = phone_compiled();
    if (phone_active_) {
        switch (p_.phone->state()) {
            case hal::PhoneLinkState::kOff:
                st.phone_phase = ui::PhonePhase::kIdle;
                break;
            case hal::PhoneLinkState::kAdvertising:
                st.phone_phase = ui::PhonePhase::kAdvertising;
                break;
            case hal::PhoneLinkState::kPairing:
                st.phone_phase = ui::PhonePhase::kPairing;
                st.phone_passkey = p_.phone->passkey(); // shown on the watch only, never logged
                break;
            case hal::PhoneLinkState::kSecure:
                st.phone_phase = ui::PhonePhase::kConnected;
                break;
        }
        st.phone_name = phone_name_.view();
    }

    st.power = power_.level();
    st.tethered = tether_.state() == TetherState::kTethered;
    const hal::FirmwareInfo fw = p_.system.firmware();
    st.fw_version = fw.version;
    st.git_hash = fw.git_hash;
    st.idf_version = fw.idf_version;
    st.drift_ppb = rtc_.time.drift_ppb;
    st.clock_degraded = rtc_.time.clock_degraded != 0;
    st.awake_ms_today = rtc_.power.awake_ms_today;
    std::uint32_t wakes = 0;
    for (const std::uint16_t n : rtc_.power.wakes_today) {
        wakes += n;
    }
    st.wakes_today = static_cast<std::uint16_t>(std::min<std::uint32_t>(wakes, UINT16_MAX));

    const std::size_t total = rtc_.wake_log.size();
    const std::size_t count = std::min(total, recent_.size());
    for (std::size_t i = 0; i < count; ++i) {
        recent_[i] = rtc_.wake_log[total - count + i];
    }
    st.recent_wakes = std::span<const model::WakeRecord>(recent_.data(), count);
    st.selftest_summary = selftest_summary_.view();
    st.settings = &rtc_.settings;
    return st;
}

} // namespace qz::app
