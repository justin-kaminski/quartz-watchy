// VirtualClock: virtual RTC + libc clock; the drift arithmetic must be exact (WP-01 acceptance).
#include "qz/hal/delay.hpp"
#include "qz/hal/system.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace qz::testkit {
namespace {

constexpr std::int64_t kSecond = 1'000'000;       // us
constexpr std::int64_t kDay = 86'400 * kSecond;   // us
constexpr std::int64_t kPpbScale = 1'000'000'000; // ppb denominator
constexpr std::int32_t kMaxPpb = 500'000'000;     // documented limit of the fake

/// RTC reading for T microseconds of true time at a constant error from a fresh clock:
/// floor(T * (1e9 + ppb) / 1e9). Exact in uint64 for T <= 1e10.
std::uint64_t reference_rtc_us(std::uint64_t true_us, std::int32_t ppb) {
    const auto rate = static_cast<std::uint64_t>(kPpbScale + ppb);
    return (true_us * rate) / static_cast<std::uint64_t>(kPpbScale);
}

/// Deterministic xorshift32 stream for chunk sizes.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed) {}
    std::uint32_t next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return state_;
    }

private:
    std::uint32_t state_;
};

// --- plain time ---------------------------------------------------------------------------------

TEST(VirtualClock, StartsAtZeroEverywhere) {
    const VirtualClock clock;
    EXPECT_EQ(clock.rtc_us(), 0);
    EXPECT_EQ(clock.true_utc_us(), 0);
    EXPECT_EQ(clock.elapsed_us(), 0);
    EXPECT_EQ(clock.system_utc_us(), 0);
}

TEST(VirtualClock, AdvanceMovesRtcAndTruthTogetherWithoutCrystalError) {
    VirtualClock clock;
    clock.advance_us(1500);
    EXPECT_EQ(clock.rtc_us(), 1500);
    EXPECT_EQ(clock.true_utc_us(), 1500);
    EXPECT_EQ(clock.elapsed_us(), 1500);
    clock.advance_us(0);
    EXPECT_EQ(clock.rtc_us(), 1500);
    clock.advance_us(kDay);
    EXPECT_EQ(clock.rtc_us(), 1500 + kDay);
}

TEST(VirtualClock, DelaysAdvanceVirtualTimeThroughTheHalInterfaces) {
    VirtualClock clock;
    hal::Delay& delay = clock;
    const hal::Clock& rtc = clock;
    delay.delay_us(250);
    EXPECT_EQ(rtc.rtc_us(), 250);
    delay.delay_ms(40);
    EXPECT_EQ(rtc.rtc_us(), 250 + 40'000);
    delay.delay_ms(std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(rtc.rtc_us(), 250 + 40'000 + (std::int64_t{4'294'967'295} * 1000));
    delay.delay_us(std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(clock.elapsed_us(),
              250 + 40'000 + (std::int64_t{4'294'967'295} * 1000) + 4'294'967'295);
}

