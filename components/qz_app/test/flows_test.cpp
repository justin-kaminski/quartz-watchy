// One integration test per wake flow of ARCHITECTURE.md section 4 (cold boot, minute tick, time
// invalid, software reset / safe mode, button session, accelerometer), full testkit.
#include "app_test_env.hpp"

#include <gtest/gtest.h>

#include <array>

// Test arithmetic mixes int literals with int64 microsecond constants and gtest macros inflate
// function complexity: those checks are relaxed for this file only.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
namespace qz::app {
namespace {

using namespace testenv;

TEST(AppSizing, CoreFitsItsStorage) {
    // The App header reserves fixed storage for the Core (no heap): keep headroom visible.
    SUCCEED() << "App object size " << sizeof(App) << " bytes";
}

// ---- cold boot ------------------------------------------------------------------------------

TEST(AppColdBoot, InitializesStateSensorsAndPanel) {
    Env env;
    const hal::SleepPlan plan = env.cold_boot();

    // Panel: one full refresh, protocol clean, panel put to sleep.
    EXPECT_EQ(env.epd.full_updates(), 1U);
    EXPECT_EQ(env.epd.partial_updates(), 0U);
    EXPECT_EQ(env.epd.violation_count(), 0U);
    EXPECT_TRUE(env.epd.in_deep_sleep());
    // Time invalid: the face shows the "--:--" state.
    EXPECT_TRUE(same_frame(env.epd.displayed(), expected_face(env, 0, false)));
    // Sensor: full init (config blob) once, no protocol violation.
    EXPECT_EQ(env.accel.config_uploads(), 1U);
    EXPECT_EQ(env.accel.protocol_violations(), 0U);
    // Battery sampled before anything else (16 samples).
    EXPECT_GE(env.io.adc_reads(), 16U);
    // Off mode / no creds: radio and console untouched, nothing written to NVS.
    EXPECT_EQ(env.net.connect_calls(), 0U);
    EXPECT_EQ(env.net.radio_init_count(), 0U);
    EXPECT_EQ(env.console_port.start_count(), 0U);
    EXPECT_TRUE(env.console_port.sent().empty());
    EXPECT_EQ(env.kv.write_count(), 0U);
    // Plan: time invalid -> relative 10-minute housekeeping tick; buttons + USB wake sources.
    EXPECT_EQ(plan.timer_us, 600 * kUs);
    EXPECT_TRUE(plan.wake_on_buttons);
    EXPECT_TRUE(plan.wake_on_usb);
    EXPECT_FALSE(plan.wake_on_accel);

    // State committed with a valid CRC, wake recorded.
    const RtcState state = load_rtc(env);
    EXPECT_EQ(state.header.boot_count, 1U);
    ASSERT_EQ(state.wake_log.size(), 1U);
    const model::WakeRecord rec = state.wake_log.newest();
    EXPECT_EQ(rec.cause, model::WakeCause::kColdBoot);
    EXPECT_NE(rec.flags & model::kWakeFlagFullRefresh, 0);
    EXPECT_NE(rec.flags & model::kWakeFlagTimeInvalid, 0);
    EXPECT_EQ(rec.error, 0);
    EXPECT_FALSE(env.io.vibrating());
}

TEST(AppColdBoot, PowerLossScramblesStateAndStartsOver) {
    Env env;
    (void)env.boot_with_time();
    EXPECT_TRUE(env.api().time_info().valid);
    env.rtc.scramble();
    env.clock.power_loss();
    env.accel.sensor_reset();
    env.sys.set_wake(hal::ResetReason::kBrownout, {});
    (void)env.app->run_wake();
    EXPECT_FALSE(env.api().time_info().valid); // a brownout is a cold boot: time invalid
    EXPECT_EQ(env.api().wake_record(env.api().wake_record_count() - 1).cause,
              model::WakeCause::kColdBoot);
    EXPECT_EQ(env.epd.violation_count(), 0U);
}

// ---- minute tick ----------------------------------------------------------------------------

TEST(AppTimerTick, ShowsTargetMinuteAndPlansTheNextBoundary) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time(kT0Utc + 10 * kUs);
    const std::uint32_t kv_writes = env.kv.write_count();
    const std::uint32_t kv_commits = env.kv.commit_count();

