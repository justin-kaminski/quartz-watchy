// FakeSleepSystem: sleep-plan recording, virtual-time light sleep, deterministic hal::System data.
#include "qz/hal/system.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <type_traits>

namespace qz::testkit {
namespace {

constexpr std::int64_t kSecond = 1'000'000;

/// What a test reads when no plan was recorded: a timer no test ever uses.
constexpr std::int64_t kNoPlanMarker = -987'654'321;

hal::SleepPlan plan_or_marker(const std::optional<hal::SleepPlan>& plan) {
    hal::SleepPlan marker;
    marker.timer_us = kNoPlanMarker;
    return plan.value_or(marker);
}

hal::SleepPlan timer_plan(std::int64_t timer_us) {
    hal::SleepPlan plan;
    plan.timer_us = timer_us;
    return plan;
}

// --- initial state ------------------------------------------------------------------------------

TEST(FakeSleepSystem, StartsLikeAPowerOnBoot) {
    VirtualClock clock;
    clock.advance_us(123);
    const FakeSleepSystem sleep(clock);
    EXPECT_EQ(sleep.reset_reason(), hal::ResetReason::kPowerOn);
    const hal::WakeSources sources = sleep.wake_sources();
    EXPECT_FALSE(sources.timer);
    EXPECT_FALSE(sources.ext0);
    EXPECT_FALSE(sources.ext1);
    EXPECT_EQ(sources.ext1_pins, 0U);
    EXPECT_EQ(sleep.boot_rtc_us(), 123) << "RTC at construction";
    EXPECT_FALSE(sleep.last_plan().has_value());
    EXPECT_FALSE(sleep.last_light_sleep_plan().has_value());
    EXPECT_EQ(sleep.deep_sleep_count(), 0U);
    EXPECT_EQ(sleep.light_sleep_count(), 0U);
    EXPECT_EQ(sleep.restart_count(), 0U);
}

TEST(FakeSleepSystem, ReportsFixedIdentityAndHealthyDefaults) {
    VirtualClock clock;
    const FakeSleepSystem sleep(clock);
    EXPECT_EQ(sleep.firmware().version, "0.0.0-host");
    EXPECT_EQ(sleep.firmware().git_hash, "host");
    EXPECT_FALSE(sleep.firmware().idf_version.empty());
    EXPECT_NE(sleep.chip_id(), 0U);
    EXPECT_LT(sleep.chip_id(), std::uint64_t{1} << 48U) << "a base MAC has 48 bits";
    EXPECT_TRUE(sleep.slow_clock().external_crystal);
    EXPECT_EQ(sleep.slow_clock().measured_hz, 32768U);
    EXPECT_EQ(sleep.heap().free_bytes, 300000U);
    EXPECT_EQ(sleep.heap().min_free_bytes, 290000U);

    const FakeSleepSystem other(clock);
    EXPECT_EQ(other.chip_id(), sleep.chip_id());
}

TEST(FakeSleepSystem, SlowClockAndHeapCanBeScripted) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    sleep.set_slow_clock({.external_crystal = false, .measured_hz = 0});
    EXPECT_FALSE(sleep.slow_clock().external_crystal);
    EXPECT_EQ(sleep.slow_clock().measured_hz, 0U);
    sleep.set_slow_clock({.external_crystal = true, .measured_hz = 32'770});
    EXPECT_EQ(sleep.slow_clock().measured_hz, 32'770U);

    sleep.set_heap({.free_bytes = 1000, .min_free_bytes = 400});
    EXPECT_EQ(sleep.heap().free_bytes, 1000U);
    EXPECT_EQ(sleep.heap().min_free_bytes, 400U);
}

// --- deep sleep ---------------------------------------------------------------------------------

TEST(FakeSleepSystemDeepSleep, RecordsTheWholePlanAndCountsCalls) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    hal::SleepControl& control = sleep;

    hal::SleepPlan plan;
    plan.timer_us = 59 * kSecond;
    plan.wake_on_buttons = false;
    plan.wake_on_accel = true;
    plan.wake_on_usb = false;
    plan.wake_on_epd_idle = true;
    control.deep_sleep(plan);

    const hal::SleepPlan recorded = plan_or_marker(sleep.last_plan());
    EXPECT_EQ(recorded.timer_us, 59 * kSecond);
    EXPECT_FALSE(recorded.wake_on_buttons);
    EXPECT_TRUE(recorded.wake_on_accel);
    EXPECT_FALSE(recorded.wake_on_usb);
    EXPECT_TRUE(recorded.wake_on_epd_idle);
    EXPECT_EQ(sleep.deep_sleep_count(), 1U);

    control.deep_sleep(timer_plan(5 * kSecond));
    EXPECT_EQ(plan_or_marker(sleep.last_plan()).timer_us, 5 * kSecond) << "the latest plan wins";
    EXPECT_EQ(sleep.deep_sleep_count(), 2U);
}

TEST(FakeSleepSystemDeepSleep, ReturnsWithoutMovingTimeOrChangingTheBootState) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    clock.advance_us(1000);
    sleep.set_wake(hal::ResetReason::kSoftware, {});
    sleep.deep_sleep(timer_plan(60 * kSecond));
    EXPECT_EQ(clock.rtc_us(), 1000);
    EXPECT_EQ(sleep.reset_reason(), hal::ResetReason::kSoftware);
    EXPECT_EQ(sleep.boot_rtc_us(), 1000);
    EXPECT_FALSE(sleep.last_light_sleep_plan().has_value());
}

TEST(FakeSleepSystemDeepSleep, AcceptsAPlanWithAnySingleWakeSource) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    hal::SleepPlan none;
    none.timer_us = -1;
    none.wake_on_buttons = false;
    none.wake_on_accel = false;
    none.wake_on_usb = false;

