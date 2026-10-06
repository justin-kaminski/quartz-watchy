// Radio rules (ARCHITECTURE.md section 12: display before radio, a failed session never blanks the
// face, Off means off) and the power policy (section 11: Low / Saver / Critical gating).
#include "app_test_env.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

// Test arithmetic mixes int literals with int64 microsecond constants and gtest macros inflate
// function complexity: those checks are relaxed for this file only.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
namespace qz::app {
namespace {

using namespace testenv;

/// NetStack decorator that records what the panel showed when the radio first came up.
class ObservingNet final : public hal::NetStack {
public:
    ObservingNet(testkit::FakeNetStack& inner, testkit::FakeEpdPanel& epd)
        : inner_(inner), epd_(epd) {}
    Status connect(const hal::WifiCredentials& creds, std::uint32_t timeout_ms) override {
        if (!seen_) {
            seen_ = true;
            shown_at_connect = epd_.displayed();
            asleep_at_connect = epd_.in_deep_sleep();
            updates_at_connect = epd_.full_updates() + epd_.partial_updates();
        }
        return inner_.connect(creds, timeout_ms);
    }
    Result<std::int64_t> sntp_sync(std::uint32_t timeout_ms, std::int64_t* rtc_us_at_utc) override {
        return inner_.sntp_sync(timeout_ms, rtc_us_at_utc);
    }
    Result<hal::HttpResponse>
    https_get(std::string_view url, std::span<char> body, std::uint32_t timeout_ms) override {
        return inner_.https_get(url, body, timeout_ms);
    }
    void shutdown() override { inner_.shutdown(); }
    [[nodiscard]] std::uint32_t radio_init_count() const override {
        return inner_.radio_init_count();
    }

