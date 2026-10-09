// Phone sync sessions (ARCHITECTURE.md section 13a) end to end against FakePhoneLink: menu start,
// pairing code on the panel, commands over the secure link, every way a session ends, and "off
// means off".
#include "app_test_env.hpp"

#include <gtest/gtest.h>

#include <string>

namespace qz::app {
namespace {

using namespace testenv;
using hal::PhoneLinkState;

/// Wakes on MENU and walks Menu > Phone > Sync with phone (radio build: Phone is the 8th item).
hal::SleepPlan start_from_menu(Env& env) {
    env.clock.advance_rtc_us(20 * kUs);
    std::int64_t at = env.clock.rtc_us() + 1'000'000;
    auto click = [&](std::uint8_t b) {
        env.sleep.at(at, b, 0);
        env.sleep.at(at + 500'000, 0, b);
        at += 2'000'000;
    };
    for (int i = 0; i < 7; ++i) {
        click(hal::kButtonBitDown);
    }
    click(hal::kButtonBitMenu); // Phone submenu
    click(hal::kButtonBitMenu); // Sync with phone
    env.io.press(hal::kButtonBitMenu);
    env.sleep.at(env.clock.rtc_us() + 100'000, 0, hal::kButtonBitMenu);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, button_wake(hal::kButtonBitMenu));
    return env.app->run_wake();
}

bool sent_contains(const Env& env, std::string_view needle) {
    return joined(env.phone.sent()).find(needle) != std::string::npos;
}

TEST(PhoneSync, MenuStartsASessionThatPairsSyncsAndEndsWhenThePhoneLeaves) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    std::uint32_t partials_at_pairing = 0;
    std::string screen_while_pairing;
    env.phone.hook = [&](std::uint32_t n) {
        if (n == 5) {
            env.phone.set_state(PhoneLinkState::kPairing);
            env.phone.set_passkey(47'201);
        } else if (n == 8) {
            partials_at_pairing = env.epd.partial_updates();
            screen_while_pairing = std::string(env.api().current_screen());
        } else if (n == 10) {
            env.phone.set_passkey(0);
            env.phone.set_state(PhoneLinkState::kSecure);
            env.phone.push_request("#p1 settings set units f");
            env.phone.push_request("#p2 weather push 215 rain 250 120");
            env.phone.push_request("#p3 reboot");
            env.phone.push_request("#p4 factory-reset confirm");
        } else if (n == 30) {
            env.phone.set_state(PhoneLinkState::kAdvertising); // the page was closed
        }
    };
    const std::uint32_t partials = env.epd.partial_updates();
    (void)start_from_menu(env);
    env.phone.hook = nullptr;

    EXPECT_EQ(env.phone.radio_init_count(), 1U);
    EXPECT_EQ(env.phone.name(), "Quartz-B2C3") << "low 16 bits of the fake chip id";
    EXPECT_FALSE(env.phone.running()) << "the radio is down again";
    EXPECT_EQ(screen_while_pairing, "phone_sync");
    EXPECT_GT(partials_at_pairing, partials) << "the pairing code reached the panel";

    EXPECT_TRUE(sent_contains(env, "@QZ1 p1 OK"));
    EXPECT_TRUE(sent_contains(env, "@QZ1 p2 OK"));
    EXPECT_TRUE(sent_contains(env, "@QZ1 p3 ERR unsupported"));
    EXPECT_TRUE(sent_contains(env, "@QZ1 p4 ERR unsupported"));
    EXPECT_EQ(env.sys.restart_count(), 0U);
    EXPECT_EQ(env.api().current_settings().temp_unit, model::TempUnit::kFahrenheit);
    const console::WeatherInfo wx = env.api().weather();
    ASSERT_EQ(wx.report.valid, 1);
    EXPECT_EQ(wx.report.temp_dc, 215);
    EXPECT_EQ(wx.report.faked, 0) << "a pushed report is real data";
    EXPECT_EQ(env.api().current_screen(), "face");
    EXPECT_EQ(env.epd.violation_count(), 0U);
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_LT(rec.awake_ms, 40'000U)
        << "the result shows for seconds, not the 3 min screen timeout";
}

TEST(PhoneSync, NobodyConnectsAndTheRadioStopsAfterTheConnectWindow) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    std::int64_t stopped_at = -1;
    const std::int64_t begin = env.clock.rtc_us();
    env.phone.hook = [&](std::uint32_t /*n*/) {
        if (!env.phone.running() && stopped_at < 0) {
            stopped_at = env.clock.rtc_us();
        }
    };
    (void)start_from_menu(env);
    env.phone.hook = nullptr;
    EXPECT_EQ(env.phone.radio_init_count(), 1U);
    EXPECT_FALSE(env.phone.running());
    // 20 s idle + ~19 s of clicks, then the 120 s connect window: well under 3 minutes in all.
    const model::WakeRecord rec = env.api().wake_record(env.api().wake_record_count() - 1);
    EXPECT_LT(rec.awake_ms, 180'000U) << "no radio left running unattended";
    EXPECT_GT(env.clock.rtc_us() - begin, 120 * kUs);
}

TEST(PhoneSync, BackStopsTheSession) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    env.phone.hook = [&](std::uint32_t n) {
        if (n == 5) {
            env.io.press(hal::kButtonBitBack);
        } else if (n == 9) {
            env.io.release(hal::kButtonBitBack);
        }
    };
    (void)start_from_menu(env);
    env.phone.hook = nullptr;
    EXPECT_EQ(env.phone.radio_init_count(), 1U);
    EXPECT_FALSE(env.phone.running());
    EXPECT_LT(env.phone.receive_count(), 20U) << "stopped right after BACK";
}

TEST(PhoneSync, OffMeansTheRadioIsNeverPowered) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().apply_setting(settings::Key::kPhoneSync, "off")));
    // Same clicks: with the feature off the Phone list only holds the switch, so the second MENU
    // turns it back on instead of starting a session.
    (void)start_from_menu(env);
    EXPECT_EQ(env.phone.radio_init_count(), 0U);
    EXPECT_TRUE(env.api().current_settings().phone_sync);
}

TEST(PhoneSync, TurningTheSettingOffFromThePhoneEndsTheSession) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    env.phone.hook = [&](std::uint32_t n) {
        if (n == 3) {
            env.phone.set_state(PhoneLinkState::kSecure);
            env.phone.push_request("#x settings set phone off");
        }
    };
    (void)start_from_menu(env);
    env.phone.hook = nullptr;
    EXPECT_FALSE(env.phone.running());
    EXPECT_FALSE(env.api().current_settings().phone_sync);
    EXPECT_LT(env.phone.receive_count(), 10U);
}

TEST(PhoneSync, CompiledOutRefusesTheSettingAndHasNoLink) {
    Env env; // no enable_phone(): BuildFeatures.phone with a null link, as in an offline image
    (void)env.boot_with_time();
    EXPECT_EQ(env.api().apply_setting(settings::Key::kPhoneSync, "on").error().code,
              Errc::kUnsupported);
    EXPECT_EQ(env.api().forget_phones().error().code, Errc::kUnsupported);
}

TEST(PhoneSync, FactoryResetForgetsPhonesToo) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    ASSERT_TRUE(static_cast<bool>(env.api().factory_reset()));
    EXPECT_EQ(env.phone.forget_count(), 1U);
}

TEST(PhoneSync, ForgetPhonesClearsBonds) {
    Env env;
    env.enable_phone();
    (void)env.boot_with_time();
    EXPECT_TRUE(static_cast<bool>(env.api().forget_phones()));
    EXPECT_EQ(env.phone.forget_count(), 1U);
}

} // namespace
} // namespace qz::app
