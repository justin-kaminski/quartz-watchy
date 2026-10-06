// Wake flows (ARCHITECTURE.md section 4). Ordering rules: the battery is sampled before display or
// radio load, the display is updated before any radio activity, the radio runs last and is
// bounded by the session budget. All state that must survive deep sleep lives in rtc_.
#include "app_core.hpp"
#include "qz/board/watchy_v3.hpp"
#include "qz/core/log.hpp"
#include "qz/time/civil.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace qz::app {

namespace {

constexpr const char* kTag = "app";

model::WakeFlag flag_of(model::WakeFlag f) noexcept {
    return f;
}

std::uint8_t buttons_from_pins(std::uint64_t pins) noexcept {
    std::uint8_t mask = 0;
    for (std::size_t i = 0; i < board::kButtonPins.size(); ++i) {
        if ((pins & (std::uint64_t{1} << board::kButtonPins[i])) != 0) {
            mask = static_cast<std::uint8_t>(mask | (1U << i));
        }
    }
    return mask;
}

} // namespace

// ---- small helpers -------------------------------------------------------------------------

void Core::note_error(Error error) noexcept {
    w_.flags |= model::kWakeFlagError;
    if (w_.error == 0) {
        w_.error = static_cast<std::uint8_t>(static_cast<std::uint8_t>(error.code) + 1U);
    }
    const std::string_view token = to_token(error.code);
    QZ_LOGW(kTag, "error %.*s", static_cast<int>(token.size()), token.data());
}

bool Core::has_creds_flag() const noexcept {
    return (rtc_.reserved[0] & wiring::kFlagCredsPresent) != 0;
}

void Core::set_creds_flag(bool present) noexcept {
    if (present) {
        rtc_.reserved[0] = static_cast<std::uint8_t>(rtc_.reserved[0] | wiring::kFlagCredsPresent);
    } else {
        rtc_.reserved[0] = static_cast<std::uint8_t>(rtc_.reserved[0] & ~wiring::kFlagCredsPresent);
    }
}

bool Core::tap_desired() const noexcept {
    return rtc_.settings.tap_wake && power_.decision().tap_wake_allowed && !w_.safe;
}

void Core::commit_all() noexcept {
    if (frame_dirty_) {
        store_.commit_frame(shown_);
        frame_dirty_ = false;
    }
    store_.commit(rtc_);
}

void Core::sleep_for(std::int64_t us, bool wake_on_buttons) noexcept {
    if (us <= 0) {
        return;
    }
    hal::SleepPlan plan;
    plan.timer_us = us;
    plan.wake_on_buttons = wake_on_buttons;
    plan.wake_on_accel = false;
    plan.wake_on_usb = false;
    plan.wake_on_epd_idle = false;
    (void)p_.sleep.light_sleep(plan); // the cause is irrelevant: callers re-check their conditions
}

void Core::wait_for_release() noexcept {
    // A held button re-wakes a level-sensed EXT1 immediately (ARCHITECTURE.md section 5).
    std::int64_t waited = 0;
    while (p_.io.pressed_buttons() != 0 && waited < wiring::kReleaseWaitMaxUs) {
        sleep_for(wiring::kPressedPollUs, false);
        waited += wiring::kPressedPollUs;
    }
    w_.held_at_end = p_.io.pressed_buttons() != 0;
}

void Core::flush_steps() noexcept {
    const Status saved = step_store_.save(rtc_.steps);
    if (!saved) {
        note_error(saved.error());
        return;
    }
    rtc_.steps.last_flush_day = rtc_.steps.today_day;
}

void Core::persist_drift() noexcept {
    // Only after a sync changed the drift by more than 1 ppm (<= 1 write/day, ARCH section 7).
    const std::int32_t drift = rtc_.time.drift_ppb;
    const Result<std::int32_t> stored = p_.kv.get_i32(wiring::kTimeNs, wiring::kDriftKey);
    if (stored && std::abs(*stored - drift) <= wiring::kDriftPersistDeltaPpb) {
        return;
    }
    const Status s = p_.kv.set_i32(wiring::kTimeNs, wiring::kDriftKey, drift);
    if (!s) {
        note_error(s.error());
        return;
    }
    (void)p_.kv.commit(); // a failed commit is retried by the next drift change
}

void Core::after_step_update(const steps::StepUpdate& update) noexcept {
    if (update.flush_due) {
        flush_steps();
    }
    if (update.goal_reached_now && rtc_.settings.vibration && power_.decision().vibration_allowed) {
        p_.io.set_vibration(true);
        p_.delay.delay_ms(wiring::kGoalVibrationMs);
        p_.io.set_vibration(false);
    }
}

