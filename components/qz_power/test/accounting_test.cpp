// Daily awake-time accounting: per-wake awake time and cause summed per local day
// (ARCHITECTURE.md section 11, "Power estimate").
#include "power_test_support.hpp"
#include "qz/model/types.hpp"
#include "qz/power/power.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace qz::power {
namespace {

using model::WakeCause;
using testsupport::Noise;

constexpr time::DayNumber kDay = 20'000; // 2024-10-04

constexpr std::size_t slot(WakeCause cause) {
    return static_cast<std::size_t>(cause);
}

TEST(AwakeAccountingTest, SumsAwakeTimeAndWakesPerCause) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    policy.record_wake(WakeCause::kTimer, 80, kDay);
    policy.record_wake(WakeCause::kTimer, 70, kDay);
    policy.record_wake(WakeCause::kButton, 900, kDay);
    policy.record_wake(WakeCause::kUsb, 0, kDay);
    EXPECT_EQ(state.stats_day, kDay);
    EXPECT_EQ(state.awake_ms_today, 1050U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kTimer)], 2);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kTimer)], 150U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kButton)], 1);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kButton)], 900U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kUsb)], 1); // a zero-length wake still counts
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kUsb)], 0U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kAccel)], 0);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kAccel)], 0U);
}

TEST(AwakeAccountingTest, ResetsEverythingWhenTheLocalDayChanges) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    policy.record_wake(WakeCause::kTimer, 80, kDay);
    policy.record_wake(WakeCause::kTimer, 70, kDay);
    policy.record_wake(WakeCause::kButton, 900, kDay);

    policy.record_wake(WakeCause::kAccel, 25, kDay + 1); // first wake of the next local day
    EXPECT_EQ(state.stats_day, kDay + 1);
    EXPECT_EQ(state.awake_ms_today, 25U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kTimer)], 0);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kTimer)], 0U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kButton)], 0);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kAccel)], 1);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kAccel)], 25U);

    policy.record_wake(WakeCause::kAccel, 5, kDay + 1); // the same day accumulates again
    EXPECT_EQ(state.awake_ms_today, 30U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kAccel)], 2);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kAccel)], 30U);
}

TEST(AwakeAccountingTest, ADayThatMovesBackwardsAlsoResets) {
    // A time-zone change or a clock correction can move the local day backwards.
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    policy.record_wake(WakeCause::kTimer, 80, kDay + 1);
    policy.record_wake(WakeCause::kTimer, 70, kDay);
    EXPECT_EQ(state.stats_day, kDay);
    EXPECT_EQ(state.awake_ms_today, 70U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kTimer)], 1);
}

TEST(AwakeAccountingTest, EveryCauseHasItsOwnSlot) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    for (std::size_t cause = 0; cause < model::kWakeCauseCount; ++cause) {
        policy.record_wake(
            static_cast<WakeCause>(cause), static_cast<std::uint32_t>(cause + 1), kDay);
    }
    for (std::size_t cause = 0; cause < model::kWakeCauseCount; ++cause) {
        EXPECT_EQ(state.wakes_today[cause], 1) << cause;
        EXPECT_EQ(state.awake_ms_by_cause[cause], cause + 1) << cause;
    }
    EXPECT_EQ(state.awake_ms_today, 36U); // 1 + 2 + ... + 8
}

/// A raw cause byte that is not a WakeCause is counted as kUnknown.
void expect_counted_as_unknown(std::uint8_t raw) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    policy.record_wake(std::bit_cast<WakeCause>(raw), 40, kDay);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kUnknown)], 1) << static_cast<int>(raw);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kUnknown)], 40U) << static_cast<int>(raw);
    EXPECT_EQ(state.awake_ms_today, 40U) << static_cast<int>(raw);
}

TEST(AwakeAccountingTest, CountsAnOutOfRangeCauseAsUnknown) {
    expect_counted_as_unknown(8); // the first value past the enum
    expect_counted_as_unknown(9);
    expect_counted_as_unknown(200);
    expect_counted_as_unknown(255);
}

TEST(AwakeAccountingTest, SaturatesInsteadOfWrapping) {
    constexpr std::uint32_t kMaxMs = std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint16_t kMaxWakes = std::numeric_limits<std::uint16_t>::max();
    PowerState state;
    state.stats_day = kDay;
    state.awake_ms_today = kMaxMs - 5U;
    state.awake_ms_by_cause[slot(WakeCause::kTimer)] = kMaxMs - 5U;
    state.wakes_today[slot(WakeCause::kTimer)] = kMaxWakes - 1U;
    PowerPolicy policy(state, Thresholds{});

    policy.record_wake(WakeCause::kTimer, 3, kDay); // still exact below the limits
    EXPECT_EQ(state.awake_ms_today, kMaxMs - 2U);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kTimer)], kMaxMs - 2U);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kTimer)], kMaxWakes);

    policy.record_wake(WakeCause::kTimer, 100, kDay); // now clamped
    EXPECT_EQ(state.awake_ms_today, kMaxMs);
    EXPECT_EQ(state.awake_ms_by_cause[slot(WakeCause::kTimer)], kMaxMs);
    EXPECT_EQ(state.wakes_today[slot(WakeCause::kTimer)], kMaxWakes);

    policy.record_wake(WakeCause::kTimer, kMaxMs, kDay); // a huge single wake
    EXPECT_EQ(state.awake_ms_today, kMaxMs);
}

/// What the accounting should hold, kept the obvious way: plain arrays reset by hand.
struct DayModel {
    time::DayNumber day = kDay;
    std::uint32_t total_ms = 0;
    std::array<std::uint32_t, model::kWakeCauseCount> ms{};
    std::array<std::uint16_t, model::kWakeCauseCount> wakes{};

    void next_day() {
        ++day;
        total_ms = 0;
        ms.fill(0);
        wakes.fill(0);
    }
    void add(std::size_t cause, std::uint32_t awake_ms) {
        total_ms += awake_ms;
        ms[cause] += awake_ms;
        ++wakes[cause];
    }
    [[nodiscard]] bool matches(const PowerState& state) const {
        return state.stats_day == day && state.awake_ms_today == total_ms &&
               state.awake_ms_by_cause == ms && state.wakes_today == wakes;
    }
};

TEST(AwakeAccountingTest, MatchesAnIndependentModelOverManyDaysAndWakes) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    Noise noise(7);
    DayModel expected;
    for (int i = 0; i < 2000; ++i) {
        if (noise.next(9) > 6) { // about one wake in six starts a new local day
            expected.next_day();
        }
        const std::int32_t cause_roll = noise.next(3) + 3;     // 0..6
        const std::int32_t awake_roll = noise.next(500) + 500; // 0..1000 ms
        const auto cause = static_cast<std::size_t>(cause_roll);
        const auto awake_ms = static_cast<std::uint32_t>(awake_roll);
        policy.record_wake(static_cast<WakeCause>(cause), awake_ms, expected.day);
        expected.add(cause, awake_ms);
        ASSERT_TRUE(expected.matches(state)) << "wake " << i;
    }
}

} // namespace
} // namespace qz::power
