// PowerPolicy: filter, hysteresis, USB rule, console override, sampling schedule, status and
// per-level decisions (ARCHITECTURE.md section 11).
#include "power_test_support.hpp"
#include "qz/model/types.hpp"
#include "qz/power/power.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace qz::power {
namespace {

using model::PowerLevel;
using testsupport::level_char;
using testsupport::Noise;
using testsupport::state_at;

constexpr std::int64_t kSecondUs = 1'000'000;
constexpr std::int64_t kSamplePeriodUs = 600 * kSecondUs;

static_assert(std::is_trivially_copyable_v<PowerState>, "PowerState lives in RTC memory");
static_assert(std::is_trivially_copyable_v<PolicyDecision>);
static_assert(std::is_trivially_copyable_v<Thresholds>);

// ---- filter -----------------------------------------------------------------------------------

TEST(FilterTest, FirstSampleSeedsTheFilterAndMarksTheStateValid) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(state.valid, 0);
    EXPECT_EQ(policy.on_sample(3815, false, 7 * kSecondUs), PowerLevel::kNormal);
    EXPECT_EQ(state.filtered_mv, 3815);
    EXPECT_EQ(state.valid, 1);
    EXPECT_EQ(state.last_sample_rtc_us, 7 * kSecondUs);
}

TEST(FilterTest, MovesAQuarterOfTheWayAndRoundsTheStepUp) {
    PowerState state = state_at(PowerLevel::kNormal, 4000);
    PowerPolicy policy(state, Thresholds{});
    policy.on_sample(3600, false, 0); // gap 400 -> 100
    EXPECT_EQ(state.filtered_mv, 3900);
    policy.on_sample(3600, false, 0); // gap 300 -> 75
    EXPECT_EQ(state.filtered_mv, 3825);
    policy.on_sample(3600, false, 0); // gap 225 -> 56.25 rounds up to 57
    EXPECT_EQ(state.filtered_mv, 3768);
    policy.on_sample(3800, false, 0); // upwards: gap 32 -> 8
    EXPECT_EQ(state.filtered_mv, 3776);
}

/// Feeds `target` repeatedly to a filter sitting at `start`: it must move toward the target without
/// ever overshooting and land on it exactly.
void expect_exact_convergence(std::uint16_t start, std::uint16_t target) {
    PowerState state = state_at(PowerLevel::kNormal, start);
    PowerPolicy policy(state, Thresholds{});
    const std::uint16_t low = std::min(start, target);
    const std::uint16_t high = std::max(start, target);
    std::uint16_t previous = start;
    bool well_behaved = true;
    for (int i = 0; i < 60; ++i) {
        policy.on_sample(target, false, 0);
        const bool toward_target =
            start > target ? state.filtered_mv <= previous : state.filtered_mv >= previous;
        well_behaved =
            well_behaved && toward_target && state.filtered_mv >= low && state.filtered_mv <= high;
        previous = state.filtered_mv;
    }
    EXPECT_TRUE(well_behaved) << "start " << start;
    EXPECT_EQ(state.filtered_mv, target) << "start " << start;
}

TEST(FilterTest, ConvergesExactlyToAConstantInputWithoutOvershoot) {
    expect_exact_convergence(4100, 3333); // from above
    expect_exact_convergence(2500, 3333); // from below
}

TEST(FilterTest, OneGlitchMovesTheFilterByAQuarterOfItsSize) {
    PowerState state = state_at(PowerLevel::kNormal, 3800);
    PowerPolicy policy(state, Thresholds{});
    policy.on_sample(4200, false, 0);
    EXPECT_EQ(state.filtered_mv, 3900);
    policy.on_sample(3800, false, 0);
    EXPECT_EQ(state.filtered_mv, 3875);
}

/// A Saver-level state sampled at 3450 mV at t = 5 s must not react to `mv` at t = 9000 s.
void expect_reading_ignored(PowerPolicy& policy, const PowerState& state, std::uint16_t mv) {
    EXPECT_EQ(policy.on_sample(mv, false, 9000 * kSecondUs), PowerLevel::kSaver) << mv;
    EXPECT_EQ(state.filtered_mv, 3450) << mv;
    EXPECT_EQ(state.last_sample_rtc_us, 5 * kSecondUs) << mv;
    EXPECT_TRUE(policy.sample_due(9000 * kSecondUs))
        << "a failed read must not satisfy the schedule";
}