// ---- wake start ----------------------------------------------------------------------------

void Core::cold_boot() noexcept {
    rtc_ = RtcState{};
    rtc_.header.boot_count = 1;
    time::TimeKeeper::reset(rtc_.time);
    steps::StepTracker::reset(rtc_.steps);

    const Result<settings::Settings> cfg = settings_store_.load();
    rtc_.settings = cfg ? *cfg : settings::defaults();
    rtc_.settings_valid = 1;
    rtc_.face_id = rtc_.settings.face_id;

    // Missing history (first boot) is not an error: the tracker starts empty.
    (void)step_store_.load(rtc_.steps);
    const Result<std::int32_t> drift = p_.kv.get_i32(wiring::kTimeNs, wiring::kDriftKey);
    if (drift) {
        keeper_.set_drift_ppb(*drift);
    }
    const Result<hal::WifiCredentials> creds = cred_store_.load();
    set_creds_flag(creds.has_value());

    shown_ = gfx::Framebuffer{};
    rtc_.display.frame_valid = 0;
    frame_dirty_ = true;
}

void Core::detect_cause(const hal::WakeSources& sources) noexcept {
    if (w_.cold) {
        w_.cause = model::WakeCause::kColdBoot;
    } else if (w_.reason != hal::ResetReason::kDeepSleep) {
        w_.cause = model::WakeCause::kReset;
    } else if (sources.ext1) {
        const std::uint8_t mask = buttons_from_pins(sources.ext1_pins);
        if (mask != 0) {
            w_.cause = model::WakeCause::kButton;
            w_.pressed_mask = mask;
        } else if ((sources.ext1_pins & (std::uint64_t{1} << board::kAccelInt1)) != 0) {
            w_.cause = model::WakeCause::kAccel;
        } else if ((sources.ext1_pins & (std::uint64_t{1} << board::kUsbDetect)) != 0) {
            w_.cause = model::WakeCause::kUsb;
        } else {
            w_.cause = model::WakeCause::kUnknown;
        }
    } else if (sources.ext0) {
        w_.cause = model::WakeCause::kUsb;
    } else if (sources.timer) {
        w_.cause = model::WakeCause::kTimer;
    } else {
        w_.cause = model::WakeCause::kUnknown;
    }
}

void Core::begin_wake() noexcept {
    w_ = WakeCtx{};
    w_.start_rtc_us = p_.clock.rtc_us();
    w_.boot_rtc_us = std::min(p_.system.boot_rtc_us(), w_.start_rtc_us);
    w_.reason = p_.system.reset_reason();
    const hal::WakeSources sources = p_.system.wake_sources();

    // Everything that is not RTC state is per-wake (the object may outlive a simulated sleep).
    if (prov_active_) {
        stop_provisioning_impl();
    }
    ui_.reset_to_face();
    recognizer_ = ui::GestureRecognizer{};
    op_phase_ = ui::OpPhase::kIdle;
    force_full_next_ = false;
    frame_dirty_ = false;
    pending_sleep_s_ = 0;
    pending_reboot_ = false;
    shown_minute_ = -1;
    selftest_summary_.clear();

    const bool power_loss =
        w_.reason == hal::ResetReason::kPowerOn || w_.reason == hal::ResetReason::kBrownout;
    const Status loaded = store_.load(rtc_);
    w_.cold = !loaded || power_loss || rtc_.settings_valid == 0;
    if (w_.cold) {
        cold_boot();
    } else {
        const Status frame = store_.load_frame(shown_);
        if (!frame) {
            shown_ = gfx::Framebuffer{};
            rtc_.display.frame_valid = 0;
        }
        if (w_.reason != hal::ResetReason::kDeepSleep) {
            ++rtc_.header.boot_count;
            rtc_.display.frame_valid = 0; // after any abnormal reset: full refresh (section 14)
        }
    }
    loaded_ = true;
    detect_cause(sources);

    refresh_tz();
    keeper_.set_clock_degraded(!p_.system.slow_clock().external_crystal);
    w_.safe = false; // evaluated below once the crash counter is up to date

    const std::int64_t now = p_.clock.rtc_us();
    if (w_.cause == model::WakeCause::kTimer) {
        (void)planner_.on_timer_wake(w_.boot_rtc_us); // false = nothing scheduled: harmless
    }
    if (w_.reason == hal::ResetReason::kPanic || w_.reason == hal::ResetReason::kWatchdog) {
        planner_.on_crash(now); // only panic/watchdog count (WP-20 notes)
    }

    // Tether check: two reads 10 ms apart (ARCHITECTURE.md section 17); the second read is only
    // needed when the first one saw USB.
    const bool first = p_.io.usb_present();
    bool second = false;
    if (first) {
        p_.delay.delay_ms(10);
        second = p_.io.usb_present();
    }
    w_.usb_present = first;
    if (tether_.on_wake(first, second) == TetherState::kTethered) {
        planner_.on_usb_attach();
    }
    w_.safe = planner_.safe_mode(p_.clock.rtc_us());
}

