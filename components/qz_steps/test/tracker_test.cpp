// One test per row of the ARCHITECTURE.md section 10 table, plus accounting invariants.
#include "qz/steps/step_tracker.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace qz::steps {
namespace {

using test::day_num;
using test::history_steps;
using test::Rig;
using test::utc;

constexpr std::int32_t kY = 2026;

// ---- Row: normal read c >= last -> delta = c - last ----
TEST(StepRows, NormalReadDeltaIsDifference) {
    Rig r;
    r.hw = 1000;
    (void)r.read(utc(kY, 6, 10, 10), 1000); // baseline
    const StepUpdate u = r.read(utc(kY, 6, 10, 10, 1), 1025);
    EXPECT_EQ(u.delta, 25U);
    EXPECT_EQ(r.state.today, 25U);
    const StepUpdate same = r.read(utc(kY, 6, 10, 10, 2), 1025);
    EXPECT_EQ(same.delta, 0U);
    EXPECT_EQ(r.state.today, 25U);
    EXPECT_EQ(r.state.last_hw_count, 1025U);
}

TEST(StepRows, FirstReadingOnlyEstablishesBaseline) {
    Rig r;
    const StepUpdate u = r.read(utc(kY, 6, 10, 10), 987'654);
    EXPECT_EQ(u.delta, 0U);
    EXPECT_EQ(r.state.today, 0U);
    EXPECT_EQ(r.state.has_baseline, 1U);
    EXPECT_EQ(r.state.last_hw_count, 987'654U);
}

// ---- Row: c < last -> sensor reset, delta = c ----
TEST(StepRows, CounterBelowLastIsSensorReset) {
    Rig r;
    (void)r.read(utc(kY, 6, 10, 10), 5000);
    (void)r.read(utc(kY, 6, 10, 10, 1), 5100);
    const StepUpdate u = r.read(utc(kY, 6, 10, 10, 2), 40);
    EXPECT_EQ(u.delta, 40U);
    EXPECT_EQ(r.state.today, 140U);
    EXPECT_EQ(r.state.last_hw_count, 40U);
    EXPECT_EQ(r.read(utc(kY, 6, 10, 10, 3), 55).delta, 15U);
}

TEST(StepRows, CounterNearUint32MaxIsNotAWrap) {
    Rig r;
    constexpr std::uint32_t kMax = std::numeric_limits<std::uint32_t>::max();
    (void)r.read(utc(kY, 6, 10, 10), kMax - 5);
    EXPECT_EQ(r.read(utc(kY, 6, 10, 10, 1), kMax).delta, 5U);
    EXPECT_EQ(r.read(utc(kY, 6, 10, 10, 2), 3).delta, 3U); // reset, not a 32-bit wrap
}

TEST(StepRows, ExplicitSensorResetMakesNextReadingABaseline) {
    Rig r;
    (void)r.read(utc(kY, 6, 10, 10), 5000);
    (void)r.read(utc(kY, 6, 10, 10, 1), 5010);
    r.tracker.on_sensor_reset();
    EXPECT_EQ(r.read(utc(kY, 6, 10, 10, 2), 7).delta, 0U);
    EXPECT_EQ(r.state.today, 10U);
    EXPECT_EQ(r.read(utc(kY, 6, 10, 10, 3), 19).delta, 12U);
    EXPECT_EQ(r.state.today, 22U);
}

// ---- Row: local day = days_from_civil(local date) ----
TEST(StepRows, LocalDayIsDaysFromCivilOfLocalDate) {
    const time::UnixSeconds t = utc(kY, 6, 10, 22, 30); // 00:30 on June 11 in Berlin (CEST)
    Rig utc_rig;
    Rig berlin(test::kBerlin);
    (void)utc_rig.read(t, 10);
    (void)berlin.read(t, 10);
    EXPECT_EQ(utc_rig.state.today_day, day_num(kY, 6, 10));
    EXPECT_EQ(berlin.state.today_day, day_num(kY, 6, 11));
    EXPECT_EQ(berlin.state.today_valid, 1U);
}

// ---- Row: day > today_day -> push history, today = 0, flush, delta placement ----
TEST(StepRows, NewDayPushesHistoryResetsTodayAndRequestsFlush) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 18), 100);
    const StepUpdate u = r.walk(utc(kY, 6, 11, 10), 50);
    EXPECT_TRUE(u.rolled_over);
    EXPECT_TRUE(u.flush_due);
    EXPECT_FALSE(u.time_went_back);
    ASSERT_EQ(r.state.history_count, 1U);
    EXPECT_EQ(r.state.history[0].day, day_num(kY, 6, 10));
    EXPECT_EQ(r.state.history[0].steps, 100U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 11));
    EXPECT_EQ(r.state.today, 50U); // last read hours before midnight: delta -> new day
    EXPECT_EQ(r.state.last_flush_day, day_num(kY, 6, 11));
    const StepUpdate next = r.walk(utc(kY, 6, 11, 10, 1), 5);
    EXPECT_FALSE(next.rolled_over);
    EXPECT_FALSE(next.flush_due);
}