TEST(FilterTest, IgnoresReadingsBelowThePlausibilityFloor) {
    PowerState state = state_at(PowerLevel::kSaver, 3450);
    state.last_sample_rtc_us = 5 * kSecondUs;
    PowerPolicy policy(state, Thresholds{});
    constexpr std::array<std::uint16_t, 3> kBad = {0, 1, 1999};
    for (const std::uint16_t mv : kBad) {
        expect_reading_ignored(policy, state, mv);
    }
    // 2000 mV is the lowest accepted reading: 3450 - ceil(1450 / 4) = 3087 -> Critical.
    EXPECT_EQ(policy.on_sample(2000, false, 9000 * kSecondUs), PowerLevel::kCritical);
    EXPECT_EQ(state.filtered_mv, 3087);
    EXPECT_EQ(state.last_sample_rtc_us, 9000 * kSecondUs);
}

TEST(FilterTest, AFailedFirstReadingLeavesTheStateUnsampled) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(policy.on_sample(0, false, 3 * kSecondUs), PowerLevel::kNormal);
    EXPECT_EQ(state.valid, 0);
    EXPECT_EQ(state.filtered_mv, 0);
    EXPECT_TRUE(policy.sample_due(3 * kSecondUs));
    EXPECT_FALSE(policy.status(false, false).valid);
}

// ---- hysteresis tables ------------------------------------------------------------------------

// Probe voltages: one below, at and one above every threshold, plus the extremes.
constexpr std::array<std::uint16_t, 14> kProbeMv = {
    3300, 3399, 3400, 3401, 3499, 3500, 3501, 3599, 3600, 3601, 3699, 3700, 3701, 4200};

struct TransitionRow {
    PowerLevel from;
    std::string_view expected; ///< level after the sample, one letter per kProbeMv entry
};

// Defaults: Low 3600/3700, Saver 3500/3600, Critical 3400/3600 (ARCHITECTURE.md section 11).
constexpr std::array<TransitionRow, 4> kOnBattery = {{
    {.from = PowerLevel::kNormal, .expected = "CCCSSSLLLNNNNN"},
    {.from = PowerLevel::kLow, .expected = "CCCSSSLLLLLNNN"},
    {.from = PowerLevel::kSaver, .expected = "CCCSSSSSLLLNNN"},
    {.from = PowerLevel::kCritical, .expected = "CCCCCCCCLLLNNN"},
}};

// USB present: Critical is left at once and cannot be entered; nothing else changes.
constexpr std::array<TransitionRow, 4> kOnUsb = {{
    {.from = PowerLevel::kNormal, .expected = "SSSSSSLLLNNNNN"},
    {.from = PowerLevel::kLow, .expected = "SSSSSSLLLLLNNN"},
    {.from = PowerLevel::kSaver, .expected = "SSSSSSSSLLLNNN"},
    {.from = PowerLevel::kCritical, .expected = "SSSSSSSSLLLNNN"},
}};

void expect_row(const TransitionRow& row, bool usb_present) {
    ASSERT_EQ(row.expected.size(), kProbeMv.size());
    for (std::size_t i = 0; i < kProbeMv.size(); ++i) {
        // The filter already sits at the probe voltage, so the EWMA step is the identity and the
        // table exercises the level logic alone.
        PowerState state = state_at(row.from, kProbeMv[i]);
        PowerPolicy policy(state, Thresholds{});
        const PowerLevel returned = policy.on_sample(kProbeMv[i], usb_present, kSecondUs);
        EXPECT_EQ(level_char(returned), row.expected[i])
            << "from " << level_char(row.from) << " at " << kProbeMv[i] << " mV, usb "
            << usb_present;
        EXPECT_EQ(state.level, returned);
        EXPECT_EQ(policy.level(), returned);
    }
}

TEST(HysteresisTest, TransitionTableOnBattery) {
    for (const TransitionRow& row : kOnBattery) {
        expect_row(row, false);
    }
}

TEST(HysteresisTest, TransitionTableOnUsb) {
    for (const TransitionRow& row : kOnUsb) {
        expect_row(row, true);
    }
}