// ---- sensors -------------------------------------------------------------------------------

void Core::attach_accel() noexcept {
    accel_ok_ = false;
    if (!w_.cold) {
        if (accel_.attach()) {
            accel_ok_ = true;
            return;
        }
    }
    bma423::Config config;
    config.tap_interrupt = false; // armed by sync_tap_interrupt() once the power level is known
    const Status init = accel_.init(config);
    if (!init) {
        note_error(init.error());
        return;
    }
    accel_ok_ = true;
    steps_.on_sensor_reset();
    rtc_.reserved[0] = static_cast<std::uint8_t>(rtc_.reserved[0] & ~wiring::kFlagTapArmed);
}

void Core::sync_tap_interrupt() noexcept {
    if (!accel_ok_) {
        return;
    }
    const bool want = tap_desired();
    const bool armed = (rtc_.reserved[0] & wiring::kFlagTapArmed) != 0;
    bool read_status = armed;
    if (want != armed) {
        const Status s = accel_.set_tap_interrupt(want);
        if (!s) {
            note_error(s.error());
            return;
        }
        rtc_.reserved[0] =
            want ? static_cast<std::uint8_t>(rtc_.reserved[0] | wiring::kFlagTapArmed)
                 : static_cast<std::uint8_t>(rtc_.reserved[0] & ~wiring::kFlagTapArmed);
        read_status = true; // clear whatever is latched so INT1 cannot hold the wake line
    }
    if (read_status) {
        const Result<std::uint8_t> status = accel_.read_int_status();
        if (status) {
            w_.accel_event =
                (*status & static_cast<std::uint8_t>(bma423::IntStatus::kDoubleTap)) != 0;
        } else {
            note_error(status.error());
        }
    }
}

void Core::read_steps() noexcept {
    if (!accel_ok_) {
        return;
    }
    const Result<std::uint32_t> count = accel_.step_count();
    if (!count) {
        note_error(count.error());
        return;
    }
    const time::UnixSeconds now = display_utc_s();
    const steps::StepUpdate update =
        steps_.on_sample(*count, local_day(now), now, rtc_.settings.step_goal);
    w_.steps_delta = static_cast<std::uint16_t>(std::min<std::uint32_t>(update.delta, UINT16_MAX));
    after_step_update(update);
}

void Core::take_fake_sample() noexcept {
    power::PowerState& ps = rtc_.power;
    ps.filtered_mv = ps.fake_mv; // the override is exact, not EWMA-smoothed
    ps.valid = 1;
    (void)power_.on_sample(ps.fake_mv, p_.io.usb_present(), p_.clock.rtc_us());
}

void Core::adc_sample() noexcept {
    std::array<std::uint16_t, wiring::kBatterySamples> samples{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Result<std::uint16_t> pin = p_.battery_adc.read_pin_mv();
        if (pin) {
            samples[n++] = *pin;
        }
    }
    if (n == 0) {
        note_error(Error{Errc::kIo});
        return;
    }
    const std::uint16_t pin_mv =
        power::robust_mean_mv(std::span<const std::uint16_t>(samples.data(), n));
    const std::uint16_t mv =
        power::battery_mv_from_pin(pin_mv, board::kBatteryDividerNum, board::kBatteryDividerDen);
    (void)power_.on_sample(mv, p_.io.usb_present(), p_.clock.rtc_us());
}

void Core::sample_battery() noexcept {
    const model::PowerLevel before = power_.level();
    if (rtc_.power.fake_mv != 0) {
        take_fake_sample();
    } else {
        const bool event = w_.cause == model::WakeCause::kButton ||
                           w_.cause == model::WakeCause::kColdBoot ||
                           w_.cause == model::WakeCause::kUsb ||
                           (before == model::PowerLevel::kCritical && p_.io.usb_present());
        if (event || power_.sample_due(p_.clock.rtc_us())) {
            adc_sample();
        }
    }
    if (before != model::PowerLevel::kCritical && power_.level() == model::PowerLevel::kCritical) {
        flush_steps(); // ARCHITECTURE.md section 11: flush steps on entering Critical
    }
}

