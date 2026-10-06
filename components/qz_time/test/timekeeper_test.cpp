// TimeKeeper (qz/time/timekeeper.hpp): anchor + drift mapping, drift estimation policy, validity,
// jump reporting. VirtualClock models the crystal, power loss and the libc clock.
#include "qz/testkit/fakes.hpp"
#include "qz/time/timekeeper.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace qz::time {
namespace {

using testkit::VirtualClock;

constexpr std::int64_t kHourUs = 3600LL * kUsPerSecond;
constexpr std::int64_t kDayUs = 24 * kHourUs;
constexpr std::int64_t kT0 = 1'780'000'000LL * kUsPerSecond; // mid 2026
constexpr std::int64_t kPpm = 1000;                          // ppb

static_assert(std::is_trivially_copyable_v<TimeKeeperState>);
static_assert(sizeof(TimeKeeperState) == 48 && alignof(TimeKeeperState) == 8);
static_assert(std::is_standard_layout_v<TimeKeeperState>);

__extension__ using Int128 = __int128; // test-side oracle only

std::int64_t iabs(std::int64_t v) {
    return v < 0 ? -v : v;
}

/// Exact floor(d * p / 1e9) reference on 128-bit integers (the production code avoids them).
std::int64_t reference_utc(const TimeKeeperState& s, std::int64_t rtc_us) {
    const Int128 d = static_cast<Int128>(rtc_us) - s.anchor_rtc_us;
    const Int128 num = d * (1'000'000'000LL + s.drift_ppb);
    Int128 q = num / 1'000'000'000;
    if ((num % 1'000'000'000) < 0) {
        --q;
    }
    return static_cast<std::int64_t>(s.anchor_utc_us + q);
}

struct Fixture {
    VirtualClock clock;
    TimeKeeperState state;
    TimeKeeper keeper{state, clock};

    /// True time moves on by `us`; the virtual world's UTC moves with it.
    void pass(std::int64_t us) { clock.advance_us(us); }
    SyncOutcome sync() { return keeper.apply_sntp(clock.true_utc_us(), clock.rtc_us()); }
    /// Manual set to the true time, which starts the world at kT0.
    void start(std::int32_t crystal_ppb) {
        clock.set_crystal_error_ppb(crystal_ppb);
        clock.set_true_utc_us(kT0);
        (void)keeper.set_utc(kT0, TimeSource::kManual); // jump report not needed for setup
    }
};

TEST(TimeKeeper, ColdBootIsInvalidAndSourceNone) {
    const Fixture f;
    EXPECT_FALSE(f.keeper.valid());
    EXPECT_EQ(f.keeper.source(), TimeSource::kNone);
    EXPECT_EQ(f.keeper.drift_ppb(), 0);
    EXPECT_EQ(f.state.last_sync_utc_us, 0);
    EXPECT_EQ(f.state.last_set_utc_us, 0);
}

TEST(TimeKeeper, SetUtcAnchorsMarksValidAndPushesLibcClock) {
    Fixture f;
    f.pass(5 * kUsPerSecond);
    const TimeJump jump = f.keeper.set_utc(kT0, TimeSource::kManual);
    EXPECT_FALSE(jump.was_valid);
    EXPECT_EQ(jump.old_utc_us, 0);
    EXPECT_EQ(jump.new_utc_us, kT0);
    EXPECT_TRUE(f.keeper.valid());
    EXPECT_EQ(f.keeper.source(), TimeSource::kManual);
    EXPECT_EQ(f.keeper.now_utc_us(), kT0);
    EXPECT_EQ(f.state.last_set_utc_us, kT0);
    EXPECT_EQ(f.clock.system_utc_us(), kT0);
    EXPECT_EQ(f.state.anchor_rtc_us, f.clock.rtc_us());

    f.pass((90 * kUsPerSecond) + 123);
    EXPECT_EQ(f.keeper.now_utc_us(), kT0 + (90 * kUsPerSecond) + 123);
    EXPECT_EQ(f.keeper.utc_at_rtc(f.clock.rtc_us()), f.keeper.now_utc_us());
}

TEST(TimeKeeper, SetUtcReportsTheJumpFromTheRunningEstimate) {
    Fixture f;
    f.start(0);
    f.pass(kHourUs);
    const TimeJump back = f.keeper.set_utc(kT0 - kDayUs, TimeSource::kConsole);
    EXPECT_TRUE(back.was_valid);
    EXPECT_EQ(back.old_utc_us, kT0 + kHourUs);
    EXPECT_EQ(back.new_utc_us, kT0 - kDayUs);
    EXPECT_EQ(f.keeper.source(), TimeSource::kConsole);
    EXPECT_EQ(f.keeper.now_utc_us(), kT0 - kDayUs);
    EXPECT_EQ(f.state.last_set_utc_us, kT0 - kDayUs);
    EXPECT_EQ(f.clock.system_utc_us(), kT0 - kDayUs);
}

TEST(TimeKeeper, SetUtcRejectsNonSetSourcesAsProgrammerError) {
    Fixture f;
    EXPECT_DEATH((void)f.keeper.set_utc(kT0, TimeSource::kSntp), "");
    EXPECT_DEATH((void)f.keeper.set_utc(kT0, TimeSource::kNone), "");
}

TEST(TimeKeeper, ConstructorRejectsNonsensePolicy) {
    VirtualClock clock;
    TimeKeeperState state;
    DriftPolicy bad_gain;
    bad_gain.gain_divisor = 0;
    EXPECT_DEATH((TimeKeeper{state, clock, bad_gain}), "");
    DriftPolicy bad_clamp;
    bad_clamp.max_drift_ppb = 600'000'000;
    EXPECT_DEATH((TimeKeeper{state, clock, bad_clamp}), "");
}

TEST(TimeKeeper, DriftCompensationMatchesTheFormula) {
    Fixture f;
    f.start(0);
    f.keeper.set_drift_ppb(-50'000); // as if restored from NVS, applied to elapsed RTC time
    f.state.anchor_rtc_us = 1'000'000;
    f.state.anchor_utc_us = kT0;
    const std::int64_t rtc = 1'000'000 + (10 * kDayUs) + 777;
    EXPECT_EQ(f.keeper.utc_at_rtc(rtc), reference_utc(f.state, rtc));
    // RTC slow by 50 ppm (drift -50 ppm): utc = d - ceil(d / 20000).
    constexpr std::int64_t kElapsed = (10 * kDayUs) + 777;
    EXPECT_EQ(f.keeper.utc_at_rtc(rtc), kT0 + kElapsed - ((kElapsed + 19'999) / 20'000));
    // Before the anchor the mapping continues linearly (floor semantics).
    EXPECT_EQ(f.keeper.utc_at_rtc(1'000'000 - 3), reference_utc(f.state, 1'000'000 - 3));
}

// ---- drift estimation ----

class DriftConvergence : public testing::TestWithParam<std::int32_t> {};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_P(DriftConvergence, WithinOnePpmAfterThreeSyncsAndKeepsTimeOverAMonth) {
    const std::int32_t error_ppb = GetParam();
    Fixture f;
    f.start(error_ppb);
    for (int i = 1; i <= 3; ++i) {
        f.pass(8 * kHourUs);
        const SyncOutcome out = f.sync();
        EXPECT_FALSE(out.clock_fault);
        EXPECT_EQ(out.drift_updated, i >= 2) << "sync " << i;
        EXPECT_EQ(f.keeper.source(), TimeSource::kSntp);
    }
    // RTC fast by e => true elapsed = rtc * (1 - e), so the correction is about -e.
    EXPECT_LE(iabs(f.keeper.drift_ppb() + error_ppb), kPpm) << "drift " << f.keeper.drift_ppb();

    f.pass(30 * kDayUs);
    EXPECT_LE(iabs(f.keeper.now_utc_us() - f.clock.true_utc_us()), 100'000)
        << "30 days of free running after convergence";
}

INSTANTIATE_TEST_SUITE_P(
    Crystals,
    DriftConvergence,
    testing::Values(-50'000, -37'123, -20'000, -1'500, -1, 0, 1, 2'500, 13'579, 31'415, 50'000));

TEST(TimeKeeperDrift, FirstSyncAfterAManualSetOnlyAnchors) {
    Fixture f;
    f.start(30'000);
    f.pass(12 * kHourUs);
    // The manual time was 3 s off; that error must not leak into the drift estimate.
    const SyncOutcome out =
        f.keeper.apply_sntp(f.clock.true_utc_us() + (3 * kUsPerSecond), f.clock.rtc_us());
    EXPECT_FALSE(out.drift_updated);
    EXPECT_FALSE(out.clock_fault);
    EXPECT_EQ(f.keeper.drift_ppb(), 0);
    EXPECT_TRUE(out.jump.was_valid);
    EXPECT_EQ(out.residual_us, f.clock.true_utc_us() + (3 * kUsPerSecond) - out.jump.old_utc_us);
    EXPECT_EQ(f.keeper.now_utc_us(), f.clock.true_utc_us() + (3 * kUsPerSecond));
    EXPECT_EQ(f.state.last_sync_utc_us, f.clock.true_utc_us() + (3 * kUsPerSecond));
    EXPECT_EQ(f.state.last_sync_rtc_us, f.clock.rtc_us());
}

TEST(TimeKeeperDrift, SyncWhileInvalidAnchorsWithoutEstimating) {
    Fixture f;
    f.clock.set_true_utc_us(kT0);
    f.pass(kHourUs);
    const SyncOutcome out = f.sync();
    EXPECT_FALSE(out.jump.was_valid);
    EXPECT_EQ(out.jump.old_utc_us, 0);
    EXPECT_EQ(out.jump.new_utc_us, kT0 + kHourUs);
    EXPECT_EQ(out.residual_us, 0);
    EXPECT_FALSE(out.drift_updated);
    EXPECT_FALSE(out.clock_fault);
    EXPECT_TRUE(f.keeper.valid());
    EXPECT_EQ(f.keeper.source(), TimeSource::kSntp);
    EXPECT_EQ(f.keeper.now_utc_us(), kT0 + kHourUs);
}

TEST(TimeKeeperDrift, SyncCloserThanTheMinimumIntervalDoesNotEstimateOrFault) {
    Fixture f;
    f.start(40'000);
    f.pass(8 * kHourUs);
    (void)f.sync();
    // 5 h later with a 90 s SNTP error (5000 ppm): too short to estimate, so no fault either.
    f.pass(5 * kHourUs);
    const SyncOutcome out =
        f.keeper.apply_sntp(f.clock.true_utc_us() + (90 * kUsPerSecond), f.clock.rtc_us());
    EXPECT_FALSE(out.drift_updated);
    EXPECT_FALSE(out.clock_fault);
    EXPECT_EQ(f.keeper.drift_ppb(), 0);
}

TEST(TimeKeeperDrift, IntervalBoundaryIsInclusive) {
    VirtualClock clock;
    TimeKeeperState state;
    TimeKeeper keeper{state, clock};
    clock.set_true_utc_us(kT0);
    clock.set_crystal_error_ppb(20'000);
    (void)keeper.apply_sntp(clock.true_utc_us(), clock.rtc_us());
    // Exactly 6 h of RTC time.
    clock.advance_rtc_us(6 * kHourUs);
    const SyncOutcome out = keeper.apply_sntp(clock.true_utc_us(), clock.rtc_us());
    EXPECT_TRUE(out.drift_updated);
    EXPECT_LE(iabs(keeper.drift_ppb() + 20'000), kPpm);
    // One microsecond short of 6 h does not estimate.
    VirtualClock clock2;
    TimeKeeperState state2;
    TimeKeeper keeper2{state2, clock2};
    clock2.set_true_utc_us(kT0);
    (void)keeper2.apply_sntp(kT0, clock2.rtc_us());
    clock2.advance_rtc_us((6 * kHourUs) - 1);
    EXPECT_FALSE(keeper2.apply_sntp(clock2.true_utc_us(), clock2.rtc_us()).drift_updated);
}

TEST(TimeKeeperDrift, ResidualOver500PpmIsAClockFaultThatKeepsDrift) {
    Fixture f;
    f.start(0);
    f.keeper.set_drift_ppb(12'345);
    f.pass(8 * kHourUs);
    (void)f.sync(); // anchors from SNTP
    EXPECT_EQ(f.keeper.drift_ppb(), 12'345);

    f.pass(8 * kHourUs);
    // 20 s over 8 h of RTC time = ~694 ppm (beyond the 500 ppm limit).
    const std::int64_t sntp = f.clock.true_utc_us() + (20 * kUsPerSecond);
    const SyncOutcome out = f.keeper.apply_sntp(sntp, f.clock.rtc_us());
    EXPECT_TRUE(out.clock_fault);
    EXPECT_FALSE(out.drift_updated);
    EXPECT_EQ(f.keeper.drift_ppb(), 12'345);
    EXPECT_EQ(f.keeper.source(), TimeSource::kSntp);
    EXPECT_EQ(f.keeper.now_utc_us(), sntp) << "anchored on the new time anyway";
    EXPECT_EQ(f.state.last_sync_utc_us, sntp);

    // The anchor moved, so the next window is eligible again and nothing is stuck.
    f.pass(8 * kHourUs);
    const SyncOutcome next =
        f.keeper.apply_sntp(f.clock.true_utc_us() + (20 * kUsPerSecond), f.clock.rtc_us());
    EXPECT_TRUE(next.drift_updated);
    EXPECT_FALSE(next.clock_fault);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TimeKeeperDrift, ResidualLimitBoundaryAndHugeResiduals) {
    for (const bool negative : {false, true}) {
        // The elapsed window is exactly 8 h of RTC time: 500 ppm = 14.4 s.
        const std::int64_t sign = negative ? -1 : 1;
        {
            VirtualClock clock;
            TimeKeeperState state;
            TimeKeeper keeper{state, clock};
            clock.set_true_utc_us(kT0);
            (void)keeper.apply_sntp(kT0, clock.rtc_us());
            clock.advance_us(8 * kHourUs);
            const SyncOutcome out =
                keeper.apply_sntp(clock.true_utc_us() + (sign * 14'400'000), clock.rtc_us());
            EXPECT_FALSE(out.clock_fault) << "exactly 500 ppm is accepted";
            EXPECT_TRUE(out.drift_updated);
            EXPECT_EQ(keeper.drift_ppb(),
                      sign * 500'000 > 200'000    ? 200'000
                      : sign * 500'000 < -200'000 ? -200'000
                                                  : 0)
                << "bootstrap applies the whole residual, then the 200 ppm clamp";
        }
        {
            VirtualClock clock;
            TimeKeeperState state;
            TimeKeeper keeper{state, clock};
            clock.set_true_utc_us(kT0);
            (void)keeper.apply_sntp(kT0, clock.rtc_us());
            clock.advance_us(8 * kHourUs);
            const SyncOutcome out =
                keeper.apply_sntp(clock.true_utc_us() + (sign * 14'401'000), clock.rtc_us());
            EXPECT_TRUE(out.clock_fault) << "500.03 ppm is a fault";
            EXPECT_FALSE(out.drift_updated);
            EXPECT_EQ(keeper.drift_ppb(), 0);
        }
        {
            // Absurd residuals (centuries) must neither overflow nor be mistaken for valid.
            VirtualClock clock;
            TimeKeeperState state;
            TimeKeeper keeper{state, clock};
            clock.set_true_utc_us(kT0);
            (void)keeper.apply_sntp(kT0, clock.rtc_us());
            clock.advance_us(8 * kHourUs);
            const std::int64_t absurd = sign * std::numeric_limits<std::int64_t>::max() / 4;
            const SyncOutcome out = keeper.apply_sntp(absurd, clock.rtc_us());
            EXPECT_TRUE(out.clock_fault);
            EXPECT_FALSE(out.drift_updated);
            EXPECT_EQ(keeper.drift_ppb(), 0);
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TimeKeeperDrift, ManualSetRestartsTheEstimationWindowAndKeepsDrift) {
    Fixture f;
    f.start(25'000);
    for (int i = 0; i < 3; ++i) {
        f.pass(8 * kHourUs);
        (void)f.sync();
    }
    const std::int32_t learned = f.keeper.drift_ppb();
    ASSERT_LE(iabs(learned + 25'000), kPpm);

    f.pass(kHourUs);
    (void)f.keeper.set_utc(f.clock.true_utc_us() + (7 * kUsPerSecond), TimeSource::kManual);
    EXPECT_EQ(f.keeper.drift_ppb(), learned) << "a manual set does not discard the drift";

    f.pass(10 * kHourUs); // long enough for an estimate, but the window restarted
    const SyncOutcome after_set = f.sync();
    EXPECT_FALSE(after_set.drift_updated);
    EXPECT_FALSE(after_set.clock_fault);
    EXPECT_EQ(f.keeper.drift_ppb(), learned) << "the 7 s manual error must not become drift";

    f.pass(10 * kHourUs);
    EXPECT_TRUE(f.sync().drift_updated);

    // The console source restarts the window the same way.
    f.pass(kHourUs);
    (void)f.keeper.set_utc(f.clock.true_utc_us(), TimeSource::kConsole);
    f.pass(10 * kHourUs);
    EXPECT_FALSE(f.sync().drift_updated);
}

TEST(TimeKeeperDrift, KnownDriftIsRefinedByHalfTheResidualPerSync) {
    Fixture f;
    f.start(50'000);
    f.keeper.set_drift_ppb(-10'000); // true value is about -50'000, so the residual is 40'000
    f.pass(8 * kHourUs);
    (void)f.sync();
    f.pass(8 * kHourUs);
    const SyncOutcome out = f.sync();
    ASSERT_TRUE(out.drift_updated);
    EXPECT_NEAR(f.keeper.drift_ppb(), -30'000, 50) << "gain 1/2 once an estimate exists";
    f.pass(8 * kHourUs);
    (void)f.sync();
    EXPECT_NEAR(f.keeper.drift_ppb(), -40'000, 50);
}

TEST(TimeKeeperDrift, PolicyClampAndGainAreHonoured) {
    VirtualClock clock;
    TimeKeeperState state;
    DriftPolicy policy;
    policy.max_drift_ppb = 30'000;
    policy.gain_divisor = 4;
    policy.min_interval_us = kHourUs;
    TimeKeeper keeper{state, clock, policy};
    clock.set_true_utc_us(kT0);
    clock.set_crystal_error_ppb(80'000);
    (void)keeper.apply_sntp(kT0, clock.rtc_us());
    clock.advance_us(2 * kHourUs);
    ASSERT_TRUE(keeper.apply_sntp(clock.true_utc_us(), clock.rtc_us()).drift_updated);
    EXPECT_EQ(keeper.drift_ppb(), -30'000) << "bootstrap residual -80 ppm clamped to the policy";
    keeper.set_drift_ppb(500'000);
    EXPECT_EQ(keeper.drift_ppb(), 30'000);
    keeper.set_drift_ppb(-500'000);
    EXPECT_EQ(keeper.drift_ppb(), -30'000);
}

// ---- mapping accuracy ----

class MappingRoundTrip : public testing::TestWithParam<std::int32_t> {};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_P(MappingRoundTrip, RtcAtUtcInvertsUtcAtRtcWithinOneMicrosecondOver30Days) {
    Fixture f;
    f.keeper.set_drift_ppb(GetParam());
    f.state.anchor_rtc_us = 123'456'789;
    f.state.anchor_utc_us = kT0 + 17;
    f.state.valid = 1;
    constexpr std::int64_t kSpan = 30 * kDayUs;
    std::uint64_t lcg = std::uint64_t{0x9E3779B97F4A7C15} ^ static_cast<std::uint64_t>(GetParam());
    for (int i = 0; i < 20'000; ++i) {
        lcg = (lcg * std::uint64_t{6364136223846793005}) + std::uint64_t{1442695040888963407};
        const auto offset = static_cast<std::int64_t>((lcg >> 11) % (kSpan + 1));
        const std::int64_t x = f.state.anchor_rtc_us + offset;
        const std::int64_t utc = f.keeper.utc_at_rtc(x);
        ASSERT_EQ(utc, reference_utc(f.state, x));
        const std::int64_t back = f.keeper.rtc_at_utc(utc);
        ASSERT_LE(iabs(back - x), 1) << "x=" << x << " drift=" << GetParam();
        // rtc_at_utc is the first RTC instant at which UTC reaches `utc` (exact inverse).
        ASSERT_GE(f.keeper.utc_at_rtc(back), utc);
        ASSERT_LT(f.keeper.utc_at_rtc(back - 1), utc);
        // Targets that are not on the image of utc_at_rtc (slope > 1 skips values).
        const std::int64_t target = utc + 1;
        ASSERT_GE(f.keeper.utc_at_rtc(f.keeper.rtc_at_utc(target)), target);
        ASSERT_LT(f.keeper.utc_at_rtc(f.keeper.rtc_at_utc(target) - 1), target);
    }
    // Edges: the anchor itself, the span end, and instants before the anchor.
    for (const std::int64_t offset : {std::int64_t{-9}, std::int64_t{0}, std::int64_t{1}, kSpan}) {
        const std::int64_t x = f.state.anchor_rtc_us + offset;
        EXPECT_LE(iabs(f.keeper.rtc_at_utc(f.keeper.utc_at_rtc(x)) - x), 1) << offset;
    }
}

INSTANTIATE_TEST_SUITE_P(
    Drifts,
    MappingRoundTrip,
    testing::Values(-200'000, -50'000, -1'000, -1, 0, 1, 7'777, 50'000, 199'999, 200'000));

TEST(TimeKeeperMapping, TenYearsAtMaximumDriftNeverOverflowsAndMatchesTheReference) {
    constexpr std::int64_t kTenYearsUs = 10LL * 366 * kDayUs;
    for (const std::int32_t drift : {-200'000, -1, 0, 1, 200'000}) {
        Fixture f;
        f.keeper.set_drift_ppb(drift);
        f.state.anchor_rtc_us = 0;
        f.state.anchor_utc_us = kT0;
        f.state.valid = 1;
        for (const std::int64_t rtc :
             {kTenYearsUs, kTenYearsUs - 1, kTenYearsUs / 3, std::int64_t{1}}) {
            const std::int64_t utc = f.keeper.utc_at_rtc(rtc);
            EXPECT_EQ(utc, reference_utc(f.state, rtc));
            EXPECT_LE(iabs(f.keeper.rtc_at_utc(utc) - rtc), 1);
        }
    }
}

TEST(TimeKeeperMapping, TenYearsOfSyncsStaysAccurate) {
    Fixture f;
    f.start(37'000);
    constexpr std::int64_t kSteps = (10LL * 366 * 24) / 12; // 12 h cadence for ten years
    for (std::int64_t i = 0; i < kSteps; ++i) {
        f.pass(12 * kHourUs);
        const SyncOutcome out = f.sync();
        ASSERT_FALSE(out.clock_fault) << i;
        if (i >= 2) {
            ASSERT_LE(iabs(out.residual_us), 1'000) << i; // learned drift keeps SNTP within 1 ms
        }
    }
    f.pass(90 * kDayUs); // then 90 days free running on the learned drift
    EXPECT_LE(iabs(f.keeper.now_utc_us() - f.clock.true_utc_us()), 100'000);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TimeKeeperMapping, ExtremeInputsSaturateInsteadOfOverflowing) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    for (const std::int32_t drift : {-200'000, 0, 200'000}) {
        Fixture f;
        f.keeper.set_drift_ppb(drift);
        f.state.anchor_rtc_us = 0;
        f.state.anchor_utc_us = 0;
        f.state.valid = 1;
        for (const std::int64_t x : {kMax, kMin, kMax - 1, kMin + 1, std::int64_t{0}}) {
            (void)f.keeper.utc_at_rtc(x);
            (void)f.keeper.rtc_at_utc(x);
        }
        EXPECT_GT(f.keeper.utc_at_rtc(kMax), f.keeper.utc_at_rtc(0));
        EXPECT_LT(f.keeper.utc_at_rtc(kMin), f.keeper.utc_at_rtc(0));
        EXPECT_GT(f.keeper.rtc_at_utc(kMax), f.keeper.rtc_at_utc(0));
        EXPECT_LT(f.keeper.rtc_at_utc(kMin), f.keeper.rtc_at_utc(0));
        if (drift == 0) {
            EXPECT_EQ(f.keeper.utc_at_rtc(kMax), kMax);
            EXPECT_EQ(f.keeper.utc_at_rtc(kMin), kMin);
            EXPECT_EQ(f.keeper.rtc_at_utc(kMax), kMax);
            EXPECT_EQ(f.keeper.rtc_at_utc(kMin), kMin);
        }
    }
    // Garbage drift from untrusted RTC memory is bounded by the mapping, never a divide by zero.
    Fixture g;
    g.state.drift_ppb = std::numeric_limits<std::int32_t>::min();
    g.state.valid = 1;
    EXPECT_LT(g.keeper.utc_at_rtc(kHourUs), kHourUs);
    EXPECT_GT(g.keeper.rtc_at_utc(kHourUs), kHourUs);
    g.state.drift_ppb = std::numeric_limits<std::int32_t>::max();
    EXPECT_GT(g.keeper.utc_at_rtc(kHourUs), kHourUs);
}

TEST(TimeKeeperMapping, WakeSchedulingLandsOnTheTargetUtcUnderCrystalError) {
    Fixture f;
    f.start(-42'000);
    for (int i = 0; i < 3; ++i) {
        f.pass(8 * kHourUs);
        (void)f.sync();
    }
    // Sleep until the next minute boundary 6 h out, as the wake planner will.
    const std::int64_t target =
        ((f.keeper.now_utc_us() / (60 * kUsPerSecond)) + 360) * 60 * kUsPerSecond;
    const std::int64_t rtc_target = f.keeper.rtc_at_utc(target);
    ASSERT_GT(rtc_target, f.clock.rtc_us());
    f.clock.advance_rtc_us(rtc_target - f.clock.rtc_us());
    EXPECT_LE(iabs(f.clock.true_utc_us() - target), 1'000);
    EXPECT_GE(f.keeper.now_utc_us(), target);
}

// ---- validity and state ----

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TimeKeeperValidity, PowerLossInvalidatesTimeAndResetKeepsOnlyTheDrift) {
    Fixture f;
    f.start(20'000);
    for (int i = 0; i < 3; ++i) {
        f.pass(8 * kHourUs);
        (void)f.sync();
    }
    const std::int32_t learned = f.keeper.drift_ppb();
    ASSERT_NE(learned, 0);
    ASSERT_TRUE(f.keeper.valid());

    f.clock.power_loss(); // RTC restarts at 0, behind the stale anchor
    EXPECT_FALSE(f.keeper.valid());
    f.pass(kHourUs);
    EXPECT_FALSE(f.keeper.valid());

    TimeKeeper::reset(f.state);
    EXPECT_FALSE(f.keeper.valid());
    EXPECT_EQ(f.keeper.source(), TimeSource::kNone);
    EXPECT_EQ(f.keeper.drift_ppb(), learned);
    EXPECT_EQ(f.state.last_sync_utc_us, 0);
    EXPECT_EQ(f.state.last_set_utc_us, 0);
    EXPECT_EQ(f.state.anchor_rtc_us, 0);
    EXPECT_EQ(f.state.anchor_utc_us, 0);

    // The next sync re-establishes time without trusting the old window.
    f.pass(kHourUs);
    const SyncOutcome out = f.sync();
    EXPECT_FALSE(out.jump.was_valid);
    EXPECT_FALSE(out.drift_updated);
    EXPECT_TRUE(f.keeper.valid());
}

TEST(TimeKeeperValidity, ResetClearsEveryFieldExceptDrift) {
    TimeKeeperState state;
    state.anchor_rtc_us = 1;
    state.anchor_utc_us = 2;
    state.last_sync_utc_us = 3;
    state.last_sync_rtc_us = 4;
    state.last_set_utc_us = 5;
    state.drift_ppb = -777;
    state.source = TimeSource::kSntp;
    state.valid = 1;
    state.clock_degraded = 1;
    state.reserved = 9;
    TimeKeeper::reset(state);
    EXPECT_EQ(state.drift_ppb, -777);
    EXPECT_EQ(state.anchor_rtc_us, 0);
    EXPECT_EQ(state.anchor_utc_us, 0);
    EXPECT_EQ(state.last_sync_utc_us, 0);
    EXPECT_EQ(state.last_sync_rtc_us, 0);
    EXPECT_EQ(state.last_set_utc_us, 0);
    EXPECT_EQ(state.source, TimeSource::kNone);
    EXPECT_EQ(state.valid, 0);
    EXPECT_EQ(state.clock_degraded, 0);
    EXPECT_EQ(state.reserved, 0);
}

TEST(TimeKeeperValidity, StateSurvivesAByteCopyLikeRtcMemory) {
    Fixture f;
    f.start(15'000);
    for (int i = 0; i < 3; ++i) {
        f.pass(8 * kHourUs);
        (void)f.sync();
    }
    // A fresh TimeKeeper over a copy of the persisted state (deep-sleep wake) agrees exactly.
    TimeKeeperState copy = f.state;
    const TimeKeeper revived{copy, f.clock};
    EXPECT_TRUE(revived.valid());
    EXPECT_EQ(revived.source(), TimeSource::kSntp);
    f.pass(kDayUs);
    EXPECT_EQ(revived.now_utc_us(), f.keeper.now_utc_us());
}

TEST(TimeKeeperValidity, SyncPushesTheCorrectedTimeToTheLibcClock) {
    Fixture f;
    f.start(0);
    f.pass(kHourUs);
    const std::int64_t sntp = f.clock.true_utc_us() + 1234;
    (void)f.keeper.apply_sntp(sntp, f.clock.rtc_us());
    EXPECT_EQ(f.clock.system_utc_us(), f.keeper.now_utc_us());
    EXPECT_EQ(f.clock.system_utc_us(), sntp);
}

TEST(TimeKeeperValidity, SntpTimeJumpIsReportedForServices) {
    Fixture f;
    f.start(0);
    f.pass(kHourUs);
    const std::int64_t sntp = f.clock.true_utc_us() - (2 * kUsPerSecond);
    const SyncOutcome out = f.keeper.apply_sntp(sntp, f.clock.rtc_us());
    EXPECT_TRUE(out.jump.was_valid);
    EXPECT_EQ(out.jump.old_utc_us, kT0 + kHourUs);
    EXPECT_EQ(out.jump.new_utc_us, sntp);
    EXPECT_EQ(out.residual_us, -2 * kUsPerSecond);
}

TEST(TimeKeeperValidity, ClockDegradedFlagIsStored) {
    Fixture f;
    EXPECT_EQ(f.state.clock_degraded, 0);
    f.keeper.set_clock_degraded(true);
    EXPECT_EQ(f.state.clock_degraded, 1);
    f.keeper.set_clock_degraded(false);
    EXPECT_EQ(f.state.clock_degraded, 0);
}

TEST(TimeKeeperValidity, SetDriftClampsToThePolicy) {
    Fixture f;
    f.keeper.set_drift_ppb(250'000);
    EXPECT_EQ(f.keeper.drift_ppb(), 200'000);
    f.keeper.set_drift_ppb(-250'000);
    EXPECT_EQ(f.keeper.drift_ppb(), -200'000);
    f.keeper.set_drift_ppb(-123);
    EXPECT_EQ(f.keeper.drift_ppb(), -123);
}

} // namespace
} // namespace qz::time