TEST(StepRows, DeltaGoesToPreviousDayWhenLastReadWasWithinTwoMinutesBeforeBoundary) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 58), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59), 100);
    const StepUpdate u = r.walk(utc(kY, 6, 11, 0, 0, 30), 7);
    EXPECT_TRUE(u.rolled_over);
    EXPECT_EQ(u.delta, 7U);
    EXPECT_EQ(r.state.history[0].day, day_num(kY, 6, 10));
    EXPECT_EQ(r.state.history[0].steps, 107U);
    EXPECT_EQ(r.state.today, 0U);
}

TEST(StepRows, TwoMinuteGapStillBelongsToPreviousDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 59), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59, 30), 100);
    (void)r.walk(utc(kY, 6, 11, 0, 1, 30), 9); // exactly 120 s after the last read
    EXPECT_EQ(r.state.history[0].steps, 109U);
    EXPECT_EQ(r.state.today, 0U);
}

TEST(StepRows, GapOverTwoMinutesGoesToNewDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 59), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59, 30), 100);
    (void)r.walk(utc(kY, 6, 11, 0, 1, 31), 9); // 121 s
    EXPECT_EQ(r.state.history[0].steps, 100U);
    EXPECT_EQ(r.state.today, 9U);
}

TEST(StepRows, SkippedDaysAreFilledWithZero) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 18), 100);
    const StepUpdate u = r.walk(utc(kY, 6, 14, 8), 30); // three days of sleep
    EXPECT_TRUE(u.rolled_over);
    ASSERT_EQ(r.state.history_count, 4U);
    EXPECT_EQ(r.state.history[0].day, day_num(kY, 6, 13));
    EXPECT_EQ(r.state.history[1].day, day_num(kY, 6, 12));
    EXPECT_EQ(r.state.history[2].day, day_num(kY, 6, 11));
    EXPECT_EQ(r.state.history[3].day, day_num(kY, 6, 10));
    EXPECT_EQ(r.state.history[0].steps, 0U);
    EXPECT_EQ(r.state.history[1].steps, 0U);
    EXPECT_EQ(r.state.history[2].steps, 0U);
    EXPECT_EQ(r.state.history[3].steps, 100U);
    EXPECT_EQ(r.state.today, 30U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 14));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(StepRows, GapLongerThanHistoryKeepsOnlyTheNewestSevenZeroDays) {
    Rig r;
    (void)r.walk(utc(kY, 3, 1, 9), 0);
    (void)r.walk(utc(kY, 3, 1, 18), 100);
    (void)r.walk(utc(kY, 6, 10, 8), 4);
    ASSERT_EQ(r.state.history_count, model::kStepHistoryDays);
    EXPECT_EQ(r.state.history[0].day, day_num(kY, 6, 9));
    EXPECT_EQ(r.state.history[6].day, day_num(kY, 6, 3));
    for (const auto& e : r.state.history) {
        EXPECT_EQ(e.steps, 0U);
    }
    EXPECT_FALSE(history_steps(r.state, day_num(kY, 3, 1)).has_value());
    EXPECT_EQ(r.state.today, 4U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(StepRows, HistoryIsNewestFirstAndCappedAtSevenDays) {
    Rig r;
    (void)r.walk(utc(kY, 6, 1, 8), 0);
    for (std::uint8_t d = 1; d <= 10; ++d) {
        (void)r.walk(utc(kY, 6, d, 12), d * 10U);
    }
    (void)r.walk(utc(kY, 6, 11, 12), 0);
    ASSERT_EQ(r.state.history_count, 7U);
    for (std::uint8_t i = 0; i < 7; ++i) {
        const auto d = static_cast<std::uint8_t>(10 - i);
        EXPECT_EQ(r.state.history[i].day, day_num(kY, 6, d));
        EXPECT_EQ(r.state.history[i].steps, d * 10U);
    }
    const model::StepsSummary s = r.tracker.summary(8000);
    EXPECT_EQ(s.history_count, 7U);
    EXPECT_EQ(s.history[0].day, day_num(kY, 6, 10));
    EXPECT_EQ(s.history[6].day, day_num(kY, 6, 4));
    EXPECT_EQ(s.goal, 8000U);
}

// ---- Row: day < today_day -> no rollback, log time_back ----
TEST(StepRows, ClockMovedBackKeepsCountingIntoTodayDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 50), 0);
    (void)r.walk(utc(kY, 6, 11, 0, 5), 60);
    ASSERT_EQ(r.state.today_day, day_num(kY, 6, 11));
    const StepUpdate u = r.walk(utc(kY, 6, 10, 23, 40), 15); // manual set back across midnight
    EXPECT_TRUE(u.time_went_back);
    EXPECT_FALSE(u.rolled_over);
    EXPECT_FALSE(u.flush_due);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 11));
    EXPECT_EQ(r.state.today, 75U);
    EXPECT_EQ(r.state.history_count, 1U);
    // The clock reaches June 11 again: still the same day, no second rollover, no second flush.
    const StepUpdate again = r.walk(utc(kY, 6, 11, 0, 5), 3);
    EXPECT_FALSE(again.rolled_over);
    EXPECT_FALSE(again.flush_due);
    EXPECT_FALSE(again.time_went_back);
    EXPECT_EQ(r.state.today, 78U);
    EXPECT_EQ(r.state.history_count, 1U);
}