// ---- display -------------------------------------------------------------------------------

Status Core::display_failed(Error error) noexcept {
    rtc_.display.frame_valid = 0; // the next update is full (section 14)
    frame_dirty_ = true;
    note_error(error);
    return error;
}

Status Core::present(bool force_full, time::UnixMicros target_utc_us, bool force_update) noexcept {
    DisplayState& d = rtc_.display;
    force_full = force_full || force_full_next_;
    if (!force_full && !force_update && d.frame_valid != 0 && next_.bits == shown_.bits) {
        return ok(); // nothing changed: the panel is not touched
    }
    const Status init = panel_.init();
    if (!init) {
        return display_failed(init.error());
    }
    const power::PolicyDecision decision = power_.decision();
    bool full =
        force_full || d.frame_valid == 0 ||
        (decision.full_refresh_every != 0 && d.partials_since_full >= decision.full_refresh_every);
    std::int16_t temp_dc = d.panel_temp_dc;
    bool temp_known = false;
    const Result<std::int16_t> temp = panel_.temperature_dc();
    if (temp) {
        temp_dc = *temp;
        temp_known = true;
        if (d.frame_valid != 0 &&
            std::abs(temp_dc - d.panel_temp_dc) > wiring::kTempFullRefreshDc) {
            full = true;
        }
    }
    const ssd1681::UpdateMode mode =
        full ? ssd1681::UpdateMode::kFull : ssd1681::UpdateMode::kPartial;
    if (!full && target_utc_us != 0) {
        // Hold the finished frame until the waveform midpoint lands on the minute (section 8.4).
        sleep_for(planner_.trigger_wait_us(p_.clock.rtc_us(), target_utc_us), false);
    }
    Status st = panel_.begin_update(shown_, next_, mode);
    if (st) {
        st = panel_.finish(mode);
    }
    if (!st) {
        return display_failed(st.error());
    }
    shown_ = next_;
    frame_dirty_ = true;
    d.frame_valid = 1;
    force_full_next_ = false;
    if (full) {
        d.partials_since_full = 0;
        d.last_refresh_full = 1;
        d.last_full_utc = keeper_.valid() ? now_utc_s() : 0;
        if (temp_known) {
            d.panel_temp_dc = temp_dc;
        }
        w_.flags |= flag_of(model::kWakeFlagFullRefresh);
    } else {
        ++d.partials_since_full;
        d.last_refresh_full = 0;
        w_.flags |= flag_of(model::kWakeFlagPartialRefresh);
    }
    return ok();
}

Status Core::render_and_present(bool force_full,
                                time::UnixMicros target_utc_us,
                                bool force_update) noexcept {
    const time::UnixSeconds t =
        target_utc_us != 0 ? target_utc_us / time::kUsPerSecond : display_utc_s();
    const ui::WatchState& st = build_state(t);
    gfx::Canvas canvas(next_);
    ui_.render(st, canvas);
    shown_minute_ = keeper_.valid() ? time::floor_to_minute(t) : -1;
    return present(force_full, target_utc_us, force_update);
}

time::UnixMicros Core::timer_wake_target() noexcept {
    // A tick wake lands slightly before the boundary (the EWMA lead may have shrunk since the
    // timer was programmed): the frame is for the upcoming boundary and the update is held in
    // light sleep until then (ARCHITECTURE.md section 8.4). Anything else (a late wake, a console
    // `sleep` wake) shows the minute the planner says is current.
    const std::int64_t now_rtc = p_.clock.rtc_us();
    const time::UnixMicros now_utc = keeper_.utc_at_rtc(now_rtc);
    const std::int64_t period_us = static_cast<std::int64_t>(std::max<std::uint16_t>(
                                       power_.decision().display_period_min, 1)) *
                                   time::kSecondsPerMinute * time::kUsPerSecond;
    const time::UnixMicros next = ((now_utc / period_us) + 1) * period_us;
    if (next - now_utc <= wiring::kEarlyTickWindowUs) {
        return next;
    }
    return planner_.tick_target_utc_us(now_rtc);
}