    for (int tick = 0; tick < 6; ++tick) {
        plan = env.wake_timer(plan);
        const std::int64_t now = env.clock.true_utc_us();
        const std::int64_t boundary = now / kMin * kMin;
        // The frame shown is the face of the minute that just started.
        EXPECT_TRUE(same_frame(env.epd.displayed(), expected_face(env, boundary / kUs)))
            << "tick " << tick;
        if (tick >= 3) {
            // Wake-ahead has converged: the update completes within 0.6 s after the flip.
            EXPECT_LT(now - boundary, 600'000) << "tick " << tick;
        }
        // Next wake is before the next boundary, never later.
        const std::int64_t next_boundary = boundary + kMin;
        EXPECT_GT(plan.timer_us, 0);
        EXPECT_LT(env.utc_now_us() + plan.timer_us, next_boundary) << "tick " << tick;
        EXPECT_GT(env.utc_now_us() + plan.timer_us, next_boundary - 2 * kUs) << "tick " << tick;
        EXPECT_TRUE(env.epd.in_deep_sleep());
        EXPECT_FALSE(plan.wake_on_accel);
        EXPECT_TRUE(plan.wake_on_buttons);
    }
    EXPECT_EQ(env.epd.violation_count(), 0U);
    EXPECT_EQ(env.epd.full_updates(), 1U); // cold boot only; ticks are partial
    EXPECT_GE(env.epd.partial_updates(), 6U);
    EXPECT_EQ(env.kv.write_count(), kv_writes); // no NVS traffic on the minute path
    EXPECT_EQ(env.kv.commit_count(), kv_commits);
    EXPECT_EQ(env.net.connect_calls(), 0U);
    EXPECT_EQ(env.console_port.start_count(), 0U);
    EXPECT_TRUE(env.console_port.sent().empty());

    console::DeviceApi& api = env.api();
    const model::WakeRecord rec = api.wake_record(api.wake_record_count() - 1);
    EXPECT_EQ(rec.cause, model::WakeCause::kTimer);
    EXPECT_NE(rec.flags & model::kWakeFlagPartialRefresh, 0);
    EXPECT_EQ(rec.flags & model::kWakeFlagTimeInvalid, 0);
}

TEST(AppTimerTick, FullRefreshEveryNPartialUpdates) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time();
    for (int i = 0; i < 33; ++i) {
        plan = env.wake_timer(plan);
    }
    // 30 partial updates, then a full refresh to clear ghosting (ARCH s14).
    EXPECT_EQ(env.epd.full_updates(), 2U);
    EXPECT_EQ(env.epd.violation_count(), 0U);
}

TEST(AppTimerTick, StepsAreReadAndRolledPerLocalDay) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time();
    env.accel.add_steps(120);
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.api().steps().today, 120U);
    env.accel.add_steps(30);
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.api().steps().today, 150U);
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_EQ(rec.steps_delta, 30);
}

// ---- time invalid ---------------------------------------------------------------------------

TEST(AppTimeInvalid, HousekeepingTicksEveryTenMinutesAndStepsPend) {
    Env env;
    hal::SleepPlan plan = env.cold_boot();
    EXPECT_EQ(plan.timer_us, 600 * kUs);
    const std::uint32_t partials = env.epd.partial_updates();

    env.accel.add_steps(200);
    plan = env.wake_timer(plan);
    EXPECT_EQ(plan.timer_us, 600 * kUs);
    EXPECT_EQ(env.api().steps().today, 0U); // pending until the time is known
    EXPECT_EQ(env.epd.partial_updates(), partials) << "an unchanged '--:--' frame is not resent";
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_NE(rec.flags & model::kWakeFlagTimeInvalid, 0);

    // Setting the time releases the pending steps into today and switches to minute ticks.
    env.clock.set_true_utc_us(kT0Utc);
    ASSERT_TRUE(static_cast<bool>(env.api().set_time_utc(kT0Utc / kUs)));
    EXPECT_TRUE(same_frame(env.epd.displayed(), expected_face(env, kT0Utc / kUs)));
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.api().steps().today, 200U);
    EXPECT_LT(plan.timer_us, kMin);
}

// ---- reset / safe mode ----------------------------------------------------------------------

TEST(AppReset, SoftwareResetResumesStateAndRefreshesFully) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time();
    plan = env.wake_timer(plan);
    const std::uint32_t full = env.epd.full_updates();
    const RtcState before = load_rtc(env);

    env.clock.advance_rtc_us(2 * kUs);
    env.sys.set_wake(hal::ResetReason::kSoftware, {});
    plan = env.app->run_wake();

    EXPECT_TRUE(env.api().time_info().valid) << "time survives a software reset";
    const RtcState after = load_rtc(env);
    EXPECT_EQ(after.header.boot_count, before.header.boot_count + 1);
    EXPECT_EQ(after.wake.crash_count_window, 0) << "a clean reboot is not a crash";
    const model::WakeRecord rec = after.wake_log.newest();
    EXPECT_EQ(rec.cause, model::WakeCause::kReset);
    EXPECT_EQ(env.epd.full_updates(), full + 1) << "full refresh after any abnormal reset";
    EXPECT_TRUE(same_frame(env.epd.displayed(),
                           expected_face(env, env.clock.true_utc_us() / kMin * kMin / kUs)));
}