TEST(HysteresisTest, CustomThresholdsAreHonoured) {
    Thresholds t;
    t.low_enter_mv = 3800;
    t.low_exit_mv = 3900;
    t.saver_enter_mv = 3700;
    t.saver_exit_mv = 3800;
    t.critical_enter_mv = 3600;
    t.critical_exit_mv = 3800;
    struct Case {
        PowerLevel from;
        std::uint16_t mv;
        PowerLevel expected;
    };
    constexpr std::array<Case, 6> kCases = {{
        {.from = PowerLevel::kNormal, .mv = 3750, .expected = PowerLevel::kLow},
        {.from = PowerLevel::kNormal, .mv = 3650, .expected = PowerLevel::kSaver},
        {.from = PowerLevel::kNormal, .mv = 3550, .expected = PowerLevel::kCritical},
        {.from = PowerLevel::kLow, .mv = 3850, .expected = PowerLevel::kLow},
        {.from = PowerLevel::kLow, .mv = 3900, .expected = PowerLevel::kNormal},
        {.from = PowerLevel::kCritical, .mv = 3799, .expected = PowerLevel::kCritical},
    }};
    for (const Case& c : kCases) {
        PowerState state = state_at(c.from, c.mv);
        PowerPolicy policy(state, t);
        EXPECT_EQ(policy.on_sample(c.mv, false, kSecondUs), c.expected)
            << level_char(c.from) << " at " << c.mv;
    }
}

TEST(HysteresisTest, DefaultBandsAreAtLeastFiveTimesTheNoiseAmplitude) {
    const Thresholds t;
    EXPECT_GE(t.low_exit_mv - t.low_enter_mv, 100);
    EXPECT_GE(t.saver_exit_mv - t.saver_enter_mv, 100);
    EXPECT_GE(t.critical_exit_mv - t.critical_enter_mv, 100);
}

// ---- no flapping on +-20 mV noise -------------------------------------------------------------

struct NoiseCase {
    const char* name;
    PowerLevel start;
    std::uint16_t center_mv; ///< the threshold under test; the noise is centred on it
    std::uint16_t start_mv;  ///< filter start value, on the far side of the threshold
    PowerLevel end;
};

constexpr NoiseCase kNoiseCases[] = {
    {.name = "low enter",
     .start = PowerLevel::kNormal,
     .center_mv = 3600,
     .start_mv = 3660,
     .end = PowerLevel::kLow},
    {.name = "low exit",
     .start = PowerLevel::kLow,
     .center_mv = 3700,
     .start_mv = 3640,
     .end = PowerLevel::kNormal},
    {.name = "saver enter",
     .start = PowerLevel::kLow,
     .center_mv = 3500,
     .start_mv = 3560,
     .end = PowerLevel::kSaver},
    {.name = "saver exit",
     .start = PowerLevel::kSaver,
     .center_mv = 3600,
     .start_mv = 3540,
     .end = PowerLevel::kLow},
    {.name = "critical enter",
     .start = PowerLevel::kSaver,
     .center_mv = 3400,
     .start_mv = 3460,
     .end = PowerLevel::kCritical},
    {.name = "critical exit",
     .start = PowerLevel::kCritical,
     .center_mv = 3600,
     .start_mv = 3540,
     .end = PowerLevel::kLow},
};

struct NoiseRun {
    int changes = 0;
    PowerLevel end = PowerLevel::kNormal;
};

NoiseRun run_noise(const NoiseCase& c, std::uint32_t seed) {
    PowerState state = state_at(c.start, c.start_mv);
    PowerPolicy policy(state, Thresholds{});
    Noise noise(seed);
    NoiseRun run;
    run.end = c.start;
    for (std::int64_t i = 0; i < 300; ++i) {
        const auto sample = static_cast<std::uint16_t>(c.center_mv + noise.next(20));
        const PowerLevel now = policy.on_sample(sample, false, i * kSamplePeriodUs);
        if (now != run.end) {
            ++run.changes;
            run.end = now;
        }
    }
    return run;
}

TEST(HysteresisTest, NoFlappingOnPlusMinus20mVNoiseAroundEveryThreshold) {
    for (const NoiseCase& c : kNoiseCases) {
        for (std::uint32_t seed = 1; seed <= 20; ++seed) {
            const NoiseRun run = run_noise(c, seed);
            EXPECT_EQ(run.changes, 1) << c.name << ", seed " << seed;
            EXPECT_EQ(level_char(run.end), level_char(c.end)) << c.name << ", seed " << seed;
        }
    }
}