TEST(VirtualClock, TruthIsSettableAndIndependentOfTheRtc) {
    VirtualClock clock;
    clock.set_true_utc_us(1'700'000'000'000'000);
    clock.advance_us(2 * kSecond);
    EXPECT_EQ(clock.true_utc_us(), 1'700'000'002'000'000);
    EXPECT_EQ(clock.rtc_us(), 2 * kSecond);
    clock.set_true_utc_us(-5);
    EXPECT_EQ(clock.true_utc_us(), -5);
    clock.advance_us(10);
    EXPECT_EQ(clock.true_utc_us(), 5);
}

TEST(VirtualClock, ElapsedIgnoresTruthJumpsPowerLossAndCrystalError) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(-50'000);
    clock.advance_us(kSecond);
    clock.set_true_utc_us(42);
    clock.power_loss();
    clock.advance_us(kSecond);
    EXPECT_EQ(clock.elapsed_us(), 2 * kSecond);
}

// --- crystal error: exact arithmetic ------------------------------------------------------------

TEST(VirtualClockDrift, FortyPpmIsExactlyThreePointFourFiveSixSecondsPerDay) {
    VirtualClock fast;
    fast.set_crystal_error_ppb(40'000);
    fast.advance_us(kDay);
    EXPECT_EQ(fast.rtc_us() - kDay, 3'456'000);

    VirtualClock slow;
    slow.set_crystal_error_ppb(-40'000);
    slow.advance_us(kDay);
    EXPECT_EQ(slow.rtc_us() - kDay, -3'456'000);
}

TEST(VirtualClockDrift, LargeSpansAreExact) {
    VirtualClock fast;
    fast.set_crystal_error_ppb(50'000);
    fast.advance_us(1'000'000'000'000); // 1e6 s
    EXPECT_EQ(fast.rtc_us(), 1'000'050'000'000);

    VirtualClock slow;
    slow.set_crystal_error_ppb(-50'000);
    slow.advance_us(1'000'000'000'000);
    EXPECT_EQ(slow.rtc_us(), 999'950'000'000);

    // Ten years in one step at +200 ppm (the clamp of the drift estimator): no overflow.
    VirtualClock decade;
    decade.set_crystal_error_ppb(200'000);
    constexpr std::int64_t kTenYears = 315'360'000 * kSecond;
    decade.advance_us(kTenYears);
    EXPECT_EQ(decade.rtc_us(), kTenYears + (kTenYears / 5000));
}

TEST(VirtualClockDrift, SubMicrosecondRemaindersAreCarriedBetweenSteps) {
    // +1 ppb: 1e9 us of true time yield exactly one extra RTC microsecond, however it is cut up.
    VirtualClock clock;
    clock.set_crystal_error_ppb(1);
    for (int i = 0; i < 999; ++i) {
        clock.advance_us(1'000'000);
    }
    EXPECT_EQ(clock.rtc_us(), 999'000'000) << "999e6 ppb-us carried, not yet a whole microsecond";
    clock.advance_us(1'000'000);
    EXPECT_EQ(clock.rtc_us(), 1'000'000'001);
}

TEST(VirtualClockDrift, OneMicrosecondStepsAddUpLikeOneBigStep) {
    VirtualClock stepped;
    stepped.set_crystal_error_ppb(50'000);
    for (int i = 0; i < 1'000'000; ++i) {
        stepped.advance_us(1);
    }
    VirtualClock single;
    single.set_crystal_error_ppb(50'000);
    single.advance_us(1'000'000);
    EXPECT_EQ(stepped.rtc_us(), 1'000'050);
    EXPECT_EQ(single.rtc_us(), 1'000'050);
}

TEST(VirtualClockDrift, NegativeErrorFollowsTheFloorOfTheRtcScale) {
    // The RTC shows floor(T * (1 - 1e-9)): it is one microsecond behind from the first one on.
    VirtualClock clock;
    clock.set_crystal_error_ppb(-1);
    clock.advance_us(1);
    EXPECT_EQ(clock.rtc_us(), 0);
    clock.advance_us(999'999'999);
    EXPECT_EQ(clock.rtc_us(), 999'999'999);
    clock.advance_us(1);
    EXPECT_EQ(clock.rtc_us(), 1'000'000'000 - 1);
    clock.advance_us(1);
    EXPECT_EQ(clock.rtc_us(), 1'000'000'000) << "the lost microsecond is repaid";
}

TEST(VirtualClockDrift, ResultDependsOnlyOnTheTotalForAnyChunking) {
    constexpr std::array<std::int32_t, 11> kErrors = {
        0, 1, -1, 999, 50'000, -50'000, 123'456'789, -123'456'789, kMaxPpb, -kMaxPpb, 200'000};
    constexpr std::uint64_t kTotalLimit = 10'000'000'000ULL; // 1e10 us: reference stays in uint64
    constexpr int kSmallSteps = 5000;
    for (const std::int32_t ppb : kErrors) {
        VirtualClock clock;
        clock.set_crystal_error_ppb(ppb);
        Rng rng(0x1234ABCDU + static_cast<std::uint32_t>(ppb));
        std::uint64_t total = 0;
        int steps = 0;
        while (total < kTotalLimit) {
            // First thousands of tiny steps (0..2999 us) so that remainders are carried over and
            // over, then big ones (0..2.5 s, i.e. also beyond 1e9 us: the quotient path).
            const std::uint32_t pick = rng.next();
            const std::uint64_t step =
                (steps++ < kSmallSteps) ? (pick % 3000U) : (pick % 2'500'000'000U);
            clock.advance_us(static_cast<std::int64_t>(step));
            total += step;
            ASSERT_EQ(static_cast<std::uint64_t>(clock.rtc_us()), reference_rtc_us(total, ppb))
                << "ppb " << ppb << " after " << total << " us";
        }
        EXPECT_EQ(static_cast<std::uint64_t>(clock.elapsed_us()), total);
    }
}

TEST(VirtualClockDrift, ErrorChangesKeepTheCarriedRemainder) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(kMaxPpb); // x1.5
    clock.advance_us(1);                  // 1.5 -> 1, remainder .5
    EXPECT_EQ(clock.rtc_us(), 1);
    clock.set_crystal_error_ppb(0);
    clock.advance_us(1); // exactly 1, remainder untouched
    EXPECT_EQ(clock.rtc_us(), 2);
    clock.set_crystal_error_ppb(kMaxPpb);
    clock.advance_us(1); // 1.5 + .5 carried -> 2
    EXPECT_EQ(clock.rtc_us(), 4) << "1.5 + 1 + 1.5 true microseconds of RTC time";
}

TEST(VirtualClockDrift, OppositeErrorsOverEqualSpansCancelExactly) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(100'000);
    clock.advance_us(1'000'000'000);
    clock.set_crystal_error_ppb(-100'000);
    clock.advance_us(1'000'000'000);
    EXPECT_EQ(clock.rtc_us(), 2'000'000'000);
}

TEST(VirtualClockDrift, ErrorBeyondThePlausibleRangeIsRejected) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(kMaxPpb);
    clock.set_crystal_error_ppb(-kMaxPpb);
    EXPECT_DEATH(clock.set_crystal_error_ppb(kMaxPpb + 1), "QZ_ASSERT");
    EXPECT_DEATH(clock.set_crystal_error_ppb(-kMaxPpb - 1), "QZ_ASSERT");
}

// --- advance by RTC time (sleep timers) ---------------------------------------------------------

TEST(VirtualClockRtcAdvance, WithoutErrorRtcAndTrueTimeAgree) {
    VirtualClock clock;
    clock.advance_rtc_us(60 * kSecond);
    EXPECT_EQ(clock.rtc_us(), 60 * kSecond);
    EXPECT_EQ(clock.elapsed_us(), 60 * kSecond);
    clock.advance_rtc_us(0);
    EXPECT_EQ(clock.rtc_us(), 60 * kSecond);
}

TEST(VirtualClockRtcAdvance, FastCrystalReachesTheTimerInLessTrueTime) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(50'000);
    clock.advance_rtc_us(60 * kSecond);
    EXPECT_EQ(clock.rtc_us(), 60 * kSecond);
    EXPECT_EQ(clock.elapsed_us(), 59'997'001); // ceil(60e6 / 1.00005)
}

TEST(VirtualClockRtcAdvance, SlowCrystalNeedsMoreTrueTime) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(-50'000);
    clock.advance_rtc_us(60 * kSecond);
    EXPECT_EQ(clock.rtc_us(), 60 * kSecond);
    EXPECT_EQ(clock.elapsed_us(), 60'003'001); // ceil(60e6 / 0.99995)
}

/// Checks advance_rtc_us(target) on a clock with the given error and warm-up (which leaves an
/// arbitrary sub-microsecond remainder): enough true time, and not one microsecond more than
/// needed.
void expect_fewest_true_microseconds(std::int32_t ppb, std::int64_t warmup, std::int64_t target) {
    VirtualClock a;
    a.set_crystal_error_ppb(ppb);
    a.advance_us(warmup);
    VirtualClock b = a; // identical state, including the sub-microsecond remainder

    const std::int64_t rtc0 = a.rtc_us();
    const std::int64_t true0 = a.elapsed_us();
    a.advance_rtc_us(target);
    const std::int64_t true_us = a.elapsed_us() - true0;
    const std::int64_t rtc_advance = a.rtc_us() - rtc0;

    b.advance_us(true_us - 1);
    const std::int64_t rtc_one_less = b.rtc_us() - rtc0;

    const bool ok =
        true_us >= 1 && rtc_advance >= target && rtc_advance <= target + 1 && rtc_one_less < target;
    EXPECT_TRUE(ok) << "ppb " << ppb << " warmup " << warmup << " target " << target << ": needed "
                    << true_us << " us true time, RTC advanced " << rtc_advance
                    << ", one microsecond less would give " << rtc_one_less;
}

TEST(VirtualClockRtcAdvance, IsTheFewestTrueMicrosecondsForEveryStateWeTry) {
    // The RTC may overshoot the target by one microsecond (a x1.5 clock skips values), never more.
    constexpr std::array<std::int32_t, 9> kErrors = {
        0, 1, -1, 50'000, -50'000, 777'777, -777'777, kMaxPpb, -kMaxPpb};
    constexpr std::array<std::int64_t, 6> kWarmups = {
        0, 1, 7, 333'333'333, 999'999'999, 1'234'567'891};
    constexpr std::array<std::int64_t, 12> kTargets = {1,
                                                       2,
                                                       3,
                                                       10,
                                                       999,
                                                       1000,
                                                       123'456,
                                                       1'000'000,
                                                       999'999'999,
                                                       1'000'000'000,
                                                       10'000'000'000,
                                                       86'400'000'000};
    for (const std::int32_t ppb : kErrors) {
        for (const std::int64_t warmup : kWarmups) {
            for (const std::int64_t target : kTargets) {
                expect_fewest_true_microseconds(ppb, warmup, target);
            }
        }
    }
}

TEST(VirtualClockRtcAdvance, RejectsNegativeAndAbsurdDurations) {
    VirtualClock clock;
    EXPECT_DEATH(clock.advance_rtc_us(-1), "QZ_ASSERT");
    EXPECT_DEATH(clock.advance_rtc_us(std::numeric_limits<std::int64_t>::max()), "QZ_ASSERT");
}

// --- power loss ---------------------------------------------------------------------------------

TEST(VirtualClockPowerLoss, RestartsRtcAndLibcClockButNotTheWorld) {
    VirtualClock clock;
    clock.set_crystal_error_ppb(-1);
    clock.set_true_utc_us(1000);
    clock.set_system_utc_us(5000);
    clock.advance_us(10 * kSecond);
    clock.power_loss();
    EXPECT_EQ(clock.rtc_us(), 0);
    EXPECT_EQ(clock.system_utc_us(), 0);
    EXPECT_EQ(clock.true_utc_us(), 1000 + (10 * kSecond));
    EXPECT_EQ(clock.elapsed_us(), 10 * kSecond);

    // The crystal is still slow, and the lost sub-microsecond remainder is gone: behaves like new.
    clock.advance_us(1);
    EXPECT_EQ(clock.rtc_us(), 0);
    clock.advance_us(999'999'999);
    EXPECT_EQ(clock.rtc_us(), 999'999'999);
}

TEST(VirtualClockPowerLoss, OutageIsModelledByAdvancingBeforeTheLoss) {
    VirtualClock clock;
    clock.advance_us(100 * kSecond);
    clock.advance_us(3600 * kSecond); // the battery is out for an hour
    clock.power_loss();               // power returns
    EXPECT_EQ(clock.rtc_us(), 0);
    EXPECT_EQ(clock.true_utc_us(), 3700 * kSecond);
}

// --- libc clock ---------------------------------------------------------------------------------

TEST(VirtualClockLibc, UnsetClockReadsTimeSincePowerOn) {
    VirtualClock clock;
    clock.advance_us(12'345);
    EXPECT_EQ(clock.system_utc_us(), 12'345);
}

TEST(VirtualClockLibc, SetValueTicksWithTheRtc) {
    VirtualClock clock;
    hal::Clock& hal_clock = clock;
    clock.advance_us(5 * kSecond);
    hal_clock.set_system_utc_us(1'760'000'000'000'000);
    EXPECT_EQ(clock.system_utc_us(), 1'760'000'000'000'000);
    clock.advance_us(2 * kSecond);
    EXPECT_EQ(clock.system_utc_us(), 1'760'000'002'000'000);

    clock.set_crystal_error_ppb(50'000);
    clock.advance_us(kSecond);
    EXPECT_EQ(clock.system_utc_us(), 1'760'000'003'000'050) << "the libc clock follows the RTC";
}

TEST(VirtualClockLibc, SettingDoesNotTouchRtcOrTruth) {
    VirtualClock clock;
    clock.advance_us(100);
    clock.set_system_utc_us(-777);
    EXPECT_EQ(clock.rtc_us(), 100);
    EXPECT_EQ(clock.true_utc_us(), 100);
    EXPECT_EQ(clock.system_utc_us(), -777);
}

// --- programmer errors --------------------------------------------------------------------------

TEST(VirtualClockDeathTest, NegativeAdvanceAborts) {
    VirtualClock clock;
    EXPECT_DEATH(clock.advance_us(-1), "QZ_ASSERT.*us >= 0");
}

TEST(VirtualClockDeathTest, StepBeyondTheSupportedRangeAborts) {
    VirtualClock clock;
    EXPECT_DEATH(clock.advance_us(std::numeric_limits<std::int64_t>::max()), "QZ_ASSERT");
}

TEST(VirtualClockDeathTest, OverflowingTheTruthClockAborts) {
    VirtualClock clock;
    clock.set_true_utc_us(std::numeric_limits<std::int64_t>::max() - 5);
    EXPECT_DEATH(clock.advance_us(10), "QZ_ASSERT.*true_utc_us_");
}

TEST(VirtualClockDeathTest, OverflowingTheLibcClockAborts) {
    VirtualClock clock;
    clock.set_system_utc_us(std::numeric_limits<std::int64_t>::max() - 5);
    EXPECT_DEATH(clock.advance_us(10), "QZ_ASSERT.*system_utc_us_");
}

TEST(VirtualClock, ImplementsBothHalInterfaces) {
    static_assert(std::is_base_of_v<hal::Clock, VirtualClock>);
    static_assert(std::is_base_of_v<hal::Delay, VirtualClock>);
    SUCCEED();
}

} // namespace
} // namespace qz::testkit