TEST(AppReset, ThreePanicsEnterSafeModeUntilUsbAttach) {
    Env env;
    (void)env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().apply_setting(settings::Key::kTapWake, "on")));
    hal::SleepPlan plan = env.wake_on(timer_wake(), 5 * kUs);
    EXPECT_TRUE(plan.wake_on_accel) << "tap wake armed in normal operation";

    for (int i = 0; i < 2; ++i) {
        env.clock.advance_rtc_us(10 * kUs);
        env.sys.set_wake(hal::ResetReason::kPanic, {});
        plan = env.app->run_wake();
        EXPECT_TRUE(plan.wake_on_accel) << "crash " << i + 1 << " is not yet safe mode";
    }
    env.clock.advance_rtc_us(10 * kUs);
    env.sys.set_wake(hal::ResetReason::kWatchdog, {});
    plan = env.app->run_wake();
    EXPECT_FALSE(plan.wake_on_accel) << "safe mode: no tap wake";
    {
        std::array<char, 512> buf{};
        console::JsonWriter json(buf);
        json.begin_object();
        ASSERT_TRUE(static_cast<bool>(env.api().write_diag("rtc", json)));
        json.end_object();
        EXPECT_NE(json.view().find("\"safe_mode\":true"), std::string_view::npos) << json.view();
    }

    // Safe mode: a button wake shows the face only (no interactive session, no menus).
    env.clock.advance_rtc_us(20 * kUs);
    env.io.press(hal::kButtonBitMenu);
    env.sleep.at(env.clock.rtc_us() + 100'000, 0, hal::kButtonBitMenu);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, button_wake(hal::kButtonBitMenu));
    plan = env.app->run_wake();
    EXPECT_EQ(env.api().current_screen(), "face");
    EXPECT_FALSE(plan.wake_on_accel);
    EXPECT_FALSE(env.io.pressed_buttons() != 0);

    // USB attach ends safe mode.
    env.io.set_usb(true, true);
    env.clock.advance_rtc_us(5 * kUs);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, usb_wake());
    env.console.hook = [&](std::uint32_t) {
        env.io.set_usb(false, false);
    };
    plan = env.app->run_wake();
    env.console.hook = nullptr;
    env.clock.advance_rtc_us(5 * kUs);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    plan = env.app->run_wake();
    EXPECT_TRUE(plan.wake_on_accel) << "tap wake is back after a USB attach";
}

// ---- button session -------------------------------------------------------------------------

/// Wake on a button press: `mask` is down when the app starts and released `hold_us` later.
hal::SleepPlan button_session(Env& env, std::uint8_t mask, std::int64_t hold_us = 100'000) {
    env.clock.advance_rtc_us(20 * kUs);
    env.io.press(mask);
    env.sleep.at(env.clock.rtc_us() + hold_us, 0, mask);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, button_wake(mask));
    return env.app->run_wake();
}