// ---- Row: time invalid -> pending; first valid time moves pending to today ----
TEST(StepRows, InvalidTimeAccumulatesPendingAndSampleMovesItToToday) {
    Rig r;
    (void)r.walk_no_time(0);
    const StepUpdate a = r.walk_no_time(30);
    const StepUpdate b = r.walk_no_time(20);
    EXPECT_EQ(a.delta, 30U);
    EXPECT_FALSE(a.rolled_over);
    EXPECT_FALSE(b.flush_due);
    EXPECT_EQ(r.state.pending, 50U);
    EXPECT_EQ(r.state.today, 0U);
    EXPECT_EQ(r.state.today_valid, 0U);
    EXPECT_EQ(r.tracker.summary(0).today, 0U);

    (void)r.walk(utc(kY, 6, 10, 12), 5);
    EXPECT_EQ(r.state.pending, 0U);
    EXPECT_EQ(r.state.today, 55U);
    EXPECT_EQ(r.state.today_valid, 1U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 10));
    EXPECT_EQ(r.state.history_count, 0U);
}

TEST(StepRows, InvalidTimeBecomingValidViaTimeJumpMovesPendingToToday) {
    Rig r;
    (void)r.walk_no_time(0);
    (void)r.walk_no_time(42);
    const StepUpdate u = r.jump(utc(kY, 6, 10, 12));
    EXPECT_FALSE(u.rolled_over);
    EXPECT_EQ(r.state.pending, 0U);
    EXPECT_EQ(r.state.today, 42U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 10));
}

TEST(StepRows, TimeJumpToInvalidChangesNothing) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 10), 10);
    const StepState before = r.state;
    const StepUpdate u = r.tracker.on_time_jump(std::nullopt, 0);
    EXPECT_FALSE(u.rolled_over || u.flush_due || u.time_went_back || u.goal_reached_now);
    EXPECT_EQ(r.state.today, before.today);
    EXPECT_EQ(r.state.last_read_utc, before.last_read_utc);
}

TEST(StepRows, PendingFromAnInvalidPeriodGoesToTheNewDayAfterRollover) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 18), 100);
    (void)r.walk_no_time(25); // clock lost during the night
    const StepUpdate u = r.walk(utc(kY, 6, 11, 7), 5);
    EXPECT_TRUE(u.rolled_over);
    EXPECT_EQ(r.state.history[0].steps, 100U);
    EXPECT_EQ(r.state.today, 30U);
    EXPECT_EQ(r.state.pending, 0U);
}

// ---- Row: daily goal ----
TEST(StepRows, GoalCrossedIsReportedOnceAndRearmsNextDay) {
    Rig r;
    r.goal = 100;
    (void)r.walk(utc(kY, 6, 10, 8), 0);
    EXPECT_FALSE(r.walk(utc(kY, 6, 10, 9), 60).goal_reached_now);
    EXPECT_TRUE(r.walk(utc(kY, 6, 10, 10), 45).goal_reached_now);
    EXPECT_EQ(r.state.goal_notified, 1U);
    EXPECT_FALSE(r.walk(utc(kY, 6, 10, 11), 500).goal_reached_now);
    EXPECT_FALSE(r.walk(utc(kY, 6, 11, 8), 10).goal_reached_now);
    EXPECT_EQ(r.state.goal_notified, 0U);
    EXPECT_TRUE(r.walk(utc(kY, 6, 11, 9), 95).goal_reached_now);
}