// ---- a full discharge / recharge through the real filter --------------------------------------

/// Does the level change `from -> to` coincide with the filtered voltage crossing the threshold
/// on this very sample (before = filtered value one sample earlier)?
bool crossing_is_right(PowerLevel from, PowerLevel to, std::uint16_t before, std::uint16_t after) {
    if (from == PowerLevel::kNormal && to == PowerLevel::kLow) {
        return before > 3600 && after <= 3600;
    }
    if (from == PowerLevel::kLow && to == PowerLevel::kSaver) {
        return before > 3500 && after <= 3500;
    }
    if (from == PowerLevel::kSaver && to == PowerLevel::kCritical) {
        return before > 3400 && after <= 3400;
    }
    if (from == PowerLevel::kCritical && to == PowerLevel::kLow) {
        return before < 3600 && after >= 3600;
    }
    if (from == PowerLevel::kLow && to == PowerLevel::kNormal) {
        return before < 3700 && after >= 3700;
    }
    return false;
}

TEST(HysteresisTest, SlowDischargeAndRechargeVisitEachLevelOnceInOrder) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    std::string seen;
    PowerLevel last = PowerLevel::kNormal;
    std::uint16_t last_filtered = 0;
    bool crossings_ok = true;
    std::int64_t now_us = 0;
    const auto feed = [&](int mv) {
        const PowerLevel level = policy.on_sample(static_cast<std::uint16_t>(mv), false, now_us);
        now_us += kSamplePeriodUs;
        if (seen.empty()) {
            seen.push_back(level_char(level));
        } else if (level != last) {
            seen.push_back(level_char(level));
            crossings_ok =
                crossings_ok && crossing_is_right(last, level, last_filtered, state.filtered_mv);
        }
        last = level;
        last_filtered = state.filtered_mv;
    };
    for (int mv = 3900; mv >= 3300; mv -= 10) {
        feed(mv);
    }
    for (int mv = 3300; mv <= 3900; mv += 10) {
        feed(mv);
    }
    for (int i = 0; i < 20; ++i) {
        feed(3900); // let the filter settle
    }
    // Down: Normal -> Low -> Saver -> Critical. Up: Critical -> Low (Saver and Critical share the
    // exit voltage, so Saver is not visited) -> Normal.
    EXPECT_EQ(seen, "NLSCLN");
    EXPECT_TRUE(crossings_ok);
    EXPECT_EQ(policy.level(), PowerLevel::kNormal);
}

// ---- USB --------------------------------------------------------------------------------------

TEST(UsbTest, CriticalExitsAtOnceAndIsNotReEnteredWhileUsbIsPresent) {
    PowerState state = state_at(PowerLevel::kCritical, 3300);
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(policy.on_sample(3300, true, 1 * kSamplePeriodUs), PowerLevel::kSaver);
    EXPECT_EQ(policy.on_sample(3300, true, 2 * kSamplePeriodUs), PowerLevel::kSaver);
    // Unplugged with the cell still empty: Critical again, at once.
    EXPECT_EQ(policy.on_sample(3300, false, 3 * kSamplePeriodUs), PowerLevel::kCritical);
}

// ---- console override -------------------------------------------------------------------------

TEST(FakeTest, OverridesTheEvaluatedAndReportedVoltageButNotTheFilter) {
    PowerState state = state_at(PowerLevel::kNormal, 4000);
    state.fake_mv = 3450;
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(policy.on_sample(3800, false, 0), PowerLevel::kSaver); // evaluated at 3450
    EXPECT_EQ(state.filtered_mv, 3950); // the real filter tracks the real reading: 4000 - 50
    const model::BatteryStatus status = policy.status(false, false);
    EXPECT_TRUE(status.faked);
    EXPECT_EQ(status.mv, 3450);
    EXPECT_EQ(status.level, PowerLevel::kSaver);
    EXPECT_EQ(status.percent, 5); // curve 4 %, shown in 5 % steps
}

