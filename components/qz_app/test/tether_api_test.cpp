// USB attach / tethered loop (ARCHITECTURE.md section 17) and the console exercised end to end
// through the real dispatcher against the real DeviceApi (section 16).
#include "app_test_env.hpp"
#include "qz/console/registry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

// Test arithmetic mixes int literals with int64 microsecond constants and gtest macros inflate
// function complexity: those checks are relaxed for this file only.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
namespace qz::app {
namespace {

using namespace testenv;

bool contains(const std::string& text, std::string_view needle) {
    return text.contains(needle);
}

/// First response line of the form "@QZ1 <id> ..." among the lines the console port captured.
std::string response_for(const Env& env, std::string_view id) {
    const std::string prefix = "@QZ1 " + std::string(id) + " ";
    for (const std::string& line : env.console_port.sent()) {
        if (line.starts_with(prefix)) {
            return line;
        }
    }
    return {};
}

// ---- tethered loop --------------------------------------------------------------------------

TEST(AppTether, UsbAttachServesConsoleThenSleepCommandSleepsOnce) {
    Env env;
    (void)env.boot_with_time();
    env.io.set_usb(true, true);
    for (const char* line : {"#a1 status",
                             "#a2 time get",
                             "#a3 settings set tfmt 12h",
                             "#a4 settings get tfmt",
                             "#a5 btn menu click",
                             "#a6 screen get",
                             "#a7 display crc",
                             "#a8 face list",
                             "#a9 sleep 30"}) {
        env.console_port.push_request(line);
    }
    const hal::SleepPlan plan = env.wake_on(usb_wake(), 20 * kUs);

    ASSERT_EQ(env.console_port.start_count(), 1U);
    const std::vector<std::string>& sent = env.console_port.sent();
    ASSERT_FALSE(sent.empty());
    EXPECT_TRUE(sent.front().starts_with("@QZ1 ! EVT ")) << sent.front();
    EXPECT_TRUE(contains(sent.front(), "\"evt\":\"ready\"")) << sent.front();
    EXPECT_TRUE(contains(sent.front(), "\"reset\":\"deepsleep\"")) << sent.front();

    for (const char* id : {"a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9"}) {
        EXPECT_TRUE(contains(response_for(env, id), " OK ")) << id << ": " << response_for(env, id);
    }
    EXPECT_TRUE(contains(response_for(env, "a1"), "\"power_state\":\"normal\""));
    EXPECT_TRUE(contains(response_for(env, "a4"), "12h"));
    EXPECT_TRUE(contains(response_for(env, "a6"), "menu")) << response_for(env, "a6");
    EXPECT_EQ(env.api().current_settings().hour_format, model::HourFormat::k12h);

    // `sleep 30`: the response went out first, then one deep sleep of exactly 30 s with USB wake
    // left off (VBUS is high).
    EXPECT_EQ(plan.timer_us, 30 * kUs);
    EXPECT_FALSE(plan.wake_on_usb);
    EXPECT_TRUE(plan.wake_on_buttons);
    EXPECT_EQ(env.console_port.stop_count(), 1U);
    EXPECT_FALSE(env.console_port.running());
    EXPECT_EQ(env.app->tether().state(), TetherState::kSleepOnce);
    EXPECT_TRUE(env.app->tether().may_deep_sleep());
}

TEST(AppTether, DetachStopsConsoleAndPlansNormalTick) {
    Env env;
    (void)env.boot_with_time();
    env.io.set_usb(true, true);
    env.console.hook = [&](std::uint32_t n) {
        if (n == 12) {
            env.io.set_usb(false, false); // unplugged 3 s into the session
        }
    };
    const hal::SleepPlan plan = env.wake_on(usb_wake(), 20 * kUs);
    env.console.hook = nullptr;

    EXPECT_EQ(env.console_port.start_count(), 1U);
    EXPECT_EQ(env.console_port.stop_count(), 1U);
    EXPECT_TRUE(contains(joined(env.console_port.sent()), "\"evt\":\"detach\""));
    EXPECT_EQ(env.app->tether().state(), TetherState::kUntethered);
    EXPECT_TRUE(env.app->tether().may_deep_sleep());
    EXPECT_GT(plan.timer_us, 0);
    EXPECT_LT(plan.timer_us, kMin);
    EXPECT_TRUE(plan.wake_on_usb) << "USB is gone: attach wake armed again";
}

TEST(AppTether, MinuteTicksKeepRunningWhileTethered) {
    Env env;
    (void)env.boot_with_time();
    env.io.set_usb(true, true);
    const std::int64_t unplug_at = env.clock.rtc_us() + 140 * kUs; // two minute flips
    env.console.hook = [&](std::uint32_t /*n*/) {
        if (env.clock.rtc_us() >= unplug_at) {
            env.io.set_usb(false, false);
        }
    };
    const std::uint32_t partials = env.epd.partial_updates();
    (void)env.wake_on(usb_wake(), 20 * kUs);
    env.console.hook = nullptr;

    EXPECT_GE(env.epd.partial_updates(), partials + 2) << "two minute flips while tethered";
    int ticks = 0;
    for (std::size_t i = 0; i < env.api().wake_record_count(); ++i) {
        ticks += env.api().wake_record(i).cause == model::WakeCause::kTetheredTick ? 1 : 0;
    }
    EXPECT_GE(ticks, 2);
    EXPECT_TRUE(contains(joined(env.console_port.sent()), "\"evt\":\"wake\""));
    EXPECT_EQ(env.epd.violation_count(), 0U);
}

TEST(AppTether, RebootCommandRestartsAfterTheResponse) {
    Env env;
    (void)env.boot_with_time();
    env.io.set_usb(true, true);
    env.console_port.push_request("#r1 reboot");
    (void)env.wake_on(usb_wake(), 20 * kUs);
    EXPECT_EQ(env.sys.restart_count(), 1U);
    EXPECT_TRUE(contains(response_for(env, "r1"), "\"rebooting\":true"));
}

TEST(AppTether, BatteryWakesNeverStartTheConsole) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time();
    for (int i = 0; i < 5; ++i) {
        plan = env.wake_timer(plan);
    }
    EXPECT_EQ(env.console_port.start_count(), 0U);
    EXPECT_TRUE(env.console_port.sent().empty());
    EXPECT_EQ(env.app->tether().state(), TetherState::kUntethered);
}

TEST(AppTether, ProvisioningStartsStopsAndNeverPrintsThePassword) {
    Env env;
    (void)env.boot_with_time();
    env.io.set_usb(true, true);
    env.console_port.push_request("#v1 provision start");
    env.console_port.push_request("#v2 provision stop");
    env.console_port.push_request("#v3 sleep 5");
    (void)env.wake_on(usb_wake(), 10 * kUs);
    EXPECT_EQ(env.portal.starts, 1U);
    EXPECT_EQ(env.portal.stops, 1U);
    EXPECT_TRUE(contains(response_for(env, "v1"), "Quartz-")) << response_for(env, "v1");
    EXPECT_FALSE(env.portal.running);
    const std::string password = env.portal.password();
    EXPECT_GE(password.size(), 12U);
    EXPECT_FALSE(contains(joined(env.console_port.sent()), password))
        << "password shown on the watch only";
}

TEST(AppTether, ProvisioningIsRefusedOnLowBattery) {
    Env env;
    (void)env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().fake_battery_mv(std::uint16_t{3550})));
    FixedString<32> ssid;
    std::uint16_t expires = 0;
    EXPECT_EQ(env.api().start_provisioning(ssid, expires).error().code, Errc::kBatteryLow);
    EXPECT_EQ(env.portal.starts, 0U);
}