void Core::show_wake_frame(bool overlay) noexcept {
    DisplayState& d = rtc_.display;
    bool force = w_.cold || w_.cause == model::WakeCause::kReset;
    if ((d.reserved & wiring::kDisplayChargeMeShown) != 0) {
        force = true; // leaving Critical: replace the "Charge me" screen with a full refresh
        d.reserved = static_cast<std::uint16_t>(d.reserved & ~wiring::kDisplayChargeMeShown);
    }
    time::UnixMicros target = 0;
    if (w_.cause == model::WakeCause::kTimer && keeper_.valid()) {
        target = timer_wake_target();
    }
    if (!keeper_.valid()) {
        w_.flags |= flag_of(model::kWakeFlagTimeInvalid);
    }
    if (overlay) {
        (void)ui_.show(ui::ScreenId::kStatusOverlay, build_state(display_utc_s()));
        last_input_us_ = p_.clock.rtc_us();
    }
    (void)render_and_present(force, target); // failures are recorded in the wake log
}

void Core::show_critical() noexcept {
    DisplayState& d = rtc_.display;
    const bool peek = w_.cause == model::WakeCause::kButton;
    if (peek) {
        // A button shows the face with the current time (+ the face's critical mark) for 10 s.
        (void)render_and_present(false, 0);
        sleep_for(wiring::kCriticalPeekUs, false);
    }
    if (peek || (d.reserved & wiring::kDisplayChargeMeShown) == 0) {
        (void)ui_.show(ui::ScreenId::kChargeMe, build_state(display_utc_s()));
        (void)render_and_present(true, 0);
        d.reserved = static_cast<std::uint16_t>(d.reserved | wiring::kDisplayChargeMeShown);
        ui_.reset_to_face();
    }
}

void Core::refresh_after_radio() noexcept {
    // A session may have moved the clock or changed the sync indicator / weather: show it. An
    // unchanged frame is not sent to the panel, so this is free when nothing visible changed.
    if ((w_.flags & model::kWakeFlagRadio) == 0 || ui_.current() != ui::ScreenId::kFace) {
        return;
    }
    const time::UnixMicros target =
        keeper_.valid() ? planner_.tick_target_utc_us(p_.clock.rtc_us()) : 0;
    (void)render_and_present(false, target);
}

void Core::refresh_face_if_stale() noexcept {
    if (ui_.current() != ui::ScreenId::kFace || !keeper_.valid()) {
        return;
    }
    const time::UnixMicros target = planner_.tick_target_utc_us(p_.clock.rtc_us());
    if (target == 0 || time::floor_to_minute(target / time::kUsPerSecond) == shown_minute_) {
        return;
    }
    (void)render_and_present(false, target);
}

// ---- radio ---------------------------------------------------------------------------------

void Core::run_connectivity() noexcept {
    if (!radio_compiled() || w_.safe) {
        return;
    }
    if (rtc_.settings.connectivity == model::ConnectivityMode::kOff) {
        return; // Off means off: nothing below may touch the net stack
    }
    if (!power_.decision().radio_allowed || power_.level() != model::PowerLevel::kNormal) {
        return;
    }
    const conn::Inputs in = make_inputs(false);
    const conn::Plan plan = sched_.plan(in);
    if (!plan.any()) {
        return;
    }
    if (!in.time_valid && rtc_.conn.fail_streak > 0) {
        // UTC unknown: next-due cannot be expressed, so pace on raw RTC time (conn.hpp).
        const std::int64_t now_s = p_.clock.rtc_us() / wiring::kUs;
        if (now_s < rtc_.conn.last_attempt_utc + sched_.retry_delay_s(in)) {
            return;
        }
    }
    (void)run_session(plan, false); // result applied + recorded inside
}