TEST(FakeTest, ClearingTheOverrideReturnsToTheRealReading) {
    PowerState state = state_at(PowerLevel::kNormal, 4000);
    state.fake_mv = 3450;
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(policy.on_sample(4000, false, 0), PowerLevel::kSaver);
    state.fake_mv = 0;
    EXPECT_EQ(policy.on_sample(4000, false, kSamplePeriodUs), PowerLevel::kNormal);
    const model::BatteryStatus status = policy.status(false, false);
    EXPECT_FALSE(status.faked);
    EXPECT_EQ(status.mv, 4000);
}

TEST(FakeTest, CanFakeCriticalAndStillExitsOnUsb) {
    PowerState state = state_at(PowerLevel::kNormal, 4000);
    state.fake_mv = 3300;
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(policy.on_sample(4000, false, 0), PowerLevel::kCritical);
    EXPECT_TRUE(policy.decision().charge_me_screen);
    EXPECT_EQ(policy.on_sample(4000, true, kSamplePeriodUs), PowerLevel::kSaver);
}

// ---- sampling schedule ------------------------------------------------------------------------

TEST(SampleDueTest, DueUntilTheFirstSampleThenOnceThePeriodHasPassed) {
    PowerState state;
    PowerPolicy policy(state, Thresholds{});
    EXPECT_TRUE(policy.sample_due(0));
    EXPECT_TRUE(policy.sample_due(1000 * kSecondUs));
    const std::int64_t t0 = 1000 * kSecondUs;
    policy.on_sample(3800, false, t0);
    EXPECT_FALSE(policy.sample_due(t0));
    EXPECT_FALSE(policy.sample_due(t0 + kSamplePeriodUs - 1));
    EXPECT_TRUE(policy.sample_due(t0 + kSamplePeriodUs));
    EXPECT_TRUE(policy.sample_due(t0 + (10 * kSamplePeriodUs)));
}

TEST(SampleDueTest, DueWhenTheClockWentBackwards) {
    PowerState state = state_at(PowerLevel::kNormal, 3800);
    state.last_sample_rtc_us = 5000 * kSecondUs;
    const PowerPolicy policy(state, Thresholds{});
    EXPECT_TRUE(policy.sample_due(4999 * kSecondUs));
    EXPECT_TRUE(policy.sample_due(0));
}

TEST(SampleDueTest, HonoursTheConfiguredPeriod) {
    Thresholds every_minute;
    every_minute.sample_period_s = 60;
    Thresholds always;
    always.sample_period_s = 0;
    PowerState state = state_at(PowerLevel::kNormal, 3800);
    state.last_sample_rtc_us = 100 * kSecondUs;
    const PowerPolicy minute(state, every_minute);
    const PowerPolicy none(state, always);
    EXPECT_FALSE(minute.sample_due((100 + 59) * kSecondUs));
    EXPECT_TRUE(minute.sample_due((100 + 60) * kSecondUs));
    EXPECT_TRUE(none.sample_due(100 * kSecondUs));
}

// ---- status -----------------------------------------------------------------------------------

TEST(StatusTest, FreshStateIsInvalidAndEmpty) {
    PowerState state;
    const PowerPolicy policy(state, Thresholds{});
    const model::BatteryStatus status = policy.status(false, false);
    EXPECT_FALSE(status.valid);
    EXPECT_FALSE(status.faked);
    EXPECT_EQ(status.mv, 0);
    EXPECT_EQ(status.percent, 0);
    EXPECT_EQ(status.level, PowerLevel::kNormal);
}

TEST(StatusTest, ReportsFilteredVoltageLevelAndUsbFlags) {
    PowerState state = state_at(PowerLevel::kSaver, 3450);
    const PowerPolicy policy(state, Thresholds{});
    const model::BatteryStatus on_usb = policy.status(true, true);
    EXPECT_EQ(on_usb.mv, 3450);
    EXPECT_EQ(on_usb.level, PowerLevel::kSaver);
    EXPECT_TRUE(on_usb.valid);
    EXPECT_FALSE(on_usb.faked);
    EXPECT_TRUE(on_usb.usb_present);
    EXPECT_TRUE(on_usb.charging);
    const model::BatteryStatus full = policy.status(true, false);
    EXPECT_TRUE(full.usb_present);
    EXPECT_FALSE(full.charging);
}

