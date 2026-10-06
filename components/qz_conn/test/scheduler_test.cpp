// conn::Scheduler: preconditions, due logic, piggyback, backoff + jitter, result bookkeeping.
#include "qz/conn/conn.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <set>

namespace qz::conn {
namespace {

using model::ConnectivityMode;
using model::PowerLevel;
using model::SyncIndicator;
using time::UnixSeconds;

constexpr UnixSeconds kNow = 1'800'000'000;
constexpr std::int64_t kH = 3600;
constexpr std::int64_t kMin = 60;

Inputs good_inputs(ConnectivityMode mode = ConnectivityMode::kTimeWeather) {
    Inputs in;
    in.mode = mode;
    in.has_credentials = true;
    in.location_set = true;
    in.time_valid = true;
    in.now_utc = kNow;
    return in;
}

struct SchedulerTest : ::testing::Test {
    ConnState state;
    Scheduler sched{state, 0x1234ABCDU};
};

// ------------------------------------------------------------ preconditions
TEST_F(SchedulerTest, OffPlansNothing) {
    Inputs in = good_inputs(ConnectivityMode::kOff);
    EXPECT_FALSE(sched.plan(in).any());
    in.manual_request = true;
    EXPECT_FALSE(sched.plan(in).any());
}

TEST_F(SchedulerTest, RadioCompiledOutPlansNothing) {
    Inputs in = good_inputs();
    in.radio_compiled = false;
    EXPECT_FALSE(sched.plan(in).any());
    in.manual_request = true;
    EXPECT_FALSE(sched.plan(in).any());
}

TEST_F(SchedulerTest, NoCredentialsPlansNothing) {
    Inputs in = good_inputs();
    in.has_credentials = false;
    EXPECT_FALSE(sched.plan(in).any());
    in.manual_request = true;
    EXPECT_FALSE(sched.plan(in).any());
}

TEST_F(SchedulerTest, NonNormalPowerPlansNothing) {
    for (const PowerLevel level : {PowerLevel::kLow, PowerLevel::kSaver, PowerLevel::kCritical}) {
        Inputs in = good_inputs();
        in.power = level;
        EXPECT_FALSE(sched.plan(in).any());
        in.manual_request = true;
        EXPECT_FALSE(sched.plan(in).any());
    }
}

TEST_F(SchedulerTest, InvalidTimePlansTimeOnly) {
    Inputs in = good_inputs();
    in.time_valid = false;
    in.now_utc = 0;
    const Plan p = sched.plan(in);
    EXPECT_TRUE(p.time);
    EXPECT_FALSE(p.weather);
}

TEST_F(SchedulerTest, FreshStatePlansByMode) {
    Plan p = sched.plan(good_inputs(ConnectivityMode::kTimeOnly));
    EXPECT_TRUE(p.time);
    EXPECT_FALSE(p.weather);
    p = sched.plan(good_inputs(ConnectivityMode::kTimeWeather));
    EXPECT_TRUE(p.time);
    EXPECT_TRUE(p.weather);
    Inputs no_loc = good_inputs();
    no_loc.location_set = false;
    p = sched.plan(no_loc);
    EXPECT_TRUE(p.time);
    EXPECT_FALSE(p.weather);
}

TEST_F(SchedulerTest, NothingDueBeforeNextTimes) {
    state.next_time_sync = kNow + (5 * kH);
    state.next_weather = kNow + (5 * kH);
    EXPECT_FALSE(sched.plan(good_inputs()).any());
    state.next_time_sync = kNow; // due exactly now
    const Plan p = sched.plan(good_inputs());
    EXPECT_TRUE(p.time);
    EXPECT_FALSE(p.weather);
}

TEST_F(SchedulerTest, ManualIgnoresBackoffNotPreconditions) {
    state.next_time_sync = kNow + (10 * kH);
    state.next_weather = kNow + (10 * kH);
    state.fail_streak = 5;
    Inputs in = good_inputs();
    in.manual_request = true;
    const Plan p = sched.plan(in);
    EXPECT_TRUE(p.time);
    EXPECT_TRUE(p.weather);
    in.mode = ConnectivityMode::kTimeOnly;
    EXPECT_FALSE(sched.plan(in).weather);
    in.mode = ConnectivityMode::kTimeWeather;
    in.time_valid = false;
    EXPECT_FALSE(sched.plan(in).weather); // weather needs valid time even when manual
}

// ------------------------------------------------------------ piggyback
TEST_F(SchedulerTest, WeatherPiggybacksOnDueTimeWithinTwoHours) {
    state.next_time_sync = 0; // due
    state.next_weather = kNow + (2 * kH);
    EXPECT_TRUE(sched.plan(good_inputs()).weather); // exactly 2 h: included
    state.next_weather = kNow + (2 * kH) + 1;
    EXPECT_FALSE(sched.plan(good_inputs()).weather);
}

TEST_F(SchedulerTest, TimePiggybacksOnDueWeatherWithinTwoHours) {
    state.next_weather = 0; // due
    state.next_time_sync = kNow + (2 * kH);
    Plan p = sched.plan(good_inputs());
    EXPECT_TRUE(p.time);
    EXPECT_TRUE(p.weather);
    state.next_time_sync = kNow + (2 * kH) + 1;
    p = sched.plan(good_inputs());
    EXPECT_FALSE(p.time);
    EXPECT_TRUE(p.weather);
}

TEST_F(SchedulerTest, NoPiggybackIntoIneligibleWeather) {
    state.next_time_sync = 0;
    state.next_weather = kNow + kH;
    EXPECT_FALSE(sched.plan(good_inputs(ConnectivityMode::kTimeOnly)).weather);
}

// ------------------------------------------------------------ backoff
TEST(Backoff, BaseSequenceAndCapDailyInterval) {
    constexpr std::int64_t kDay = 24 * kH;
    const std::int64_t expect[] = {0,
                                   15 * kMin,
                                   30 * kMin,
                                   60 * kMin,
                                   120 * kMin,
                                   240 * kMin,
                                   480 * kMin,
                                   720 * kMin,
                                   720 * kMin};
    for (std::size_t n = 0; n < std::size(expect); ++n) {
        EXPECT_EQ(Scheduler::backoff_base_s(static_cast<std::uint8_t>(n), kDay), expect[n])
            << "streak " << n;
    }
    EXPECT_EQ(Scheduler::backoff_base_s(255, kDay), 12 * kH); // no overflow at saturation
}

TEST(Backoff, CapIsTheIntervalWhenShorter) {
    EXPECT_EQ(Scheduler::backoff_base_s(1, 30 * kMin), 15 * kMin);
    EXPECT_EQ(Scheduler::backoff_base_s(2, 30 * kMin), 30 * kMin);
    EXPECT_EQ(Scheduler::backoff_base_s(9, 30 * kMin), 30 * kMin);
    EXPECT_EQ(Scheduler::backoff_base_s(9, 6 * kH), 6 * kH);
    EXPECT_EQ(Scheduler::backoff_base_s(9, 168 * kH), 12 * kH); // 12 h absolute cap
    EXPECT_EQ(Scheduler::backoff_base_s(1, 0), 15 * kMin);      // degenerate interval clamped
}

/// One (seed, streak) point: inside +-10 % of the base and reproducible.
void expect_jitter_point(std::uint32_t seed, std::uint8_t n) {
    const std::int64_t base = Scheduler::backoff_base_s(n, 24 * kH);
    const std::int64_t d = Scheduler::backoff_delay_s(n, 24 * kH, seed);
    EXPECT_GE(d * 10, base * 9) << "seed " << seed << " n " << int{n};
    EXPECT_LE(d * 10, base * 11) << "seed " << seed << " n " << int{n};
    EXPECT_EQ(d, Scheduler::backoff_delay_s(n, 24 * kH, seed));
}

TEST(Backoff, JitterWithinTenPercentAndDeterministic) {
    for (std::uint32_t seed = 0; seed < 2000; ++seed) {
        for (std::uint8_t n = 1; n <= 12; ++n) {
            expect_jitter_point(seed * 2654435761U, n);
        }
    }
    EXPECT_EQ(Scheduler::backoff_delay_s(0, 24 * kH, 99), 0);
}

TEST(Backoff, JitterSpreadsAcrossBothSidesOfTheBase) {
    std::set<std::int64_t> distinct;
    bool saw_low = false;
    bool saw_high = false;
    const std::int64_t base = Scheduler::backoff_base_s(3, 24 * kH);
    for (std::uint32_t seed = 0; seed < 2000; ++seed) {
        const std::int64_t d = Scheduler::backoff_delay_s(3, 24 * kH, seed * 2654435761U);
        distinct.insert(d);
        saw_low = saw_low || (d * 100 < base * 92);
        saw_high = saw_high || (d * 100 > base * 108);
    }
    EXPECT_GT(distinct.size(), 50U); // jitter really spreads
    EXPECT_TRUE(saw_low);
    EXPECT_TRUE(saw_high);
}

TEST(Backoff, DifferentSeedsDifferentDelays) {
    EXPECT_NE(Scheduler::backoff_delay_s(4, 24 * kH, 1), Scheduler::backoff_delay_s(4, 24 * kH, 2));
}

TEST_F(SchedulerTest, RetryDelayFollowsStreak) {
    const Inputs in = good_inputs();
    EXPECT_EQ(sched.retry_delay_s(in), 0);
    state.fail_streak = 3;
    EXPECT_EQ(sched.retry_delay_s(in), Scheduler::backoff_delay_s(3, 24 * kH, 0x1234ABCDU));
}

// ------------------------------------------------------------ on_result
SessionResult ok_result() {
    return SessionResult{};
}

SessionResult connect_failed() {
    SessionResult r;
    r.connect = Errc::kTimeout;
    r.time = Errc::kTimeout;
    r.weather = Errc::kTimeout;
    return r;
}

TEST_F(SchedulerTest, SuccessSchedulesIntervalsAndResets) {
    state.fail_streak = 4;
    state.last_error = 7;
    Inputs in = good_inputs();
    in.sync_interval_h = 12;
    in.weather_interval_min = 120;
    sched.on_result(Plan{true, true}, ok_result(), in);
    EXPECT_EQ(state.next_time_sync, kNow + (12 * kH));
    EXPECT_EQ(state.next_weather, kNow + (120 * kMin));
    EXPECT_EQ(state.fail_streak, 0);
    EXPECT_EQ(state.last_error, 0);
    EXPECT_EQ(state.last_ok_utc, kNow);
    EXPECT_EQ(state.last_attempt_utc, kNow);
    EXPECT_EQ(state.sessions, 1U);
    EXPECT_EQ(state.ever_synced, 1);
}

TEST_F(SchedulerTest, FailureSequenceBacksOffAndCaps) {
    const Inputs in = good_inputs();
    for (std::uint8_t n = 1; n <= 10; ++n) {
        sched.on_result(Plan{true, false}, connect_failed(), in);
        EXPECT_EQ(state.fail_streak, n);
        const std::int64_t delay = state.next_time_sync - kNow;
        const std::int64_t base = Scheduler::backoff_base_s(n, 24 * kH);
        EXPECT_TRUE(delay * 10 >= base * 9 && delay * 10 <= base * 11) << "streak " << int{n};
        EXPECT_EQ(delay, Scheduler::backoff_delay_s(n, 24 * kH, 0x1234ABCDU));
    }
    EXPECT_EQ(Scheduler::backoff_base_s(state.fail_streak, 24 * kH), 12 * kH);
}

TEST_F(SchedulerTest, FailureRecordsErrorAndNeverCountsAsSynced) {
    sched.on_result(Plan{true, false}, connect_failed(), good_inputs());
    EXPECT_EQ(state.ever_synced, 0);
    EXPECT_EQ(state.last_ok_utc, 0);
    EXPECT_EQ(state.last_error,
              static_cast<std::uint8_t>(static_cast<unsigned>(Errc::kTimeout) + 1U));
}

TEST_F(SchedulerTest, BackoffHoldsRadioOffUntilSuccessResets) {
    const Inputs in = good_inputs(ConnectivityMode::kTimeOnly);
    sched.on_result(Plan{true, false}, connect_failed(), in);
    Inputs later = in;
    later.now_utc = kNow + 60;
    EXPECT_FALSE(sched.plan(later).time); // interval has not elapsed and backoff pending
    later.now_utc = kNow + (2 * kH);
    EXPECT_TRUE(sched.plan(later).time); // 15 min +10 % has passed
    sched.on_result(Plan{true, false}, ok_result(), in);
    EXPECT_EQ(state.fail_streak, 0);
    EXPECT_EQ(state.next_time_sync, kNow + (24 * kH));
}

TEST_F(SchedulerTest, PartialFailureKeepsSuccessfulJobOnItsInterval) {
    SessionResult r;
    r.weather = Errc::kIo;
    sched.on_result(Plan{true, true}, r, good_inputs());
    EXPECT_EQ(state.next_time_sync, kNow + (24 * kH));
    EXPECT_EQ(state.fail_streak, 1);
    EXPECT_LE(state.next_weather - kNow, 17 * kMin); // 15 min +10 %
    EXPECT_GE(state.next_weather - kNow, 13 * kMin);
    EXPECT_EQ(state.last_error, static_cast<std::uint8_t>(static_cast<unsigned>(Errc::kIo) + 1U));
    EXPECT_EQ(state.ever_synced, 1);
    EXPECT_EQ(state.last_ok_utc, 0); // not a fully successful session
}

TEST_F(SchedulerTest, UnplannedJobIsUntouched) {
    state.next_weather = 42;
    sched.on_result(Plan{true, false}, ok_result(), good_inputs());
    EXPECT_EQ(state.next_weather, 42);
}

TEST_F(SchedulerTest, EmptyPlanIsNoOp) {
    sched.on_result(Plan{}, connect_failed(), good_inputs());
    EXPECT_EQ(state.sessions, 0U);
    EXPECT_EQ(state.fail_streak, 0);
}

TEST_F(SchedulerTest, TimeLearnedFromSntpWhenClockInvalid) {
    Inputs in = good_inputs();
    in.time_valid = false;
    in.now_utc = 0;
    SessionResult r;
    r.sntp_utc_us = ((kNow + 5) * 1'000'000LL) + 123;
    sched.on_result(Plan{true, false}, r, in);
    EXPECT_EQ(state.next_time_sync, kNow + 5 + (24 * kH));
    EXPECT_EQ(state.last_ok_utc, kNow + 5);
}

TEST_F(SchedulerTest, FailureWithUnknownUtcKeepsDueTimesButCountsStreak) {
    Inputs in = good_inputs();
    in.time_valid = false;
    in.now_utc = 0;
    sched.on_result(Plan{true, false}, connect_failed(), in);
    EXPECT_EQ(state.fail_streak, 1);
    EXPECT_EQ(state.next_time_sync, 0);
    EXPECT_EQ(state.last_attempt_utc, 0);
}

TEST_F(SchedulerTest, ManualFailureAlsoCountsStreak) {
    Inputs in = good_inputs();
    in.manual_request = true;
    sched.on_result(Plan{true, true}, connect_failed(), in);
    EXPECT_EQ(state.fail_streak, 1);
}

TEST_F(SchedulerTest, StreakAndSessionCountersSaturate) {
    state.fail_streak = 255;
    state.sessions = UINT32_MAX;
    sched.on_result(Plan{true, false}, connect_failed(), good_inputs());
    EXPECT_EQ(state.fail_streak, 255);
    EXPECT_EQ(state.sessions, UINT32_MAX);
}

TEST_F(SchedulerTest, ConfigChangeMakesJobsDue) {
    state.next_time_sync = kNow + kH;
    state.next_weather = kNow + kH;
    state.fail_streak = 3;
    state.last_error = 4;
    sched.on_config_changed();
    EXPECT_EQ(state.next_time_sync, 0);
    EXPECT_EQ(state.next_weather, 0);
    EXPECT_EQ(state.fail_streak, 0);
    EXPECT_EQ(state.last_error, 0);
    EXPECT_TRUE(sched.plan(good_inputs()).any());
}

// ------------------------------------------------------------ indicator
TEST_F(SchedulerTest, IndicatorStates) {
    Inputs in = good_inputs();
    EXPECT_EQ(sched.indicator(in, 0), SyncIndicator::kNeverSynced);
    EXPECT_EQ(sched.indicator(in, kNow - kH), SyncIndicator::kOk);
    EXPECT_EQ(sched.indicator(in, kNow - (48 * kH)), SyncIndicator::kOk); // exactly 2 intervals
    EXPECT_EQ(sched.indicator(in, kNow - (48 * kH) - 1), SyncIndicator::kStale);
    state.fail_streak = 1;
    EXPECT_EQ(sched.indicator(in, kNow - kH), SyncIndicator::kLastFailed);
    state.fail_streak = 0;
    in.mode = ConnectivityMode::kOff;
    EXPECT_EQ(sched.indicator(in, kNow), SyncIndicator::kNone);
    in = good_inputs();
    in.has_credentials = false;
    EXPECT_EQ(sched.indicator(in, kNow), SyncIndicator::kNone);
    in = good_inputs();
    in.radio_compiled = false;
    EXPECT_EQ(sched.indicator(in, kNow), SyncIndicator::kNone);
    in = good_inputs();
    in.time_valid = false;
    EXPECT_EQ(sched.indicator(in, kNow - (100 * kH)), SyncIndicator::kOk); // cannot judge age
}

// ------------------------------------------------------------ Off means off
TEST(OffMeansOff, SevenDaysNeverStartsTheRadio) {
    testkit::VirtualClock clock;
    const testkit::FakeNetStack net(clock);
    ConnState state;
    const Scheduler sched(state, 7);
    Inputs in = good_inputs(ConnectivityMode::kOff);
    constexpr std::int64_t kMinutes = std::int64_t{7} * 24 * 60;
    for (std::int64_t m = 0; m < kMinutes; ++m) {
        in.now_utc = kNow + (m * 60);
        in.manual_request = (m % 1000 == 0);
        const Plan p = sched.plan(in);
        if (p.any()) {
            ADD_FAILURE() << "planned at minute " << m;
            break;
        }
    }
    EXPECT_EQ(net.connect_calls(), 0U);
    EXPECT_EQ(net.radio_init_count(), 0U);
}

} // namespace
} // namespace qz::conn