Status Core::run_session(const conn::Plan& plan, bool manual) noexcept {
    if (!session_.has_value()) {
        return Error{Errc::kUnsupported};
    }
    Result<hal::WifiCredentials> creds = cred_store_.load();
    if (!creds) {
        set_creds_flag(false);
        return creds.error();
    }
    w_.flags |= flag_of(model::kWakeFlagRadio);
    const conn::Inputs pre = make_inputs(manual);
    const conn::SessionResult result = session_->run(
        plan, *creds, rtc_.settings.location, conn::Budget{}, pre.time_valid ? pre.now_utc : 0);
    creds->password.clear();

    if (plan.time && result.time && result.sntp_utc_us.has_value()) {
        const time::SyncOutcome outcome =
            keeper_.apply_sntp(*result.sntp_utc_us, result.sntp_rtc_us);
        const time::UnixSeconds now = now_utc_s();
        after_step_update(steps_.on_time_jump(local_day(now), now));
        if (outcome.drift_updated) {
            persist_drift();
        }
    }
    if (plan.weather && result.weather && result.report.has_value()) {
        rtc_.weather = *result.report;
        rtc_.weather.valid = 1;
        rtc_.weather.faked = 0;
    }
    const conn::Inputs post = make_inputs(manual);
    sched_.on_result(plan, result, post);
    if (!post.time_valid) {
        rtc_.conn.last_attempt_utc = p_.clock.rtc_us() / wiring::kUs;
    }

    Status first = ok();
    if (!result.connect) {
        first = result.connect;
    } else if (plan.time && !result.time) {
        first = result.time;
    } else if (plan.weather && !result.weather) {
        first = result.weather;
    }
    if (!first) {
        note_error(first.error());
    }
    return first;
}

// ---- wake end ------------------------------------------------------------------------------

void Core::record_wake() noexcept {
    const std::int64_t now = p_.clock.rtc_us();
    const std::int64_t awake_us = std::max<std::int64_t>(now - w_.boot_rtc_us, 0);
    const auto awake_ms =
        static_cast<std::uint32_t>(std::min<std::int64_t>(awake_us / 1000, UINT32_MAX));
    model::WakeRecord rec;
    const std::int64_t stamp_s = keeper_.valid()
                                     ? keeper_.utc_at_rtc(w_.start_rtc_us) / time::kUsPerSecond
                                     : w_.start_rtc_us / wiring::kUs;
    rec.start_utc_s = static_cast<std::uint32_t>(stamp_s);
    rec.awake_ms = static_cast<std::uint16_t>(std::min<std::uint32_t>(awake_ms, UINT16_MAX));
    rec.cause = w_.cause;
    rec.flags = w_.flags;
    if (!keeper_.valid()) {
        rec.flags |= flag_of(model::kWakeFlagTimeInvalid);
    }
    rec.battery_mv = power_.status(false, false).mv;
    rec.power = power_.level();
    rec.error = w_.error;
    rec.steps_delta = w_.steps_delta;
    rtc_.wake_log.push(rec);
    const std::optional<time::DayNumber> day = local_day(display_utc_s());
    power_.record_wake(w_.cause, awake_ms, day.value_or(0));
}

PlanRequest Core::make_plan_request(std::int64_t sleep_override_us) const noexcept {
    PlanRequest req;
    req.now_rtc_us = p_.clock.rtc_us();
    req.level = power_.level();
    req.decision = power_.decision();
    req.tap_wake = tap_desired() && accel_ok_ && (rtc_.reserved[0] & wiring::kFlagTapArmed) != 0;
    req.usb_wake = f_.usb_wake;
    req.usb_present = p_.io.usb_present();
    req.sleep_override_us = sleep_override_us;
    return req;
}

hal::SleepPlan Core::finish_wake(std::int64_t sleep_override_us) noexcept {
    wait_for_release();
    record_wake();
    const WakePlan plan = planner_.plan(make_plan_request(sleep_override_us));
    // A button that is still held is excluded per pin by the platform (build_deep_arm leaves out
    // pins already at their wake level), so wake_on_buttons stays on: turning every button wake off
    // could leave a Critical-level watch with no wake source at all (pre-flash review finding 2).
    commit_all();
    return plan.sleep;
}

// ---- the wake ------------------------------------------------------------------------------

hal::SleepPlan Core::run_wake() noexcept {
    begin_wake();
    attach_accel();
    read_steps();
    sample_battery();
    sync_tap_interrupt();

    const bool tethered = tether_.state() == TetherState::kTethered;
    const power::PolicyDecision decision = power_.decision();
    std::int64_t sleep_override_us = 0;
    if (power_.level() == model::PowerLevel::kCritical && decision.charge_me_screen && !tethered) {
        show_critical();
    } else {
        const bool overlay = w_.cause == model::WakeCause::kAccel && w_.accel_event && !w_.safe;
        show_wake_frame(overlay);
        if (tethered) {
            run_connectivity();
            refresh_after_radio();
            sleep_override_us = tethered_loop();
        } else {
            if ((w_.cause == model::WakeCause::kButton || overlay) && !w_.safe) {
                interactive_session(w_.pressed_mask);
            }
            run_connectivity();
            refresh_after_radio();
        }
    }
    return finish_wake(sleep_override_us);
}

} // namespace qz::app