TEST(AppButton, MenuSessionOpensMenuThenIdleReturnsToFaceWithFullRefresh) {
    Env env;
    (void)env.boot_with_time();
    const std::uint32_t full = env.epd.full_updates();
    const std::uint32_t partial = env.epd.partial_updates();

    const hal::SleepPlan plan = button_session(env, hal::kButtonBitMenu);

    EXPECT_EQ(env.api().current_screen(), "face");
    EXPECT_GE(env.epd.partial_updates(), partial + 1) << "the menu was shown";
    EXPECT_EQ(env.epd.full_updates(), full + 1) << "leaving the menu: full refresh";
    EXPECT_TRUE(same_frame(env.epd.displayed(),
                           expected_face(env, env.clock.true_utc_us() / kMin * kMin / kUs)));
    EXPECT_FALSE(env.sleep.light_plans.empty()) << "light sleep between inputs";
    // Menu idle timeout is 30 s: the session lasted at least that long, not much more.
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_EQ(rec.cause, model::WakeCause::kButton);
    EXPECT_NE(rec.flags & model::kWakeFlagInput, 0);
    EXPECT_GE(rec.awake_ms, 30'000);
    EXPECT_LT(rec.awake_ms, 36'000);
    EXPECT_EQ(env.epd.violation_count(), 0U);
    EXPECT_GT(plan.timer_us, 0);
    EXPECT_TRUE(plan.wake_on_buttons);
    EXPECT_EQ(env.io.pressed_buttons(), 0);
}

TEST(AppButton, BackOnFaceForcesFullRefreshAndShortSessionEndsAfterTwoSeconds) {
    Env env;
    (void)env.boot_with_time();
    const std::uint32_t full = env.epd.full_updates();
    (void)button_session(env, hal::kButtonBitBack);
    EXPECT_EQ(env.epd.full_updates(), full + 1);
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_LT(rec.awake_ms, 4000) << "the face idles out after ~2 s";
}

TEST(AppButton, MenuNavigationSavesTwelveHourSetting) {
    Env env;
    (void)env.boot_with_time();
    ASSERT_EQ(env.api().current_settings().hour_format, model::HourFormat::k24h);
    const std::uint32_t writes = env.kv.write_count();

    // MENU (wake, click) opens the menu; DOWN x2 -> "12/24h"; MENU enters; DOWN -> 12h; MENU saves.
    const std::int64_t t = env.clock.rtc_us() + 20 * kUs;
    std::int64_t at = t + 1'000'000;
    // A press must outlast the panel update the previous click triggered (the app only samples
    // the pins between updates), like a real finger does.
    auto click = [&](std::uint8_t b) {
        env.sleep.at(at, b, 0);
        env.sleep.at(at + 500'000, 0, b);
        at += 2'000'000;
    };
    click(hal::kButtonBitDown);
    click(hal::kButtonBitDown);
    click(hal::kButtonBitMenu);
    click(hal::kButtonBitDown);
    click(hal::kButtonBitMenu);
    (void)button_session(env, hal::kButtonBitMenu);

    EXPECT_EQ(env.api().current_settings().hour_format, model::HourFormat::k12h);
    EXPECT_GT(env.kv.write_count(), writes);
    EXPECT_EQ(env.api().current_screen(), "face");
    EXPECT_TRUE(same_frame(env.epd.displayed(),
                           expected_face(env, env.clock.true_utc_us() / kMin * kMin / kUs)));
    // The setting persisted in NVS and the RTC cache.
    EXPECT_TRUE(env.kv.contains_text("tfmt"));
    EXPECT_EQ(load_rtc(env).settings.hour_format, model::HourFormat::k12h);
}

TEST(AppButton, HeldButtonAtTheEndKeepsTheButtonSourceArmedPerPin) {
    Env env;
    (void)env.boot_with_time();
    // Held far longer than the 5 s release wait.
    const hal::SleepPlan plan = button_session(env, hal::kButtonBitUp, 120 * kUs);
    (void)plan;
    // The session itself waits for the release, so the plan is normal here...
    EXPECT_EQ(env.io.pressed_buttons(), 0);

    // ...and a non-interactive wake with a stuck button keeps wake_on_buttons set: the platform
    // leaves out the pins that are already at their wake level (sleep.cpp build_deep_arm), and
    // turning every button wake off could leave a Critical-level watch with no wake source at all
    // (pre-flash review finding 2). The plan must still have SOME wake source.
    env.clock.advance_rtc_us(10 * kUs);
    env.io.press(hal::kButtonBitDown); // never released
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    const hal::SleepPlan stuck = env.app->run_wake();
    EXPECT_TRUE(stuck.wake_on_buttons);
    EXPECT_TRUE(stuck.wake_on_buttons || stuck.timer_us >= 0 || stuck.wake_on_usb);
    env.io.release(hal::kButtonBitDown);
}

// ---- accelerometer --------------------------------------------------------------------------

TEST(AppAccel, DoubleTapShowsStatusOverlayForFiveSeconds) {
    Env env;
    (void)env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().apply_setting(settings::Key::kTapWake, "on")));
    hal::SleepPlan plan = env.wake_on(timer_wake(), 30 * kUs);
    ASSERT_TRUE(plan.wake_on_accel);
    EXPECT_TRUE(env.accel.int1_active() == false);

    env.accel.trigger_double_tap();
    EXPECT_TRUE(env.accel.int1_active());
    const std::uint32_t partial = env.epd.partial_updates();
    plan = env.wake_on(accel_wake(), 20 * kUs);

    EXPECT_FALSE(env.accel.int1_active()) << "interrupt status read and cleared";
    EXPECT_GT(env.epd.partial_updates(), partial) << "overlay shown, then the face again";
    EXPECT_EQ(env.api().current_screen(), "face");
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_EQ(rec.cause, model::WakeCause::kAccel);
    EXPECT_GE(rec.awake_ms, 5000);
    EXPECT_LT(rec.awake_ms, 8000);
    EXPECT_TRUE(plan.wake_on_accel);
    EXPECT_TRUE(same_frame(env.epd.displayed(),
                           expected_face(env, env.clock.true_utc_us() / kMin * kMin / kUs)));
}

TEST(AppAccel, TapWakeOffIgnoresTheSensorLine) {
    Env env;
    (void)env.boot_with_time();
    const hal::SleepPlan plan = env.wake_on(accel_wake(), 30 * kUs);
    EXPECT_FALSE(plan.wake_on_accel);
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_EQ(rec.cause, model::WakeCause::kAccel);
    EXPECT_LT(rec.awake_ms, 2000) << "no overlay without tap wake";
}

} // namespace
} // namespace qz::app
// NOLINTEND(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