TEST(StatusTest, ChargingNeverReportsWithoutUsb) {
    PowerState state = state_at(PowerLevel::kNormal, 3800);
    const PowerPolicy policy(state, Thresholds{});
    const model::BatteryStatus status = policy.status(false, true);
    EXPECT_FALSE(status.usb_present);
    EXPECT_FALSE(status.charging);
}

TEST(StatusTest, PercentRoundsToFiveUpTo3700AndToTenAbove) {
    struct Case {
        std::uint16_t mv;
        unsigned percent;
    };
    // Curve value in brackets. Above 3700 the nearest 10 never drops below the 15 shown at 3700.
    constexpr std::array<Case, 14> kCases = {{
        {.mv = 3300, .percent = 0},   // [0]
        {.mv = 3400, .percent = 5},   // [3]
        {.mv = 3500, .percent = 5},   // [5]
        {.mv = 3600, .percent = 10},  // [8]
        {.mv = 3700, .percent = 15},  // [13]
        {.mv = 3701, .percent = 15},  // [13] nearest 10 would be 10: held at the boundary value
        {.mv = 3707, .percent = 15},  // [14]
        {.mv = 3708, .percent = 20},  // [15] nearest 10
        {.mv = 3730, .percent = 20},  // [20]
        {.mv = 3900, .percent = 60},  // [64]
        {.mv = 3950, .percent = 70},  // [70]
        {.mv = 4150, .percent = 90},  // [94]
        {.mv = 4195, .percent = 100}, // [99]
        {.mv = 4200, .percent = 100}, // [100]
    }};
    for (const Case& c : kCases) {
        PowerState state = state_at(PowerLevel::kNormal, c.mv);
        const PowerPolicy policy(state, Thresholds{});
        EXPECT_EQ(policy.status(false, false).percent, c.percent) << c.mv << " mV";
    }
}

TEST(StatusTest, PercentIsMonotonicInFivesAndNeverAbove100) {
    PowerState state;
    state.valid = 1;
    const PowerPolicy policy(state, Thresholds{});
    unsigned previous = 0;
    for (std::uint32_t mv = 0; mv <= 5000; ++mv) {
        state.filtered_mv = static_cast<std::uint16_t>(mv);
        const unsigned percent = policy.status(false, false).percent;
        ASSERT_GE(percent, previous) << mv << " mV";
        ASSERT_EQ(percent % 5U, 0U) << mv << " mV";
        ASSERT_LE(percent, 100U) << mv << " mV";
        previous = percent;
    }
    EXPECT_EQ(previous, 100U);
}

// ---- decision ---------------------------------------------------------------------------------

struct ExpectedDecision {
    PowerLevel level;
    bool radio;
    bool tap_wake;
    bool vibration;
    unsigned display_period_min;
    unsigned full_refresh_every;
    bool charge_me;
};

constexpr std::array<ExpectedDecision, 4> kDecisions = {{
    {.level = PowerLevel::kNormal,
     .radio = true,
     .tap_wake = true,
     .vibration = true,
     .display_period_min = 1,
     .full_refresh_every = 30,
     .charge_me = false},
    {.level = PowerLevel::kLow,
     .radio = false,
     .tap_wake = false,
     .vibration = false,
     .display_period_min = 1,
     .full_refresh_every = 30,
     .charge_me = false},
    {.level = PowerLevel::kSaver,
     .radio = false,
     .tap_wake = false,
     .vibration = false,
     .display_period_min = 5,
     .full_refresh_every = 48, // 4 h of 5-minute updates
     .charge_me = false},
    {.level = PowerLevel::kCritical,
     .radio = false,
     .tap_wake = false,
     .vibration = false,
     .display_period_min = 0, // no timer wake
     .full_refresh_every = 1,
     .charge_me = true},
}};

void expect_decision(const PolicyDecision& d, const ExpectedDecision& e) {
    const char level = level_char(e.level);
    EXPECT_EQ(d.radio_allowed, e.radio) << level;
    EXPECT_EQ(d.tap_wake_allowed, e.tap_wake) << level;
    EXPECT_EQ(d.vibration_allowed, e.vibration) << level;
    EXPECT_EQ(d.display_period_min, e.display_period_min) << level;
    EXPECT_EQ(d.full_refresh_every, e.full_refresh_every) << level;
    EXPECT_EQ(d.charge_me_screen, e.charge_me) << level;
}