    hal::SleepPlan only_timer = none;
    only_timer.timer_us = 0;
    hal::SleepPlan only_buttons = none;
    only_buttons.wake_on_buttons = true;
    hal::SleepPlan only_accel = none;
    only_accel.wake_on_accel = true;
    hal::SleepPlan only_usb = none;
    only_usb.wake_on_usb = true;
    for (const hal::SleepPlan& plan : {only_timer, only_buttons, only_accel, only_usb}) {
        sleep.deep_sleep(plan);
    }
    EXPECT_EQ(sleep.deep_sleep_count(), 4U);
}

TEST(FakeSleepSystemDeathTest, DeepSleepWithNothingToWakeItAborts) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    hal::SleepPlan dead;
    dead.timer_us = -1;
    dead.wake_on_buttons = false;
    dead.wake_on_accel = false;
    dead.wake_on_usb = false;
    EXPECT_DEATH(sleep.deep_sleep(dead), "QZ_ASSERT");

    // The EPD BUSY line is a light-sleep source only: it cannot end a deep sleep.
    dead.wake_on_epd_idle = true;
    EXPECT_DEATH(sleep.deep_sleep(dead), "QZ_ASSERT");
}

// --- light sleep --------------------------------------------------------------------------------

TEST(FakeSleepSystemLightSleep, AdvancesVirtualTimeByTheTimerAndReportsTimer) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    hal::SleepControl& control = sleep;

    EXPECT_EQ(control.light_sleep(timer_plan(2 * kSecond)), hal::LightSleepWake::kTimer);
    EXPECT_EQ(clock.rtc_us(), 2 * kSecond);
    EXPECT_EQ(clock.elapsed_us(), 2 * kSecond);
    EXPECT_EQ(sleep.light_sleep_count(), 1U);

    EXPECT_EQ(control.light_sleep(timer_plan(0)), hal::LightSleepWake::kTimer);
    EXPECT_EQ(clock.rtc_us(), 2 * kSecond) << "a zero timer returns at once";
    EXPECT_EQ(sleep.light_sleep_count(), 2U);
}

TEST(FakeSleepSystemLightSleep, TimerIsInRawRtcTimeSoCrystalErrorShiftsTrueTime) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(50'000);
    FakeSleepSystem sleep(clock);
    EXPECT_EQ(sleep.light_sleep(timer_plan(60 * kSecond)), hal::LightSleepWake::kTimer);
    EXPECT_EQ(clock.rtc_us(), 60 * kSecond);
    EXPECT_EQ(clock.elapsed_us(), 59'997'001);
}

TEST(FakeSleepSystemLightSleep, RecordsItsPlanSeparatelyFromDeepSleep) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    sleep.deep_sleep(timer_plan(10 * kSecond));

    hal::SleepPlan plan = timer_plan(3 * kSecond);
    plan.wake_on_epd_idle = true;
    plan.wake_on_accel = true;
    sleep.light_sleep(plan);

    const hal::SleepPlan light = plan_or_marker(sleep.last_light_sleep_plan());
    EXPECT_EQ(light.timer_us, 3 * kSecond);
    EXPECT_TRUE(light.wake_on_epd_idle);
    EXPECT_TRUE(light.wake_on_accel);
    EXPECT_EQ(plan_or_marker(sleep.last_plan()).timer_us, 10 * kSecond)
        << "light sleep must not overwrite it";
    EXPECT_EQ(sleep.deep_sleep_count(), 1U);
    EXPECT_EQ(sleep.light_sleep_count(), 1U);
}