TEST(StepRows, GoalZeroNeverNotifies) {
    Rig r;
    r.goal = 0;
    (void)r.walk(utc(kY, 6, 10, 8), 0);
    EXPECT_FALSE(r.walk(utc(kY, 6, 10, 9), 90'000).goal_reached_now);
    EXPECT_EQ(r.tracker.summary(0).goal, 0U);
}

TEST(StepRows, GoalCrossedWithPendingStepsFiresWhenTimeBecomesValid) {
    Rig r;
    r.goal = 50;
    (void)r.walk_no_time(0);
    (void)r.walk_no_time(80);
    const StepUpdate j = r.jump(utc(kY, 6, 10, 12)); // jump has no goal parameter
    EXPECT_FALSE(j.goal_reached_now);
    EXPECT_TRUE(r.walk(utc(kY, 6, 10, 12, 1), 1).goal_reached_now);
}

TEST(StepRows, GoalAtDayStartDoesNotCountPreviousDaySteps) {
    Rig r;
    r.goal = 100;
    (void)r.walk(utc(kY, 6, 10, 23, 58), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59), 90);
    // Ten steps walked before midnight complete the previous day's total, not today's goal.
    EXPECT_FALSE(r.walk(utc(kY, 6, 11, 0, 0, 30), 10).goal_reached_now);
}

// ---- helpers: summary / inject / reset ----
TEST(StepApi, SummaryMirrorsStateAndExcludesPending) {
    Rig r;
    (void)r.walk_no_time(0);
    (void)r.walk_no_time(10);
    (void)r.walk(utc(kY, 6, 10, 9), 5);
    (void)r.walk(utc(kY, 6, 11, 9), 7);
    const model::StepsSummary s = r.tracker.summary(10'000);
    EXPECT_EQ(s.today, 7U);
    EXPECT_EQ(s.goal, 10'000U);
    EXPECT_EQ(s.history_count, 1U);
    EXPECT_EQ(s.history[0].steps, 15U);
}

TEST(StepApi, InjectAddsAndClampsAtZero) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    r.tracker.inject(500);
    EXPECT_EQ(r.state.today, 500U);
    r.tracker.inject(-200);
    EXPECT_EQ(r.state.today, 300U);
    r.tracker.inject(-100'000);
    EXPECT_EQ(r.state.today, 0U);
    r.tracker.inject(std::numeric_limits<std::int32_t>::max());
    r.tracker.inject(std::numeric_limits<std::int32_t>::max());
    r.tracker.inject(std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(r.state.today, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(r.state.last_hw_count, 0U); // the hardware baseline is untouched
}

TEST(StepApi, ResetTodayClearsTodayPendingAndGoalFlagButKeepsHistoryAndBaseline) {
    Rig r;
    r.goal = 10;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 10), 20);
    (void)r.walk(utc(kY, 6, 11, 10), 20);
    (void)r.walk_no_time(5);
    r.tracker.reset_today();
    EXPECT_EQ(r.state.today, 0U);
    EXPECT_EQ(r.state.pending, 0U);
    EXPECT_EQ(r.state.goal_notified, 0U);
    EXPECT_EQ(r.state.history_count, 1U);
    EXPECT_EQ(r.state.has_baseline, 1U);
}

TEST(StepApi, ResetRestoresPristineState) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 10);
    (void)r.walk(utc(kY, 6, 11, 9), 10);
    StepTracker::reset(r.state);
    const StepState pristine{};
    EXPECT_EQ(r.state.today, pristine.today);
    EXPECT_EQ(r.state.history_count, 0U);
    EXPECT_EQ(r.state.has_baseline, 0U);
    EXPECT_EQ(r.state.today_valid, 0U);
    EXPECT_EQ(r.state.last_read_utc, 0);
}

TEST(StepApi, CountersSaturateInsteadOfWrapping) {
    Rig r;
    constexpr std::uint32_t kMax = std::numeric_limits<std::uint32_t>::max();
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    r.tracker.inject(std::numeric_limits<std::int32_t>::max());
    r.tracker.inject(std::numeric_limits<std::int32_t>::max());
    r.tracker.inject(std::numeric_limits<std::int32_t>::max());
    (void)r.walk(utc(kY, 6, 10, 10), 100);
    EXPECT_EQ(r.state.today, kMax);
}

} // namespace
} // namespace qz::steps
