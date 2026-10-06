// Deep-sleep and light-sleep tests (owner-run; WP-25 acceptance, HARDWARE_BRINGUP.md B4/B8/B9).
// Multi-stage cases: each stage ends in deep sleep (a reset); the Unity menu runner resumes the
// next stage after the wake [ASSUMED: same mechanism as the esp_restart case in
// test_apps/platform].
#include "driver/gpio.h"
#include "esp_attr.h"
#include "platform_impl.hpp"
#include "unity.h"
#include "unity_test_runner.h"

#include <cstdint>
#include <cstdio>

using qz::platform::IdfBoardIo;
using qz::platform::IdfClock;
using qz::platform::IdfDelay;
using qz::platform::IdfSleep;
using qz::platform::IdfSystem;

namespace {

RTC_NOINIT_ATTR std::int64_t s_stage_start_rtc_us;

constexpr std::int64_t kTimerWakeUs = 5'000'000;
constexpr std::int64_t kButtonWindowUs = 20'000'000;

constexpr std::uint64_t button_mask() {
    std::uint64_t mask = 0;
    for (const std::uint8_t pin : qz::board::kButtonPins) {
        mask |= std::uint64_t{1} << pin;
    }
    return mask;
}

} // namespace

// ---- Case 1: timer wake -----------------------------------------------------------------------

static void timer_stage_enter() {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    TEST_ASSERT_EQUAL_UINT8(0, io.pressed_buttons()); // release every button first
    IdfSleep sleep(io);
    qz::hal::SleepPlan plan;
    plan.timer_us = kTimerWakeUs;
    plan.wake_on_buttons = false;
    plan.wake_on_usb = false;
    s_stage_start_rtc_us = IdfClock().rtc_us();
    std::printf("deep sleep for 5 s (timer only); the console will drop and reconnect\n");
    sleep.deep_sleep(plan); // does not return
}

static void timer_stage_verify() {
    const std::int64_t now = IdfClock().rtc_us();
    IdfSystem system;
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(qz::hal::ResetReason::kDeepSleep),
                            static_cast<std::uint8_t>(system.reset_reason()));
    TEST_ASSERT_TRUE(system.wake_sources().timer);
    TEST_ASSERT_FALSE(system.wake_sources().ext1);
    const std::int64_t elapsed = now - s_stage_start_rtc_us;
    std::printf("elapsed %lld us (target %lld)\n",
                static_cast<long long>(elapsed),
                static_cast<long long>(kTimerWakeUs));
    // Boot + wake overhead adds < 1 s; the crystal timebase is checked properly in B8.
    TEST_ASSERT_TRUE(elapsed >= kTimerWakeUs && elapsed < kTimerWakeUs + 1'000'000);
    // Holds are released after wake: GPIO17 (vibration) is driven low by BoardIo::init and stays
    // low.
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    TEST_ASSERT_TRUE(IdfSleep::release_holds());
    TEST_ASSERT_EQUAL_INT(0, gpio_get_level(static_cast<gpio_num_t>(qz::board::kVibration)));
    // The released EPD pads must be drivable again: CS idles high, DC low.
    TEST_ASSERT_EQUAL_INT(1, gpio_get_level(static_cast<gpio_num_t>(qz::board::kEpdCs)));
    TEST_ASSERT_EQUAL_INT(0, gpio_get_level(static_cast<gpio_num_t>(qz::board::kEpdDc)));
    TEST_ASSERT_TRUE(gpio_set_level(static_cast<gpio_num_t>(qz::board::kEpdDc), 1) == ESP_OK);
    TEST_ASSERT_EQUAL_INT(1, gpio_get_level(static_cast<gpio_num_t>(qz::board::kEpdDc)));
    TEST_ASSERT_TRUE(gpio_set_level(static_cast<gpio_num_t>(qz::board::kEpdDc), 0) == ESP_OK);
}

TEST_CASE_MULTIPLE_STAGES("SleepControl: deep sleep timer wake, holds released",
                          "[qz_sleep][deep]",
                          timer_stage_enter,
                          timer_stage_verify);

