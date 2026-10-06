// LiPo curve, robust mean and board-divider scaling (ARCHITECTURE.md section 11).
#include "power_test_support.hpp"
#include "qz/power/power.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace qz::power {
namespace {

constexpr std::uint16_t kMaxMv = std::numeric_limits<std::uint16_t>::max();

struct MvPercent {
    std::uint16_t mv;
    unsigned percent;
};

// ---- default curve ----------------------------------------------------------------------------

TEST(DefaultCurveTest, HasElevenPointsFrom4200To3300) {
    const std::span<const CurvePoint> curve = default_curve();
    ASSERT_EQ(curve.size(), 11U);
    EXPECT_EQ(curve.front().mv, 4200);
    EXPECT_EQ(curve.front().percent, 100);
    EXPECT_EQ(curve.back().mv, 3300);
    EXPECT_EQ(curve.back().percent, 0);
}

TEST(DefaultCurveTest, DescendsInMvAndNeverRisesInPercent) {
    const std::span<const CurvePoint> curve = default_curve();
    for (std::size_t i = 1; i < curve.size(); ++i) {
        EXPECT_GT(curve[i - 1].mv, curve[i].mv) << "point " << i;
        EXPECT_GE(curve[i - 1].percent, curve[i].percent) << "point " << i;
    }
}

TEST(PercentFromMvTest, IsExactAtEveryCurvePoint) {
    for (const CurvePoint& point : default_curve()) {
        EXPECT_EQ(percent_from_mv(point.mv, default_curve()), point.percent) << point.mv << " mV";
    }
}

TEST(PercentFromMvTest, IsMonotonicAndBoundedForEveryInput) {
    std::uint8_t previous = percent_from_mv(0, default_curve());
    EXPECT_EQ(previous, 0);
    for (std::uint32_t mv = 1; mv <= kMaxMv; ++mv) {
        const std::uint8_t now = percent_from_mv(static_cast<std::uint16_t>(mv), default_curve());
        ASSERT_GE(now, previous) << mv << " mV";
        ASSERT_LE(now, 100) << mv << " mV";
        previous = now;
    }
    EXPECT_EQ(previous, 100);
}

TEST(PercentFromMvTest, ClampsBelowAndAboveTheCurve) {
    EXPECT_EQ(percent_from_mv(0, default_curve()), 0);
    EXPECT_EQ(percent_from_mv(3299, default_curve()), 0);
    EXPECT_EQ(percent_from_mv(4201, default_curve()), 100);
    EXPECT_EQ(percent_from_mv(kMaxMv, default_curve()), 100);
}

TEST(PercentFromMvTest, InterpolatesTheDefaultCurveToHandComputedValues) {
    // Rounded to nearest, halves up. The last segment is 3690..3300 mV for 10 %: 39 mV per percent.
    constexpr MvPercent kCases[] = {
        {.mv = 3350, .percent = 1},  // 50/39 = 1.28
        {.mv = 3400, .percent = 3},  // 2.56
        {.mv = 3500, .percent = 5},  // 5.13
        {.mv = 3600, .percent = 8},  // 7.69
        {.mv = 3700, .percent = 13}, // 3690..3730 is 10 % over 40 mV: 12.5, the half rounds up
        {.mv = 3900, .percent = 64}, // 3870..3950: 63.75
        {.mv = 4150, .percent = 94}, // 4110..4200: 94.44
        {.mv = 4195, .percent = 99}, // 99.44
    };
    for (const MvPercent& c : kCases) {
        EXPECT_EQ(percent_from_mv(c.mv, default_curve()), c.percent) << c.mv << " mV";
    }
}

TEST(PercentFromMvTest, InterpolatesACustomCurveWithAFlatSegment) {
    constexpr std::array<CurvePoint, 4> kShelf = {{
        {.mv = 4000, .percent = 100},
        {.mv = 3900, .percent = 60},
        {.mv = 3700, .percent = 60},
        {.mv = 3600, .percent = 0},
    }};
    constexpr MvPercent kCases[] = {
        {.mv = 5000, .percent = 100},
        {.mv = 4000, .percent = 100},
        {.mv = 3950, .percent = 80}, // halfway down the steep first segment
        {.mv = 3900, .percent = 60},
        {.mv = 3800, .percent = 60}, // flat segment
        {.mv = 3700, .percent = 60},
        {.mv = 3699, .percent = 59}, // 59.4
        {.mv = 3650, .percent = 30},
        {.mv = 3601, .percent = 1}, // 0.6
        {.mv = 3600, .percent = 0},
        {.mv = 3599, .percent = 0},
        {.mv = 0, .percent = 0},
    };
    for (const MvPercent& c : kCases) {
        EXPECT_EQ(percent_from_mv(c.mv, kShelf), c.percent) << c.mv << " mV";
    }
}

TEST(PercentFromMvTest, ClampsPercentsAbove100) {
    constexpr std::array<CurvePoint, 2> kCurve = {
        {{.mv = 4200, .percent = 120}, {.mv = 3300, .percent = 0}}};
    EXPECT_EQ(percent_from_mv(4300, kCurve), 100); // above the top point (120 %)
    EXPECT_EQ(percent_from_mv(4200, kCurve), 100);
    EXPECT_EQ(percent_from_mv(4100, kCurve), 100); // 107.2 inside the segment
    EXPECT_EQ(percent_from_mv(4000, kCurve), 93);  // 93.3
    EXPECT_EQ(percent_from_mv(3750, kCurve), 60);
}

TEST(PercentFromMvTest, WorksWithTheMinimumOfTwoPoints) {
    constexpr std::array<CurvePoint, 2> kCurve = {
        {{.mv = 4200, .percent = 100}, {.mv = 3000, .percent = 0}}};
    EXPECT_EQ(percent_from_mv(4200, kCurve), 100);
    EXPECT_EQ(percent_from_mv(3600, kCurve), 50);
    EXPECT_EQ(percent_from_mv(3007, kCurve), 1); // 0.58
    EXPECT_EQ(percent_from_mv(3000, kCurve), 0);
    EXPECT_EQ(percent_from_mv(2999, kCurve), 0);
}

TEST(PercentFromMvDeathTest, RejectsCurvesThatCannotBeInterpolated) {
    constexpr std::array<CurvePoint, 1> kSingle = {{{.mv = 4000, .percent = 100}}};
    constexpr std::array<CurvePoint, 2> kAscending = {
        {{.mv = 3300, .percent = 0}, {.mv = 4200, .percent = 100}}};
    constexpr std::array<CurvePoint, 2> kZeroWidth = {
        {{.mv = 4000, .percent = 100}, {.mv = 4000, .percent = 0}}};
    constexpr std::array<CurvePoint, 2> kRising = {
        {{.mv = 4000, .percent = 10}, {.mv = 3900, .percent = 50}}};
    EXPECT_DEATH((void)percent_from_mv(3700, std::span<const CurvePoint>{}), "");
    EXPECT_DEATH((void)percent_from_mv(3700, kSingle), "");
    EXPECT_DEATH((void)percent_from_mv(3700, kAscending), "");
    EXPECT_DEATH((void)percent_from_mv(3700, kZeroWidth), "");
    EXPECT_DEATH((void)percent_from_mv(3700, kRising), "");
}

// ---- robust mean ------------------------------------------------------------------------------

/// Independent oracle: sort, drop the first and last, floating-point mean rounded half up.
std::uint16_t sorted_oracle(std::vector<std::uint16_t> values) {
    if (values.empty()) {
        return 0;
    }
    std::ranges::sort(values);
    std::size_t first = 0;
    std::size_t last = values.size();
    if (values.size() >= 3) {
        first = 1;
        last = values.size() - 1;
    }
    double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        sum += values[i];
    }
    return static_cast<std::uint16_t>(std::floor((sum / static_cast<double>(last - first)) + 0.5));
}