TEST(DecisionTest, TableForEveryLevel) {
    for (const ExpectedDecision& e : kDecisions) {
        PowerState state = state_at(e.level, 3800);
        const PowerPolicy policy(state, Thresholds{});
        expect_decision(policy.decision(), e);
    }
}

TEST(DecisionTest, FollowsTheLevelThatOnSampleProduces) {
    constexpr std::array<std::uint16_t, 4> kMv = {3800, 3550, 3450, 3350};
    for (std::size_t i = 0; i < kMv.size(); ++i) {
        PowerState state;
        PowerPolicy policy(state, Thresholds{});
        EXPECT_EQ(policy.on_sample(kMv[i], false, 0), kDecisions[i].level) << kMv[i];
        expect_decision(policy.decision(), kDecisions[i]);
    }
}

// ---- persistent state and construction --------------------------------------------------------

TEST(PolicyStateTest, ConstructionLeavesTheStateUntouched) {
    PowerState state = state_at(PowerLevel::kSaver, 3450);
    state.fake_mv = 3300;
    state.awake_ms_today = 5;
    const PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(state.level, PowerLevel::kSaver);
    EXPECT_EQ(state.filtered_mv, 3450);
    EXPECT_EQ(state.fake_mv, 3300);
    EXPECT_EQ(state.awake_ms_today, 5U);
    EXPECT_EQ(state.valid, 1);
}

/// A level byte that is not a PowerLevel (RTC corruption, a newer layout) must behave as Normal.
void expect_level_reads_as_normal(std::uint8_t raw) {
    PowerState state = state_at(std::bit_cast<PowerLevel>(raw), 3800);
    PowerPolicy policy(state, Thresholds{});
    EXPECT_EQ(policy.level(), PowerLevel::kNormal) << static_cast<int>(raw);
    EXPECT_TRUE(policy.decision().radio_allowed) << static_cast<int>(raw);
    EXPECT_EQ(policy.status(false, false).level, PowerLevel::kNormal) << static_cast<int>(raw);
    EXPECT_EQ(policy.on_sample(3800, false, 0), PowerLevel::kNormal) << static_cast<int>(raw);
    EXPECT_EQ(state.level, PowerLevel::kNormal) << "rewritten with a valid value";
}

TEST(PolicyStateTest, AnOutOfRangeLevelReadsAsNormal) {
    expect_level_reads_as_normal(4); // the first value past the enum
    expect_level_reads_as_normal(9);
    expect_level_reads_as_normal(255);
}

void construct_with(const Thresholds& thresholds) {
    PowerState state;
    const PowerPolicy policy(state, thresholds);
    (void)policy;
}

TEST(PowerPolicyDeathTest, RejectsThresholdsThatCannotWork) {
    Thresholds no_low_band;
    no_low_band.low_exit_mv = no_low_band.low_enter_mv;
    Thresholds no_saver_band;
    no_saver_band.saver_exit_mv = no_saver_band.saver_enter_mv;
    Thresholds no_critical_band;
    no_critical_band.critical_exit_mv = no_critical_band.critical_enter_mv;
    Thresholds saver_above_low;
    saver_above_low.saver_enter_mv = 3700;
    Thresholds critical_above_saver;
    critical_above_saver.critical_enter_mv = 3550;
    EXPECT_DEATH(construct_with(no_low_band), "");
    EXPECT_DEATH(construct_with(no_saver_band), "");
    EXPECT_DEATH(construct_with(no_critical_band), "");
    EXPECT_DEATH(construct_with(saver_above_low), "");
    EXPECT_DEATH(construct_with(critical_above_saver), "");
}

TEST(PowerPolicyTest, AcceptsTheNarrowestPossibleBands) {
    Thresholds narrow;
    narrow.low_exit_mv = static_cast<std::uint16_t>(narrow.low_enter_mv + 1);
    narrow.saver_exit_mv = static_cast<std::uint16_t>(narrow.saver_enter_mv + 1);
    narrow.critical_exit_mv = static_cast<std::uint16_t>(narrow.critical_enter_mv + 1);
    construct_with(narrow);
    construct_with(Thresholds{});
}

} // namespace
} // namespace qz::power