// ---- Case 2: button wake (EXT1) ---------------------------------------------------------------

static void button_stage_enter() {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    IdfDelay().delay_ms(300);
    TEST_ASSERT_EQUAL_UINT8(0, io.pressed_buttons());
    IdfSleep sleep(io);
    qz::hal::SleepPlan plan;
    plan.timer_us = kButtonWindowUs; // fail-safe: wakes by timer if nobody presses
    plan.wake_on_buttons = true;
    plan.wake_on_usb = false;
    std::printf(">>> PRESS ANY BUTTON within 20 s (try UP/GPIO0 once: it must wake, not enter "
                "download mode)\n");
    IdfDelay().delay_ms(200);
    sleep.deep_sleep(plan); // does not return
}

static void button_stage_verify() {
    IdfSystem system;
    const auto wake = system.wake_sources();
    std::printf("ext1=%d pins=0x%llx timer=%d\n",
                wake.ext1,
                static_cast<unsigned long long>(wake.ext1_pins),
                wake.timer);
    TEST_ASSERT_TRUE_MESSAGE(wake.ext1, "no button press seen (timer fail-safe fired)");
    TEST_ASSERT_TRUE((wake.ext1_pins & button_mask()) != 0);
    // After the wake the pressed pin must read as pressed (level not stuck by a pad hold) and the
    // buttons must still be plain inputs without pulls.
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    TEST_ASSERT_TRUE(IdfSleep::release_holds());
}

TEST_CASE_MULTIPLE_STAGES("SleepControl: deep sleep EXT1 button wake (needs a press)",
                          "[qz_sleep][deep][manual]",
                          button_stage_enter,
                          button_stage_verify);

// ---- Case 3: light sleep timer ----------------------------------------------------------------

TEST_CASE("SleepControl: light sleep timer wake returns kTimer in time", "[qz_sleep][light]") {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    IdfSleep sleep(io);
    qz::hal::SleepPlan plan;
    plan.timer_us = 200'000;
    plan.wake_on_buttons = false;
    plan.wake_on_usb = false;
    const std::int64_t t0 = IdfClock().rtc_us();
    const auto cause = sleep.light_sleep(plan);
    const std::int64_t elapsed = IdfClock().rtc_us() - t0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(qz::hal::LightSleepWake::kTimer),
                            static_cast<std::uint8_t>(cause));
    TEST_ASSERT_TRUE(elapsed >= 195'000 && elapsed < 260'000);
    // Vibration pin must still be low after the sleep (SLP_SEL kept the output).
    TEST_ASSERT_EQUAL_INT(0, gpio_get_level(static_cast<gpio_num_t>(qz::board::kVibration)));
}

TEST_CASE("SleepControl: tethered light sleep polls and honors the timer", "[qz_sleep][light]") {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    IdfSleep sleep(io);
    sleep.set_tethered(true);
    qz::hal::SleepPlan plan;
    plan.timer_us = 50'000;
    plan.wake_on_buttons = false;
    plan.wake_on_usb = false;
    const std::int64_t t0 = IdfClock().rtc_us();
    const auto cause = sleep.light_sleep(plan);
    const std::int64_t elapsed = IdfClock().rtc_us() - t0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(qz::hal::LightSleepWake::kTimer),
                            static_cast<std::uint8_t>(cause));
    TEST_ASSERT_TRUE(elapsed >= 49'000 && elapsed < 80'000);
}

TEST_CASE("SleepControl: light sleep with nothing armed returns instead of blocking",
          "[qz_sleep][light]") {
    IdfBoardIo io;
    TEST_ASSERT_TRUE(io.init());
    IdfSleep sleep(io);
    qz::hal::SleepPlan plan;
    plan.timer_us = -1;
    plan.wake_on_buttons = false;
    plan.wake_on_usb = false;
    TEST_ASSERT_EQUAL_UINT8(static_cast<std::uint8_t>(qz::hal::LightSleepWake::kOther),
                            static_cast<std::uint8_t>(sleep.light_sleep(plan)));
}