TEST(AppTimerTick, DailyGoalVibratesOnceWhenCrossed) {
    Env env;
    hal::SleepPlan plan = env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().apply_setting(settings::Key::kStepGoal, "500")));
    env.accel.add_steps(300);
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.io.vibration_pulses(), 0U);
    env.accel.add_steps(300);
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.io.vibration_pulses(), 1U) << "crossing the goal buzzes once";
    env.accel.add_steps(300);
    plan = env.wake_timer(plan);
    EXPECT_EQ(env.io.vibration_pulses(), 1U);
    EXPECT_FALSE(env.io.vibrating());
}

// ---- console end to end ---------------------------------------------------------------------

class ConsoleHarness {
public:
    explicit ConsoleHarness(Env& env) : dispatcher_(registry_, env.api(), true) {
        EXPECT_TRUE(static_cast<bool>(console::register_builtin_commands(registry_)));
    }
    std::string run(const std::string& text) {
        std::vector<char> line(text.begin(), text.end());
        const std::string_view reply =
            dispatcher_.handle_line(std::span<char>(line.data(), line.size()), response_);
        return std::string(reply);
    }

private:
    console::Registry registry_;
    console::Dispatcher dispatcher_;
    std::array<char, console::kMaxResponseBytes> response_{};
};

TEST(AppConsole, TimeSetGetAndTheFaceFollows) {
    Env env;
    (void)env.cold_boot();
    ConsoleHarness console(env);
    EXPECT_TRUE(contains(console.run("#t0 time get"), "\"valid\":false"));

    env.clock.set_true_utc_us(1'806'582'600LL * kUs);
    const std::string set = console.run("#t1 time set 2027-03-31T12:30:00Z");
    EXPECT_TRUE(contains(set, "@QZ1 t1 OK")) << set;
    const std::string got = console.run("#t2 time get");
    EXPECT_TRUE(contains(got, "2027-03-31")) << got;
    EXPECT_TRUE(contains(got, "\"valid\":true")) << got;
    EXPECT_TRUE(contains(got, "console")) << got; // source
    EXPECT_TRUE(
        same_frame(env.epd.displayed(), expected_face(env, env.api().time_info().utc_us / kUs)));
    // Committed: a following wake still knows the time.
    const hal::SleepPlan plan = env.wake_on(timer_wake(), 5 * kUs);
    EXPECT_TRUE(env.api().time_info().valid);
    EXPECT_LT(plan.timer_us, kMin);
}

TEST(AppConsole, SettingsPressScreensAndSteps) {
    Env env;
    (void)env.boot_with_time();
    ConsoleHarness console(env);

    EXPECT_TRUE(contains(console.run("#s1 settings set goal 5000"), "@QZ1 s1 OK"));
    EXPECT_TRUE(contains(console.run("#s2 settings get goal"), "5000"));
    EXPECT_TRUE(contains(console.run("#s3 settings set goal 123"), "@QZ1 s3 ERR bad_args"));
    EXPECT_EQ(env.api().current_settings().step_goal, 5000U);
    EXPECT_TRUE(env.kv.contains_text("goal"));

    EXPECT_TRUE(contains(console.run("#b1 btn menu click"), "menu"));
    EXPECT_EQ(env.api().current_screen(), "menu");
    EXPECT_TRUE(contains(console.run("#b2 screen get"), "menu"));
    EXPECT_TRUE(contains(console.run("#b3 btn back click"), "face"));
    EXPECT_EQ(env.api().current_screen(), "face");
    EXPECT_TRUE(contains(console.run("#b4 screen list"), "face"));
    EXPECT_TRUE(contains(console.run("#b5 screen show about"), "@QZ1 b5 OK"));
    EXPECT_EQ(env.api().current_screen(), "about");

    EXPECT_TRUE(contains(console.run("#p1 steps inject 100"), "@QZ1 p1 OK"));
    EXPECT_EQ(env.api().steps().today, 100U);

    EXPECT_TRUE(contains(console.run("#v1 face list"), "@QZ1 v1 OK"));
    EXPECT_TRUE(contains(console.run("#v2 version"), "\"proto\":1"));
    EXPECT_TRUE(contains(console.run("#v3 diag clock"), "\"drift_ppb\""));
    EXPECT_TRUE(contains(console.run("#v4 diag nothing"), "ERR not_found"));
    EXPECT_TRUE(contains(console.run("#v5 selftest list"), "@QZ1 v5 OK"));
}

TEST(AppConsole, DisplayDumpCrcRefreshAndLog) {
    Env env;
    (void)env.boot_with_time();
    ConsoleHarness console(env);
    const std::string dump = console.run("#d1 display dump");
    EXPECT_TRUE(contains(dump, "\"w\":200")) << dump.substr(0, 80);
    EXPECT_TRUE(contains(dump, "1bpp-msb"));
    EXPECT_GT(dump.size(), 6600U) << "5000 bytes base64";
    EXPECT_TRUE(contains(console.run("#d2 display crc"), "crc32"));

    const std::uint32_t full = env.epd.full_updates();
    EXPECT_TRUE(contains(console.run("#d3 display refresh full"), "@QZ1 d3 OK"));
    EXPECT_EQ(env.epd.full_updates(), full + 1);
    const std::uint32_t partial = env.epd.partial_updates();
    EXPECT_TRUE(contains(console.run("#d4 display refresh partial"), "@QZ1 d4 OK"));
    EXPECT_EQ(env.epd.partial_updates(), partial + 1) << "an explicit refresh always updates";
    EXPECT_TRUE(
        same_frame(env.epd.displayed(), expected_face(env, env.api().time_info().utc_us / kUs)));

    EXPECT_TRUE(contains(console.run("#l1 log wakes 3"), "@QZ1 l1 OK"));
    EXPECT_TRUE(contains(console.run("#l2 log clear"), "@QZ1 l2 OK"));
    EXPECT_EQ(env.api().wake_record_count(), 0U);
}

TEST(AppConsole, BatteryFakeSleepAndLifecycleRequests) {
    Env env;
    (void)env.boot_with_time();
    ConsoleHarness console(env);
    EXPECT_TRUE(contains(console.run("#f5 vibrate 50"), "@QZ1 f5 OK"));
    EXPECT_EQ(env.io.vibration_pulses(), 1U);
    EXPECT_FALSE(env.io.vibrating());
    EXPECT_TRUE(contains(console.run("#f1 battery fake 3550"), "low")) << "3550 mV is Low";
    EXPECT_EQ(env.api().battery().level, model::PowerLevel::kLow);
    EXPECT_TRUE(env.api().battery().faked);
    EXPECT_TRUE(contains(console.run("#f2 battery fake off"), "@QZ1 f2 OK"));
    EXPECT_FALSE(env.api().battery().faked);
    EXPECT_TRUE(contains(console.run("#f3 sleep 30"), "\"seconds\":30"));
    EXPECT_TRUE(contains(console.run("#f4 sleep 99999"), "ERR bad_args"));
    EXPECT_TRUE(contains(console.run("#f6 sync now"), "ERR invalid_state")) << "Off means off";
    EXPECT_EQ(env.net.connect_calls(), 0U);
}

TEST(AppConsole, WifiNeverEchoesThePassword) {
    Env env;
    (void)env.boot_with_time();
    ConsoleHarness console(env);
    const std::string set = console.run("#w1 wifi set HomeNet hunter2hunter2");
    EXPECT_TRUE(contains(set, "@QZ1 w1 OK")) << set;
    EXPECT_FALSE(contains(set, "hunter2"));
    const std::string status = console.run("#w2 wifi status");
    EXPECT_TRUE(contains(status, "HomeNet"));
    EXPECT_TRUE(contains(status, "\"has_password\":true"));
    EXPECT_FALSE(contains(status, "hunter2"));
    EXPECT_FALSE(contains(joined(env.console_port.sent()), "hunter2"));
    EXPECT_TRUE(contains(console.run("#w3 wifi clear"), "@QZ1 w3 OK"));
    EXPECT_TRUE(contains(console.run("#w4 wifi status"), "\"has_password\":false"));
}

TEST(AppConsole, RadioCommandsAreUnsupportedWithoutTheRadio) {
    Env env(false);
    (void)env.boot_with_time();
    ConsoleHarness console(env);
    EXPECT_TRUE(contains(console.run("#n1 wifi set HomeNet hunter2hunter2"), "ERR unsupported"));
    EXPECT_TRUE(contains(console.run("#n2 settings set conn time"), "ERR unsupported"));
    EXPECT_TRUE(contains(console.run("#n3 sync now"), "ERR unsupported"));
    EXPECT_FALSE(env.api().firmware().radio_compiled);
}

TEST(AppConsole, FactoryResetWipesSettingsKeepsTime) {
    Env env;
    (void)env.boot_with_time();
    ConsoleHarness console(env);
    EXPECT_TRUE(contains(console.run("#r0 settings set goal 5000"), "OK"));
    EXPECT_TRUE(contains(console.run("#r1 factory-reset"), "ERR bad_args"));
    EXPECT_TRUE(contains(console.run("#r2 factory-reset confirm"), "@QZ1 r2 OK"));
    EXPECT_EQ(env.api().current_settings().step_goal, 0U);
    EXPECT_TRUE(env.api().time_info().valid) << "the RTC timer keeps running: time is kept";
    EXPECT_EQ(env.kv.entry_count("qz_set"), 0U);
    EXPECT_EQ(env.api().current_screen(), "face");
}

} // namespace
} // namespace qz::app
// NOLINTEND(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