    gfx::Framebuffer shown_at_connect;
    bool asleep_at_connect = false;
    std::uint32_t updates_at_connect = 0;

private:
    testkit::FakeNetStack& inner_;
    testkit::FakeEpdPanel& epd_;
    bool seen_ = false;
};

TEST(AppRadio, DisplayIsUpdatedBeforeAnyRadioActivity) {
    // The observer must see the *same* panel the App drives, so the Env is created with a pointer
    // to an observer that is wired to the Env's own fakes right after construction.
    class LateNet final : public hal::NetStack {
    public:
        hal::NetStack* target = nullptr;
        Status connect(const hal::WifiCredentials& c, std::uint32_t t) override {
            return target->connect(c, t);
        }
        Result<std::int64_t> sntp_sync(std::uint32_t t, std::int64_t* r) override {
            return target->sntp_sync(t, r);
        }
        Result<hal::HttpResponse>
        https_get(std::string_view u, std::span<char> b, std::uint32_t t) override {
            return target->https_get(u, b, t);
        }
        void shutdown() override { target->shutdown(); }
        [[nodiscard]] std::uint32_t radio_init_count() const override {
            return target->radio_init_count();
        }
    } late;
    Env env(true, &late);
    ObservingNet observer(env.net, env.epd);
    late.target = &observer;

    env.clock.set_true_utc_us(kT0Utc);
    provision_nvs(env, model::ConnectivityMode::kTimeOnly);
    const hal::SleepPlan plan = env.cold_boot();

    ASSERT_EQ(env.net.connect_calls(), 1U);
    // When the radio came up the panel had already finished the first frame and was asleep: the
    // '--:--' face (time still unknown), nothing blank.
    EXPECT_TRUE(observer.asleep_at_connect);
    EXPECT_EQ(observer.updates_at_connect, 1U);
    EXPECT_GT(std::ranges::count_if(observer.shown_at_connect.bits,
                                    [](std::uint8_t b) { return b != 0; }),
              50)
        << "the first frame (face with '--:--') was already on the panel, not a blank";
    // SNTP set the time; the display was refreshed again afterwards with the right minute.
    EXPECT_TRUE(env.api().time_info().valid);
    EXPECT_TRUE(same_frame(env.epd.displayed(),
                           expected_face(env, env.clock.true_utc_us() / kMin * kMin / kUs)));
    EXPECT_EQ(env.epd.violation_count(), 0U);
    EXPECT_EQ(env.net.shutdown_calls(), 1U);
    EXPECT_FALSE(env.net.radio_on());
    EXPECT_EQ(env.net.violations(), 0U);
    EXPECT_LT(plan.timer_us, kMin) << "minute ticks from now on";
    const RtcState state = load_rtc(env);
    EXPECT_NE(state.wake_log.newest().flags & model::kWakeFlagRadio, 0);
    EXPECT_EQ(state.conn.fail_streak, 0);
    EXPECT_EQ(state.conn.ever_synced, 1);
    EXPECT_EQ(env.api().sync_info().indicator, model::SyncIndicator::kOk);
    // Nothing is retried on the following ticks (next sync is a day away).
    hal::SleepPlan next = plan;
    for (int i = 0; i < 3; ++i) {
        next = env.wake_timer(next);
    }
    EXPECT_EQ(env.net.connect_calls(), 1U);
}

TEST(AppRadio, FailedSessionNeverBlanksTheFaceAndBacksOff) {
    Env env;
    env.clock.set_true_utc_us(kT0Utc);
    provision_nvs(env, model::ConnectivityMode::kTimeOnly);
    hal::SleepPlan plan = env.cold_boot(); // first sync succeeds
    ASSERT_TRUE(env.api().time_info().valid);
    ASSERT_EQ(env.net.connect_calls(), 1U);

    // A day later the sync is due and the access point is gone.
    env.net.script_connect(Error{Errc::kTimeout}, 10'000);
    env.clock.advance_rtc_us(24 * 3600 * kUs + 60 * kUs);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    plan = env.app->run_wake();

    EXPECT_EQ(env.net.connect_calls(), 2U);
    EXPECT_EQ(env.net.shutdown_calls(), 2U) << "teardown always runs";
    EXPECT_FALSE(env.net.radio_on());
    const std::int64_t now_s = env.clock.true_utc_us() / kUs;
    EXPECT_TRUE(same_frame(env.epd.displayed(), expected_face(env, now_s / 60 * 60)))
        << "the face shows the right minute after a failed session";
    const console::SyncInfo sync = env.api().sync_info();
    EXPECT_EQ(sync.indicator, model::SyncIndicator::kLastFailed);
    EXPECT_EQ(sync.fail_streak, 1);
    EXPECT_NE(sync.last_error, 0);
    EXPECT_GT(sync.next_time_sync, now_s + 12 * 60) << "15 min backoff +-10 %";
    EXPECT_LT(sync.next_time_sync, now_s + 17 * 60);
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_NE(rec.flags & model::kWakeFlagRadio, 0);
    EXPECT_NE(rec.flags & model::kWakeFlagError, 0);
    EXPECT_EQ(rec.error, static_cast<std::uint8_t>(static_cast<std::uint8_t>(Errc::kTimeout) + 1));
    // Not on every wake: the following ticks do not retry.
    for (int i = 0; i < 3; ++i) {
        plan = env.wake_timer(plan);
    }
    EXPECT_EQ(env.net.connect_calls(), 2U);
    EXPECT_EQ(env.epd.violation_count(), 0U);
}

TEST(AppRadio, FailedFirstSyncKeepsTheInvalidTimeFaceAndPacesRetries) {
    Env env;
    provision_nvs(env, model::ConnectivityMode::kTimeOnly);
    env.net.script_connect(Error{Errc::kTimeout}, 10'000);
    hal::SleepPlan plan = env.cold_boot();
    EXPECT_EQ(env.net.connect_calls(), 1U);
    EXPECT_FALSE(env.api().time_info().valid);
    EXPECT_TRUE(same_frame(env.epd.displayed(), expected_face(env, 0, false)));
    EXPECT_EQ(plan.timer_us, 600 * kUs);
    // Retry pacing on raw RTC time (15 min backoff): the next 10-min tick does not retry...
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.net.connect_calls(), 1U);
    // ...the one after (20 min since the attempt) does.
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.net.connect_calls(), 2U);
}

TEST(AppRadio, OffModeNeverTouchesTheNetStack) {
    Env env;
    provision_nvs(env, model::ConnectivityMode::kOff, true, true); // creds + location, mode Off
    env.clock.set_true_utc_us(kT0Utc);
    hal::SleepPlan plan = env.cold_boot();
    ASSERT_TRUE(static_cast<bool>(env.api().set_time_utc(kT0Utc / kUs)));
    for (int i = 0; i < 40; ++i) {
        plan = env.wake_timer(plan);
    }
    // A button session and a USB tether session too.
    env.clock.advance_rtc_us(10 * kUs);
    env.io.press(hal::kButtonBitBack);
    env.sleep.at(env.clock.rtc_us() + 100'000, 0, hal::kButtonBitBack);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, button_wake(hal::kButtonBitBack));
    (void)env.app->run_wake();
    env.io.set_usb(true, true);
    env.console.hook = [&](std::uint32_t n) {
        if (n == 10) {
            env.io.set_usb(false, false);
        }
    };
    (void)env.wake_on(usb_wake(), 10 * kUs);
    env.console.hook = nullptr;
    // A day passes in one wake.
    env.clock.advance_rtc_us(30 * 3600 * kUs);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    (void)env.app->run_wake();

    EXPECT_TRUE(env.net.calls().empty());
    EXPECT_EQ(env.net.radio_init_count(), 0U);
    EXPECT_EQ(env.net.connect_calls(), 0U);
    EXPECT_EQ(env.portal.starts, 0U);
    EXPECT_EQ(env.api().sync_info().indicator, model::SyncIndicator::kNone);
    EXPECT_EQ(env.api().sync_now(true, true).error().code, Errc::kInvalidState);
    EXPECT_EQ(env.net.connect_calls(), 0U);
}

TEST(AppRadio, WeatherIsFetchedAndShownAfterTheTimeSync) {
    Env env;
    env.clock.set_true_utc_us(kT0Utc);
    provision_nvs(env, model::ConnectivityMode::kTimeWeather, true, true);
    env.net.script_http(
        200,
        R"({"current":{"temperature_2m":12.3,"weather_code":3},"daily":{"temperature_2m_max":[15.1],"temperature_2m_min":[8.4]}})",
        200);
    (void)env.cold_boot();
    ASSERT_TRUE(env.api().time_info().valid);
    const console::WeatherInfo wx = env.api().weather();
    EXPECT_EQ(env.net.http_calls(), 0U) << "weather needs valid time: first session is time only";
    EXPECT_FALSE(wx.report.valid != 0 && wx.freshness == model::WeatherFreshness::kFresh);

    // The next tick finds weather due (time is valid now, piggyback window).
    hal::SleepPlan plan = env.wake_on(timer_wake(), 1000);
    for (int i = 0; i < 2; ++i) {
        plan = env.wake_timer(plan);
    }
    EXPECT_EQ(env.net.http_calls(), 1U);
    const console::WeatherInfo got = env.api().weather();
    EXPECT_EQ(got.freshness, model::WeatherFreshness::kFresh);
    EXPECT_EQ(got.report.temp_dc, 123);
    EXPECT_EQ(got.report.condition, model::WeatherCondition::kCloudy);
    EXPECT_TRUE(same_frame(env.epd.displayed(),
                           expected_face(env, env.clock.true_utc_us() / kMin * kMin / kUs)));
}

TEST(AppRadio, SyncNowViaConsoleApiRunsImmediately) {
    Env env;
    env.clock.set_true_utc_us(kT0Utc);
    provision_nvs(env, model::ConnectivityMode::kTimeOnly);
    (void)env.cold_boot();
    ASSERT_EQ(env.net.connect_calls(), 1U);
    ASSERT_TRUE(static_cast<bool>(env.api().sync_now(true, true)));
    EXPECT_EQ(env.net.connect_calls(), 2U) << "manual sync ignores the schedule";
    EXPECT_EQ(env.api().sync_now(false, false).error().code, Errc::kBadArgs);
    env.net.script_connect(Error{Errc::kTimeout}, 1000);
    EXPECT_FALSE(static_cast<bool>(env.api().sync_now(true, true)));
    EXPECT_EQ(env.net.shutdown_calls(), 3U);
}

// ---- power policy ---------------------------------------------------------------------------

TEST(AppPower, LowBatteryForbidsRadioTapWakeAndSyncNow) {
    Env env;
    env.clock.set_true_utc_us(kT0Utc);
    provision_nvs(env, model::ConnectivityMode::kTimeOnly);
    (void)env.cold_boot();
    ASSERT_EQ(env.net.connect_calls(), 1U);
    ASSERT_TRUE(static_cast<bool>(env.api().apply_setting(settings::Key::kTapWake, "on")));
    ASSERT_TRUE(static_cast<bool>(env.api().fake_battery_mv(std::uint16_t{3550})));

    env.clock.advance_rtc_us(25 * 3600 * kUs); // sync due
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    const hal::SleepPlan plan = env.app->run_wake();
    EXPECT_EQ(env.api().battery().level, model::PowerLevel::kLow);
    EXPECT_EQ(env.net.connect_calls(), 1U) << "no radio session while Low";
    EXPECT_FALSE(plan.wake_on_accel) << "tap wake off while Low";
    EXPECT_EQ(env.api().sync_now(true, true).error().code, Errc::kBatteryLow);
    EXPECT_LT(plan.timer_us, kMin) << "display cadence unchanged in Low";

    // Recovery: back to a healthy cell, the overdue sync runs.
    ASSERT_TRUE(
        static_cast<bool>(env.api().fake_battery_mv(std::uint16_t{3800}))); // hysteresis: >= 3700
    ASSERT_TRUE(static_cast<bool>(env.api().fake_battery_mv(std::nullopt)));
    env.clock.advance_rtc_us(60 * kUs);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    (void)env.app->run_wake();
    EXPECT_EQ(env.net.connect_calls(), 2U);
}

TEST(AppPower, SaverUpdatesEveryFiveMinutes) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().fake_battery_mv(std::uint16_t{3450})));
    plan = env.wake_on(timer_wake(), 30 * kUs);
    EXPECT_EQ(env.api().battery().level, model::PowerLevel::kSaver);
    for (int i = 0; i < 3; ++i) {
        const std::int64_t next = env.clock.true_utc_us() + plan.timer_us;
        EXPECT_GT(plan.timer_us, kMin) << "5-minute cadence";
        EXPECT_EQ((next + 600'000) / (5 * kMin) * (5 * kMin) / kMin % 5, 0)
            << "wake lands just before a 5-minute boundary";
        plan = env.wake_timer(plan);
        const std::int64_t now = env.clock.true_utc_us();
        EXPECT_TRUE(same_frame(env.epd.displayed(), expected_face(env, now / kMin * kMin / kUs)));
    }
}

TEST(AppPower, CriticalShowsChargeMeOnceThenOnlyButtonsAndUsbWake) {
    Env env;
    (void)env.boot_with_time();
    env.accel.add_steps(40);
    const std::uint32_t writes = env.kv.write_count();
    ASSERT_TRUE(static_cast<bool>(env.api().fake_battery_mv(std::uint16_t{3300})));
    const std::uint32_t full = env.epd.full_updates();

    hal::SleepPlan plan = env.wake_on(timer_wake(), 30 * kUs);
    EXPECT_EQ(env.api().battery().level, model::PowerLevel::kCritical);
    EXPECT_EQ(plan.timer_us, -1) << "Critical: no timer";
    EXPECT_TRUE(plan.wake_on_buttons);
    EXPECT_FALSE(plan.wake_on_accel);
    EXPECT_EQ(env.epd.full_updates(), full + 1) << "'Charge me' screen with a full refresh";
    EXPECT_GT(env.kv.write_count(), writes) << "steps flushed to NVS on entering Critical";
    EXPECT_TRUE(env.epd.in_deep_sleep());

    // A second wake does not redraw it.
    plan = env.wake_on(button_wake(0), 100 * kUs); // spurious wake without a button
    (void)plan;
    EXPECT_EQ(env.epd.full_updates(), full + 1);

    // A button shows the face with the time for 10 s, then Charge me again.
    env.clock.advance_rtc_us(5 * kUs);
    env.io.press(hal::kButtonBitMenu);
    env.sleep.at(env.clock.rtc_us() + 50'000, 0, hal::kButtonBitMenu);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, button_wake(hal::kButtonBitMenu));
    const std::int64_t before = env.clock.rtc_us();
    plan = env.app->run_wake();
    EXPECT_GE(env.clock.rtc_us() - before, 10 * kUs);
    EXPECT_EQ(plan.timer_us, -1);
    EXPECT_FALSE(same_frame(env.epd.displayed(), expected_face(env, env.clock.true_utc_us() / kUs)))
        << "Charge me is back on the panel after the 10 s face";
    EXPECT_EQ(env.epd.full_updates(), full + 2);
}

TEST(AppPower, UsbAttachLeavesCritical) {
    Env env;
    (void)env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().fake_battery_mv(std::uint16_t{3300})));
    (void)env.wake_on(timer_wake(), 30 * kUs);
    ASSERT_EQ(env.api().battery().level, model::PowerLevel::kCritical);

    env.io.set_usb(true, true);
    env.console.hook = [&](std::uint32_t n) {
        if (n == 12) {
            env.io.set_usb(false, false);
        }
    };
    (void)env.wake_on(usb_wake(), 10 * kUs);
    env.console.hook = nullptr;
    EXPECT_NE(env.api().battery().level, model::PowerLevel::kCritical);
    EXPECT_EQ(env.console_port.start_count(), 1U) << "tethered, not stuck on the Charge me screen";
}

} // namespace
} // namespace qz::app
// NOLINTEND(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
