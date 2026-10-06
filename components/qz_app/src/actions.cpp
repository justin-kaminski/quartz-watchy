// Action executor (ARCHITECTURE.md section 15): runs what UI screens, console commands and the
// provisioning form ask for, always through the one validation path (settings::set_from_string).
#include "app_core.hpp"
#include "qz/core/log.hpp"
#include "qz/time/civil.hpp"

#include <algorithm>
#include <cstdio>

namespace qz::app {

namespace {

constexpr const char* kTag = "app";

// selftest::run takes a plain function pointer for its clock: the app task is the only caller.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
const hal::Clock* g_selftest_clock = nullptr;

std::uint32_t selftest_now_ms() {
    return g_selftest_clock != nullptr
               ? static_cast<std::uint32_t>(g_selftest_clock->rtc_us() / 1000)
               : 0U;
}

std::string_view outcome_name(selftest::Outcome outcome) noexcept {
    switch (outcome) {
        case selftest::Outcome::kPass:
            return "pass";
        case selftest::Outcome::kFail:
            return "fail";
        case selftest::Outcome::kSkip:
            return "skip";
    }
    return "skip";
}

class JsonReportSink final : public selftest::ReportSink {
public:
    explicit JsonReportSink(console::JsonWriter* out) noexcept : out_(out) {}
    void on_result(const selftest::TestReport& report) override {
        if (out_ == nullptr) {
            return;
        }
        std::array<char, 96> name{};
        const int n = std::snprintf(name.data(),
                                    name.size(),
                                    "%.*s/%.*s",
                                    static_cast<int>(report.suite.size()),
                                    report.suite.data(),
                                    static_cast<int>(report.name.size()),
                                    report.name.data());
        out_->begin_object();
        out_->field("name", std::string_view(name.data(), n > 0 ? static_cast<std::size_t>(n) : 0));
        out_->field("status", outcome_name(report.outcome));
        out_->field("ms", static_cast<std::int64_t>(report.duration_ms));
        out_->field("detail", report.detail.view());
        out_->end_object();
    }

private:
    console::JsonWriter* out_;
};

/// Validates every settings field of the provisioning form into `next` (all or nothing).
Status fill_settings(settings::Settings& next, const conn::ProvisioningForm& form) noexcept {
    QZ_RETURN_IF_ERROR(
        settings::set_from_string(next, settings::Key::kTimeZone, form.tz_name.view()));
    QZ_RETURN_IF_ERROR(
        settings::set_from_string(next, settings::Key::kTempUnit, form.units.view()));
    QZ_RETURN_IF_ERROR(
        settings::set_from_string(next, settings::Key::kConnectivity, form.mode.view()));
    if (!form.lat.empty() || !form.lon.empty()) {
        QZ_RETURN_IF_ERROR(
            settings::set_from_string(next, settings::Key::kLatitude, form.lat.view()));
        QZ_RETURN_IF_ERROR(
            settings::set_from_string(next, settings::Key::kLongitude, form.lon.view()));
    }
    return ok();
}

} // namespace

void Core::execute(const ui::Action& action) noexcept {
    switch (action.kind) {
        case ui::ActionKind::kSetSetting: {
            const Status s = apply_setting_impl(action.key, action.value.view());
            if (!s) {
                note_error(s.error());
            }
            break;
        }
        case ui::ActionKind::kSetTime: {
            const Status s = set_time_local(action.date, action.time);
            if (!s) {
                note_error(s.error());
            }
            break;
        }
        case ui::ActionKind::kSyncNow:
            (void)sync_with_ui(); // the outcome is shown by the SyncNow screen (op_phase_)
            break;
        case ui::ActionKind::kStartProvisioning: {
            FixedString<32> ssid;
            std::uint16_t expires_s = 0;
            const Status s = start_provisioning_impl(ssid, expires_s);
            op_phase_ = s ? ui::OpPhase::kRunning : ui::OpPhase::kFailed;
            op_error_ = s ? Errc::kInternal : s.error().code;
            break;
        }
        case ui::ActionKind::kStopProvisioning:
            stop_provisioning_impl();
            break;
        case ui::ActionKind::kFactoryReset: {
            const Status s = factory_reset_impl();
            if (!s) {
                note_error(s.error());
            }
            break;
        }
        case ui::ActionKind::kRunSelfTest: {
            const Status s = run_selftests_impl({}, nullptr);
            if (!s) {
                note_error(s.error());
            }
            break;
        }
        case ui::ActionKind::kFullRefresh:
            force_full_next_ = true;
            break;
        case ui::ActionKind::kVibrate:
            if (rtc_.settings.vibration) {
                (void)vibrate_impl(action.arg); // refused by policy at low battery: not an error
            }
            break;
    }
}

// ---- settings ------------------------------------------------------------------------------

Status Core::apply_settings_object(const settings::Settings& next) noexcept {
    const settings::Settings old = rtc_.settings;
    if (next == old) {
        return ok();
    }
    QZ_RETURN_IF_ERROR(settings_store_.save(next, old)); // writes the changed keys, one commit
    rtc_.settings = next;
    rtc_.face_id = next.face_id;
    rtc_.settings_valid = 1;
    if (!(next.tz_name == old.tz_name) || !(next.tz_posix == old.tz_posix)) {
        refresh_tz();
        const time::UnixSeconds now = display_utc_s();
        after_step_update(steps_.on_time_jump(local_day(now), now));
    }
    if (next.connectivity != old.connectivity || next.location_set != old.location_set ||
        !(next.location == old.location)) {
        sched_.on_config_changed(); // reconfiguration makes the jobs due now
    }
    if (next.tap_wake != old.tap_wake) {
        sync_tap_interrupt();
    }
    return ok();
}

Status Core::apply_setting_impl(settings::Key key, std::string_view value) noexcept {
    settings::Settings next = rtc_.settings;
    QZ_RETURN_IF_ERROR(settings::set_from_string(next, key, value));
    if (key == settings::Key::kFace && !faces::is_registered(next.face_id)) {
        return Errc::kBadArgs;
    }
    if (key == settings::Key::kConnectivity && next.connectivity != model::ConnectivityMode::kOff &&
        !radio_compiled()) {
        return Errc::kUnsupported; // BuildFeatures.radio = false forces Off
    }
    return apply_settings_object(next);
}

// ---- time ----------------------------------------------------------------------------------

void Core::set_time_impl(time::UnixSeconds utc, time::TimeSource source) noexcept {
    (void)keeper_.set_utc(utc * time::kUsPerSecond, source); // services re-evaluate below
    after_step_update(steps_.on_time_jump(local_day(utc), utc));
    sched_.on_config_changed();
}

Status Core::set_time_local(const time::CivilDate& date, const time::CivilTime& time) noexcept {
    if (!time::is_valid(date, time)) {
        return Errc::kBadArgs;
    }
    const Result<time::UnixSeconds> utc = tz_.to_utc(date, time, time::GapPolicy::kEarlier);
    if (!utc) {
        return utc.error();
    }
    set_time_impl(*utc, time::TimeSource::kManual);
    return ok();
}

// ---- factory reset -------------------------------------------------------------------------

Status Core::factory_reset_impl() noexcept {
    const Status erased = settings_store_.erase_all(); // every qz_* namespace
    // RAM is reset even if the flash erase failed: the owner asked for a clean device.
    const time::TimeKeeperState keep = rtc_.time; // the RTC timer keeps running: time is kept
    const std::uint32_t boots = rtc_.header.boot_count;
    const std::uint8_t tap = rtc_.reserved[0] & wiring::kFlagTapArmed; // the sensor stays armed
    rtc_ = RtcState{};
    rtc_.header.boot_count = boots;
    rtc_.time = keep;
    rtc_.settings = settings::defaults();
    rtc_.settings_valid = 1;
    rtc_.face_id = rtc_.settings.face_id;
    rtc_.reserved[0] = tap;
    steps::StepTracker::reset(rtc_.steps);
    steps_.on_sensor_reset(); // software baseline reset (ARCHITECTURE.md section 6)
    shown_ = gfx::Framebuffer{};
    rtc_.display.frame_valid = 0;
    frame_dirty_ = true;
    force_full_next_ = true;
    selftest_summary_.clear();
    op_phase_ = ui::OpPhase::kIdle;
    refresh_tz();
    ui_.reset_to_face();
    sync_tap_interrupt();
    return erased;
}

// ---- radio ---------------------------------------------------------------------------------

Status Core::run_sync(bool want_time, bool want_weather) noexcept {
    if (!want_time && !want_weather) {
        return Errc::kBadArgs;
    }
    if (!radio_compiled()) {
        return Errc::kUnsupported;
    }
    if (rtc_.settings.connectivity == model::ConnectivityMode::kOff) {
        return Errc::kInvalidState; // Off means off
    }
    if (!power_.decision().radio_allowed || power_.level() != model::PowerLevel::kNormal) {
        return Errc::kBatteryLow;
    }
    if (!has_creds_flag()) {
        return Errc::kNoCredentials;
    }
    const conn::Inputs in = make_inputs(true);
    conn::Plan plan = sched_.plan(in);
    plan.time = plan.time && want_time;
    plan.weather = plan.weather && want_weather;
    if (!plan.any()) {
        if (want_weather && !want_time && !in.time_valid) {
            return Errc::kNoTime;
        }
        return Errc::kInvalidState;
    }
    return run_session(plan, true);
}

Status Core::sync_with_ui() noexcept {
    op_phase_ = ui::OpPhase::kRunning;
    (void)render_and_present(false, 0); // the display is updated before the radio runs
    const Status s = run_sync(true, true);
    op_phase_ = s ? ui::OpPhase::kSucceeded : ui::OpPhase::kFailed;
    op_error_ = s ? Errc::kInternal : s.error().code;
    return s;
}

Status Core::start_provisioning_impl(FixedString<32>& ssid_out, std::uint16_t& expires_s) noexcept {
    if (!radio_compiled() || p_.portal == nullptr) {
        return Errc::kUnsupported;
    }
    if (!power_.decision().radio_allowed) {
        return Errc::kBatteryLow;
    }
    if (prov_active_) {
        return Errc::kBusy;
    }
    prov_.begin(p_.clock.rtc_us());
    const Status started = p_.portal->start(prov_.ssid(), prov_.password(), prov_);
    if (!started) {
        prov_.end();
        return started;
    }
    prov_active_ = true;
    prov_started_rtc_us_ = p_.clock.rtc_us();
    w_.flags |= model::kWakeFlagRadio;
    if (!ssid_out.assign(prov_.ssid())) {
        ssid_out.clear();
    }
    expires_s = wiring::kProvisioningTtlS;
    return ok();
}

void Core::stop_provisioning_impl() noexcept {
    if (!prov_active_) {
        return;
    }
    const bool done = prov_.completed();
    if (p_.portal != nullptr) {
        p_.portal->stop();
    }
    prov_.end(); // wipes the AP password and the form token
    prov_active_ = false;
    op_phase_ = done ? ui::OpPhase::kSucceeded : ui::OpPhase::kIdle;
}

void Core::poll_provisioning() noexcept {
    if (!prov_active_ || p_.portal == nullptr) {
        return;
    }
    if (prov_.tick(p_.clock.rtc_us())) {
        stop_provisioning_impl();
        op_phase_ = ui::OpPhase::kFailed;
        op_error_ = Errc::kTimeout;
        return;
    }
    (void)p_.portal->poll(wiring::kPortalPollMs); // errors end by expiry; nothing to recover
    if (prov_.completed()) {
        stop_provisioning_impl();
    }
}

Status Core::apply(const conn::ProvisioningForm& form) {
    settings::Settings next = rtc_.settings;
    QZ_RETURN_IF_ERROR(fill_settings(next, form));
    hal::WifiCredentials creds;
    creds.ssid = form.ssid;
    creds.password = form.password;
    QZ_RETURN_IF_ERROR(settings::validate_credentials(creds));
    // Everything validated: persist (credentials first so a half-applied form never enables a
    // mode without a network).
    QZ_RETURN_IF_ERROR(cred_store_.save(creds));
    set_creds_flag(true);
    QZ_RETURN_IF_ERROR(apply_settings_object(next));
    sched_.on_config_changed();
    return ok();
}

// ---- actuators / self-test -----------------------------------------------------------------

Status Core::vibrate_impl(std::uint16_t ms) noexcept {
    if (ms == 0) {
        return Errc::kBadArgs;
    }
    if (!power_.decision().vibration_allowed) {
        return Errc::kBatteryLow;
    }
    p_.io.set_vibration(true);
    p_.delay.delay_ms(ms);
    p_.io.set_vibration(false);
    return ok();
}

Status Core::run_selftests_impl(std::string_view filter, console::JsonWriter* out) noexcept {
    selftest::Context ctx;
    ctx.panel = &panel_;
    ctx.accel = &accel_;
    ctx.battery_adc = &p_.battery_adc;
    ctx.io = &p_.io;
    ctx.kv = &p_.kv;
    ctx.system = &p_.system;
    ctx.delay = &p_.delay;
    ctx.faces = &faces_;
    ctx.interactive = f_.selftest_interactive && tether_.state() == TetherState::kTethered;
    JsonReportSink sink(out);
    if (out != nullptr) {
        out->begin_array("results");
    }
    g_selftest_clock = &p_.clock;
    const selftest::Summary summary = selftest::run(ctx, filter, sink, selftest_now_ms);
    g_selftest_clock = nullptr;
    if (out != nullptr) {
        out->end_array();
        out->field("passed", static_cast<std::int64_t>(summary.passed));
        out->field("failed", static_cast<std::int64_t>(summary.failed));
        out->field("skipped", static_cast<std::int64_t>(summary.skipped));
    }
    const unsigned total = static_cast<unsigned>(summary.passed) + summary.failed + summary.skipped;
    // The tests drive the panel and the sensor directly: the next frame is a full refresh.
    rtc_.display.frame_valid = 0;
    force_full_next_ = true;
    if (total == 0) {
        return Errc::kNotFound;
    }
    std::array<char, 24> text{};
    const int n =
        std::snprintf(text.data(), text.size(), "%u/%u pass", unsigned{summary.passed}, total);
    if (n > 0 &&
        !selftest_summary_.assign(std::string_view(text.data(), static_cast<std::size_t>(n)))) {
        selftest_summary_.clear();
    }
    QZ_LOGI(kTag, "selftest %u/%u", unsigned{summary.passed}, total);
    return ok();
}

} // namespace qz::app