TEST(FakeSleepSystemDeathTest, LightSleepWithoutATimerWouldNeverReturn) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    EXPECT_DEATH(static_cast<void>(sleep.light_sleep(timer_plan(-1))), "QZ_ASSERT.*timer_us");
}

// --- wake bookkeeping ---------------------------------------------------------------------------

TEST(FakeSleepSystemWake, SetWakeReportsReasonAndSourcesAndLatchesBootTime) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    const hal::System& system = sleep;

    clock.advance_us(60 * kSecond);
    hal::WakeSources sources;
    sources.ext1 = true;
    sources.ext1_pins = (std::uint64_t{1} << 7U) | (std::uint64_t{1} << 14U);
    sleep.set_wake(hal::ResetReason::kDeepSleep, sources);

    EXPECT_EQ(system.reset_reason(), hal::ResetReason::kDeepSleep);
    EXPECT_FALSE(system.wake_sources().timer);
    EXPECT_FALSE(system.wake_sources().ext0);
    EXPECT_TRUE(system.wake_sources().ext1);
    EXPECT_EQ(system.wake_sources().ext1_pins,
              (std::uint64_t{1} << 7U) | (std::uint64_t{1} << 14U));
    EXPECT_EQ(system.boot_rtc_us(), 60 * kSecond);

    clock.advance_us(5 * kSecond); // the app runs on: the latched boot time does not follow
    EXPECT_EQ(system.boot_rtc_us(), 60 * kSecond);
}

TEST(FakeSleepSystemWake, HarnessModelsBootLatencyByAdvancingBeforeSetWake) {
    // Deep sleep for 60 s, then the "boot" takes 350 ms before the app's first instruction.
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    const std::int64_t scheduled_wake_rtc_us = clock.rtc_us() + (60 * kSecond);
    sleep.deep_sleep(timer_plan(60 * kSecond));

    clock.advance_rtc_us(plan_or_marker(sleep.last_plan()).timer_us);
    clock.advance_us(350'000);
    hal::WakeSources timer;
    timer.timer = true;
    sleep.set_wake(hal::ResetReason::kDeepSleep, timer);

    EXPECT_EQ(sleep.boot_rtc_us() - scheduled_wake_rtc_us, 350'000) << "the measured wake latency";
    EXPECT_TRUE(sleep.wake_sources().timer);
}

TEST(FakeSleepSystemWake, RestartIsOnlyCounted) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    sleep.set_wake(hal::ResetReason::kDeepSleep, {});
    sleep.restart();
    sleep.restart();
    EXPECT_EQ(sleep.restart_count(), 2U);
    EXPECT_EQ(sleep.reset_reason(), hal::ResetReason::kDeepSleep) << "the harness does the reboot";
    EXPECT_EQ(clock.rtc_us(), 0);
}

// --- random numbers -----------------------------------------------------------------------------

TEST(FakeSleepSystemRandom, FollowsTheDocumentedXorshiftSequence) {
    VirtualClock clock;
    FakeSleepSystem sleep(clock);
    hal::System& system = sleep;
    // Marsaglia xorshift32 (13, 17, 5) from the seed 2463534242, reference computed in Python.
    EXPECT_EQ(system.random_u32(), 0x2B1F4D63U);
    EXPECT_EQ(system.random_u32(), 0x94DACB7AU);
    EXPECT_EQ(system.random_u32(), 0x7B0859A0U);
    EXPECT_EQ(system.random_u32(), 0x77B0567EU);
    EXPECT_EQ(system.random_u32(), 0xD28AB0E1U);
}

TEST(FakeSleepSystemRandom, IsReproduciblePerInstanceAndNeverZero) {
    VirtualClock clock;
    FakeSleepSystem a(clock);
    FakeSleepSystem b(clock);
    std::set<std::uint32_t> distinct;
    for (int i = 0; i < 20'000; ++i) {
        const std::uint32_t value = a.random_u32();
        EXPECT_EQ(value, b.random_u32());
        EXPECT_NE(value, 0U);
        distinct.insert(value);
    }
    EXPECT_EQ(distinct.size(), 20'000U)
        << "no repeats within a tiny fraction of the 2^32 - 1 period";
}

TEST(FakeSleepSystem, ClockCannotBeATemporary) {
    static_assert(std::is_constructible_v<FakeSleepSystem, VirtualClock&>);
    static_assert(!std::is_constructible_v<FakeSleepSystem, VirtualClock>, "would dangle");
    SUCCEED();
}

} // namespace
} // namespace qz::testkit
