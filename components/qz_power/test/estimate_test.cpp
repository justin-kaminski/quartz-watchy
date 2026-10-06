// estimate_hours against hand-computed cases (ARCHITECTURE.md section 11, "Power estimate"):
//   hours = capacity_uAh / (sleep_floor_uA + awake_ms_per_day x active_mA / 86'400), floored.
#include "qz/power/power.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace qz::power {
namespace {

constexpr std::uint32_t kMaxU32 = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kMsPerDay = 86'400'000;

std::uint32_t hours(std::uint32_t capacity_mah,
                    std::uint32_t floor_ua,
                    std::uint32_t active_ma,
                    std::uint32_t awake_ms_per_day) {
    return estimate_hours({.capacity_mah = capacity_mah,
                           .sleep_floor_ua = floor_ua,
                           .active_ma = active_ma,
                           .awake_ms_per_day = awake_ms_per_day});
}

TEST(EstimateHoursTest, SleepFloorOnly) {
    EXPECT_EQ(hours(200, 50, 25, 0), 4000U);  // 200'000 uAh / 50 uA
    EXPECT_EQ(hours(100, 100, 25, 0), 1000U); // 100'000 uAh / 100 uA
    EXPECT_EQ(hours(170, 40, 25, 0), 4250U);  // 170'000 uAh / 40 uA
}

TEST(EstimateHoursTest, DefaultInputsAreTheHeaderDefaults) {
    EXPECT_EQ(estimate_hours(EstimateInputs{}), 3400U); // 170 mAh, 50 uA, no awake time
}

TEST(EstimateHoursTest, AddsTheAwakeTimeAveragedOverTheDay) {
    // 100 s awake per day at 25 mA = 2500 mA*s = 0.694 mAh/day = 28.9 uA on average;
    // 200'000 uAh / (50 + 28.935) uA = 2533.7 h, floored.
    EXPECT_EQ(hours(200, 50, 25, 100'000), 2533U);
    // ARCHITECTURE.md section 20, minute wakes: 1440 x 70 ms = 100'800 ms/day at 25 mA = 29.2 uA;
    // 170'000 uAh / (40 + 29.17) uA = 2457.8 h, floored (over 100 days).
    EXPECT_EQ(hours(170, 40, 25, 100'800), 2457U);
}

TEST(EstimateHoursTest, AwakeAllDayIsJustTheActiveCurrent) {
    EXPECT_EQ(hours(100, 0, 10, kMsPerDay), 10U);     // 100 mAh / 10 mA
    EXPECT_EQ(hours(300, 0, 20, kMsPerDay / 2), 30U); // half a day at 20 mA = 10 mA on average
}

TEST(EstimateHoursTest, AwakeTimeBeyondOneDayIsClampedToOneDay) {
    EXPECT_EQ(hours(100, 0, 10, 200'000'000), 10U);
    EXPECT_EQ(hours(100, 0, 10, kMaxU32), 10U);
}

TEST(EstimateHoursTest, FloorsInsteadOfRounding) {
    EXPECT_EQ(hours(1, 3, 0, 0), 333U); // 1000 uAh / 3 uA = 333.3
    EXPECT_EQ(hours(2, 3, 0, 0), 666U); // 666.7: never promise more than the numbers give
}

TEST(EstimateHoursTest, NoCapacityMeansNoHours) {
    EXPECT_EQ(hours(0, 50, 25, 1000), 0U);
    EXPECT_EQ(hours(0, 0, 0, 0), 0U);
}

TEST(EstimateHoursTest, NoDrainSaturates) {
    EXPECT_EQ(hours(170, 0, 25, 0), kMaxU32);
    EXPECT_EQ(hours(170, 0, 0, 5000), kMaxU32); // awake at 0 mA draws nothing
}

TEST(EstimateHoursTest, VeryLongLifeSaturatesInsteadOfWrapping) {
    EXPECT_EQ(hours(kMaxU32, 1, 0, 0), kMaxU32); // 4.3e9 mAh at 1 uA is 4.3e12 h
}

TEST(EstimateHoursTest, ExtremeInputsDoNotOverflow) {
    // Capacity, floor and active current all at 2^32 - 1, awake all day: the awake term dominates
    // and the estimate is just under one hour.
    EXPECT_EQ(hours(kMaxU32, kMaxU32, kMaxU32, kMaxU32), 0U);
}

TEST(EstimateHoursTest, MoreCapacityNeverShortensAndMoreAwakeTimeNeverLengthens) {
    std::uint32_t previous = 0;
    for (std::uint32_t capacity = 0; capacity <= 400; capacity += 10) {
        const std::uint32_t now = hours(capacity, 50, 25, 100'000);
        ASSERT_GE(now, previous) << capacity << " mAh";
        previous = now;
    }
    previous = kMaxU32;
    for (std::uint32_t awake = 0; awake <= kMsPerDay; awake += 1'000'000) {
        const std::uint32_t now = hours(170, 50, 25, awake);
        ASSERT_LE(now, previous) << awake << " ms";
        previous = now;
    }
}

} // namespace
} // namespace qz::power