TEST(RobustMeanTest, EmptyIsZeroAndSmallSetsUseThePlainMean) {
    EXPECT_EQ(robust_mean_mv({}), 0);
    constexpr std::array<std::uint16_t, 1> kOne = {3700};
    constexpr std::array<std::uint16_t, 2> kTwo = {3700, 3701};
    constexpr std::array<std::uint16_t, 2> kTwoEven = {3700, 3702};
    EXPECT_EQ(robust_mean_mv(kOne), 3700);
    EXPECT_EQ(robust_mean_mv(kTwo), 3701); // 3700.5 rounds up
    EXPECT_EQ(robust_mean_mv(kTwoEven), 3701);
}

TEST(RobustMeanTest, ThreeSamplesGiveTheMiddleOne) {
    constexpr std::array<std::uint16_t, 3> kSpread = {4200, 3700, 3000};
    constexpr std::array<std::uint16_t, 3> kEqual = {3700, 3700, 3700};
    constexpr std::array<std::uint16_t, 3> kTwoLow = {5, 5, 9};
    EXPECT_EQ(robust_mean_mv(kSpread), 3700);
    EXPECT_EQ(robust_mean_mv(kEqual), 3700);
    EXPECT_EQ(robust_mean_mv(kTwoLow), 5);
}

TEST(RobustMeanTest, DropsOnlyOneMinimumAndOneMaximum) {
    // A second outlier on a side still counts: {3000, 3700, 3700, 4200} loses one of each extreme.
    constexpr std::array<std::uint16_t, 6> kTwoEach = {3000, 3000, 3700, 3700, 4200, 4200};
    constexpr std::array<std::uint16_t, 4> kOneEach = {3000, 3700, 3700, 4200};
    EXPECT_EQ(robust_mean_mv(kTwoEach), 3650); // (3000 + 3700 + 3700 + 4200) / 4
    EXPECT_EQ(robust_mean_mv(kOneEach), 3700);
}

TEST(RobustMeanTest, RejectsOutliersInASixteenSampleBurst) {
    // 14 readings around 3700 mV plus one 3100 and one 4000: the plain mean is 3682, the robust
    // mean ignores both. 14 readings sum to 51807 -> 3700.5 -> 3701.
    constexpr std::array<std::uint16_t, 16> kBurst = {3702,
                                                      3698,
                                                      3700,
                                                      3704,
                                                      3696,
                                                      3701,
                                                      3699,
                                                      3703,
                                                      3697,
                                                      3705,
                                                      3695,
                                                      3706,
                                                      3694,
                                                      3707,
                                                      3100,
                                                      4000};
    EXPECT_EQ(robust_mean_mv(kBurst), 3701);
}

TEST(RobustMeanTest, ConstantInputIsUnchanged) {
    const std::vector<std::uint16_t> values(16, 3815);
    EXPECT_EQ(robust_mean_mv(values), 3815);
}

TEST(RobustMeanTest, DoesNotDependOnTheOrderOfTheSamples) {
    std::array<std::uint16_t, 6> values = {3000, 3690, 3700, 3710, 3720, 4200};
    const std::uint16_t expected = robust_mean_mv(values);
    EXPECT_EQ(expected, 3705); // (3690 + 3700 + 3710 + 3720) / 4
    do {
        ASSERT_EQ(robust_mean_mv(values), expected);
    } while (std::ranges::next_permutation(values).found);
}

TEST(RobustMeanTest, MatchesASortingOracleOnRandomSets) {
    testsupport::Noise noise(12345);
    for (int round = 0; round < 500; ++round) {
        std::vector<std::uint16_t> values;
        const std::int32_t size_roll = noise.next(20) + 20; // 0..40
        const auto size = static_cast<std::size_t>(size_roll);
        for (std::size_t i = 0; i < size; ++i) {
            // Mostly 3600..3800 mV, with the occasional wild reading in either direction.
            const std::int32_t roll = noise.next(15);
            std::int32_t mv = 3700 + noise.next(100);
            if (roll > 12) {
                mv = 100;
            } else if (roll < -12) {
                mv = 65000;
            }
            values.push_back(static_cast<std::uint16_t>(mv));
        }
        ASSERT_EQ(robust_mean_mv(values), sorted_oracle(values)) << "round " << round;
    }
}

TEST(RobustMeanTest, DoesNotOverflowOnVeryLargeSets) {
    // 70'000 full-scale samples sum to more than 2^32.
    const std::vector<std::uint16_t> values(70'000, kMaxMv);
    EXPECT_EQ(robust_mean_mv(values), kMaxMv);
}

// ---- board divider ----------------------------------------------------------------------------

TEST(BatteryMvFromPinTest, MatchesTheBoardDividerAtHandComputedPoints) {
    // R8 100k / R9 360k: battery = pin x 460 / 360 [R1 s4], rounded to nearest, halves up.
    EXPECT_EQ(battery_mv_from_pin(0, 460, 360), 0);
    EXPECT_EQ(battery_mv_from_pin(2583, 460, 360), 3301); // 3300.5
    EXPECT_EQ(battery_mv_from_pin(2739, 460, 360), 3500); // 3499.83
    EXPECT_EQ(battery_mv_from_pin(2900, 460, 360), 3706); // 3705.56: end of the calibrated range
    EXPECT_EQ(battery_mv_from_pin(3287, 460, 360), 4200); // 4200.06
}

TEST(BatteryMvFromPinTest, ScalesAndRoundsAnyRatio) {
    EXPECT_EQ(battery_mv_from_pin(1234, 1, 1), 1234);
    EXPECT_EQ(battery_mv_from_pin(1000, 2, 1), 2000);
    EXPECT_EQ(battery_mv_from_pin(1001, 1, 2), 501); // 500.5 rounds up
    EXPECT_EQ(battery_mv_from_pin(1000, 1, 3), 333);
    EXPECT_EQ(battery_mv_from_pin(2000, 1, 3), 667);
}

TEST(BatteryMvFromPinTest, SaturatesInsteadOfWrapping) {
    EXPECT_EQ(battery_mv_from_pin(kMaxMv, 460, 360), kMaxMv); // 83'733 clamps
    EXPECT_EQ(battery_mv_from_pin(kMaxMv, kMaxMv, 1), kMaxMv);
    EXPECT_EQ(battery_mv_from_pin(kMaxMv, kMaxMv, kMaxMv), kMaxMv);
}

TEST(BatteryMvFromPinTest, MatchesAFloatingPointOracleAndIsMonotonic) {
    std::uint16_t previous = 0;
    for (std::uint32_t pin = 0; pin <= kMaxMv; ++pin) {
        const std::uint16_t mv = battery_mv_from_pin(static_cast<std::uint16_t>(pin), 460, 360);
        const double exact = std::min(static_cast<double>(pin) * 460.0 / 360.0, 65535.0);
        ASSERT_EQ(mv, static_cast<std::uint16_t>(std::floor(exact + 0.5))) << pin << " mV pin";
        ASSERT_GE(mv, previous) << pin << " mV pin";
        previous = mv;
    }
}

TEST(BatteryMvFromPinDeathTest, RejectsAZeroDenominator) {
    EXPECT_DEATH((void)battery_mv_from_pin(2900, 460, 0), "");
}

} // namespace
} // namespace qz::power
