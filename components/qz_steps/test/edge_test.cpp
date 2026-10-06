// Virtual-time edge cases: DST in both hemispheres, midnight transitions, SNTP/manual time jumps,
// long sleeps and the once-per-day flush rule.
#include "qz/steps/step_tracker.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace qz::steps {
namespace {

using test::day_num;
using test::Rig;
using test::utc;

constexpr std::int32_t kY = 2026;

/// Local midnight (first instant of the local day), independent of the tracker.
time::UnixSeconds day_start(const time::TimeZone& tz, time::DayNumber day) {
    const auto r =
        tz.to_utc(time::civil_from_days(day), time::CivilTime{0, 0, 0}, time::GapPolicy::kEarlier);
    EXPECT_TRUE(r.has_value());
    return r ? *r : 0;
}

/// One step per minute from local noon of `first` for `days` days. Every fully covered local day
/// must hold exactly one step per minute of its real length (23/24/25 h): the sample taken at the
/// boundary instant belongs to the day that just ended.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void expect_each_day_has_one_step_per_minute(std::string_view posix,
                                             time::CivilDate first,
                                             std::int32_t days) {
    Rig r(posix);
    const time::DayNumber d0 = time::days_from_civil(first);
    const time::UnixSeconds start = day_start(r.tz, d0) + (12 * test::kHourS);
    const time::UnixSeconds end = day_start(r.tz, d0 + days) + (12 * test::kHourS);
    (void)r.walk(start, 0);
    std::uint32_t rollovers = 0;
    for (time::UnixSeconds t = start + 60; t <= end; t += 60) {
        const StepUpdate u = r.walk(t, 1);
        rollovers += u.rolled_over ? 1U : 0U;
        EXPECT_FALSE(u.time_went_back) << posix << " t=" << t;
    }
    EXPECT_EQ(rollovers, static_cast<std::uint32_t>(days));
    for (time::DayNumber d = d0 + 1; d < d0 + days; ++d) {
        const std::int64_t minutes = (day_start(r.tz, d + 1) - day_start(r.tz, d)) / 60;
        EXPECT_EQ(test::steps_on(r.state, d), static_cast<std::uint32_t>(minutes))
            << posix << " day " << d;
    }
}

TEST(StepDst, BerlinSpringForwardDayHas23Hours) {
    expect_each_day_has_one_step_per_minute(test::kBerlin, {kY, 3, 27}, 5);
}
TEST(StepDst, BerlinFallBackDayHas25Hours) {
    expect_each_day_has_one_step_per_minute(test::kBerlin, {kY, 10, 23}, 5);
}
TEST(StepDst, SydneySpringForwardDayHas23Hours) {
    expect_each_day_has_one_step_per_minute(test::kSydney, {kY, 10, 2}, 5);
}
TEST(StepDst, SydneyFallBackDayHas25Hours) {
    expect_each_day_has_one_step_per_minute(test::kSydney, {kY, 4, 3}, 5);
}
TEST(StepDst, SantiagoSpringForwardAtMidnightSkipsTheMidnightHour) {
    expect_each_day_has_one_step_per_minute(test::kSantiago, {kY, 9, 4}, 5);
}
TEST(StepDst, SantiagoFallBackAtMidnightRepeatsTheLastHourOfTheDay) {
    expect_each_day_has_one_step_per_minute(test::kSantiago, {kY, 4, 2}, 5);
}
TEST(StepDst, HavanaSpringForwardAtMidnight) {
    expect_each_day_has_one_step_per_minute(test::kHavana, {kY, 3, 6}, 5);
}
TEST(StepDst, HavanaFallBackJustAfterMidnight) {
    expect_each_day_has_one_step_per_minute(test::kHavana, {kY, 10, 30}, 5);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(StepDst, WallClockJumpingBackAcrossMidnightNeverRollsBack) {
    Rig r(test::kMidnightBack);
    // 2026-11-01 DST ends at 00:30 DST; the wall clock then reads 23:30 on Oct 31 again.
    const time::UnixSeconds start = utc(kY, 11, 1, 3, 0); // 23:00 Oct 31 local (UTC-4)
    const time::UnixSeconds end = utc(kY, 11, 2, 20, 0);
    (void)r.walk(start, 0);
    std::int64_t t1 = 0;
    std::int64_t t2 = 0;
    std::uint32_t back_samples = 0;
    std::uint32_t rollovers = 0;
    for (time::UnixSeconds t = start + 60; t <= end; t += 60) {
        const StepUpdate u = r.walk(t, 1);
        if (u.rolled_over) {
            ++rollovers;
            (rollovers == 1 ? t1 : t2) = t;
        }
        back_samples += u.time_went_back ? 1U : 0U;
    }
    ASSERT_EQ(rollovers,
              2U); // Oct 31 -> Nov 1, Nov 1 -> Nov 2 (and none for the repeated half hour)
    EXPECT_GT(back_samples, 0U);
    EXPECT_EQ(back_samples, 30U); // 23:30 .. 24:00 standard time is counted into Nov 1
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 10, 31)),
              static_cast<std::uint32_t>((t1 - start) / 60));
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 11, 1)),
              static_cast<std::uint32_t>((t2 - t1) / 60));
    // No duplicate day entries.
    for (std::size_t i = 1; i < r.state.history_count; ++i) {
        EXPECT_LT(r.state.history[i].day, r.state.history[i - 1].day);
    }
}

// ---- SNTP / manual time jumps ----
TEST(StepJump, PlusOneMinuteAcrossMidnightSampleOnlyBooksStepsToPreviousDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 58, 0), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59, 0), 100);
    // The clock is stepped +60 s between two samples: wall gap 120 s, still the previous day.
    const StepUpdate u = r.walk(utc(kY, 6, 11, 0, 1, 0), 3);
    EXPECT_TRUE(u.rolled_over);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 103U);
    EXPECT_EQ(r.state.today, 0U);
    EXPECT_EQ(r.state.history_count, 1U);
}

TEST(StepJump, PlusOneMinuteAcrossMidnightWithTimeJumpEventRollsOnce) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 59, 30), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59, 40), 100);
    const StepUpdate j = r.jump(utc(kY, 6, 11, 0, 0, 40)); // SNTP stepped the clock +60 s
    EXPECT_TRUE(j.rolled_over);
    EXPECT_TRUE(j.flush_due);
    EXPECT_EQ(j.delta, 0U);
    const StepUpdate s = r.walk(utc(kY, 6, 11, 0, 1, 40), 4);
    EXPECT_FALSE(s.rolled_over);
    EXPECT_EQ(r.state.today, 4U);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 100U);
    EXPECT_EQ(r.state.history_count, 1U);
}

TEST(StepJump, MinusOneMinuteAcrossMidnightDoesNotDuplicateTheDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 59), 0);
    (void)r.walk(utc(kY, 6, 11, 0, 0, 20), 40); // midnight passed
    ASSERT_EQ(r.state.today_day, day_num(kY, 6, 11));
    const StepUpdate j = r.jump(utc(kY, 6, 10, 23, 59, 20)); // SNTP steps back 61 s
    EXPECT_TRUE(j.time_went_back);
    EXPECT_FALSE(j.rolled_over);
    EXPECT_FALSE(j.flush_due);
    (void)r.walk(utc(kY, 6, 10, 23, 59, 50), 5); // still counted into June 11
    const StepUpdate again = r.walk(utc(kY, 6, 11, 0, 0, 50), 5);
    EXPECT_FALSE(again.rolled_over);
    EXPECT_FALSE(again.flush_due);
    EXPECT_EQ(r.state.history_count, 1U);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 40U);
    EXPECT_EQ(r.state.today, 10U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 11));
}

TEST(StepJump, PlusOneDayJumpRollsOverWithoutZeroFill) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 12), 0);
    (void)r.walk(utc(kY, 6, 10, 12, 30), 70);
    const StepUpdate j = r.jump(utc(kY, 6, 11, 12, 30));
    EXPECT_TRUE(j.rolled_over);
    EXPECT_TRUE(j.flush_due);
    ASSERT_EQ(r.state.history_count, 1U);
    EXPECT_EQ(r.state.history[0].steps, 70U);
    EXPECT_EQ(r.state.today, 0U);
    const StepUpdate s = r.walk(utc(kY, 6, 11, 12, 31), 6);
    EXPECT_EQ(r.state.today, 6U);
    EXPECT_FALSE(s.rolled_over);
}

TEST(StepJump, PlusOneDayJumpAtMidnightDoesNotAttributeToPreviousDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 23, 59), 0);
    (void)r.walk(utc(kY, 6, 10, 23, 59, 30), 100);
    (void)r.walk(utc(kY, 6, 12, 0, 0, 10), 8); // wall clock now a day and a bit later
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 100U);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 11)), 0U);
    EXPECT_EQ(r.state.today, 8U);
}

TEST(StepJump, MinusOneDayJumpKeepsCountingThenRollsOnlyWhenPastTodayDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 11, 9), 10); // today_day = June 11
    ASSERT_EQ(r.state.history_count, 1U);
    const StepUpdate j = r.jump(utc(kY, 6, 10, 9, 1)); // manual set back a day
    EXPECT_TRUE(j.time_went_back);
    (void)r.walk(utc(kY, 6, 10, 15), 20);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 11));
    EXPECT_EQ(r.state.today, 30U);
    (void)r.walk(utc(kY, 6, 11, 15), 5); // clock reaches June 11 again: not a new day
    EXPECT_EQ(r.state.today, 35U);
    EXPECT_EQ(r.state.history_count, 1U);
    const StepUpdate next = r.walk(utc(kY, 6, 12, 9), 1);
    EXPECT_TRUE(next.rolled_over);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 11)), 35U);
    EXPECT_EQ(r.state.today, 1U);
}

TEST(StepJump, ManualSetForwardSeveralDaysFillsZeros) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 10), 12);
    (void)r.jump(utc(kY, 6, 13, 10));
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 12U);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 11)), 0U);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 12)), 0U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 13));
}

TEST(StepJump, TimezoneChangeMovingTheLocalDayIsHandledLikeAJump) {
    Rig r("UTC0");
    (void)r.walk(utc(kY, 6, 10, 20), 0);
    (void)r.walk(utc(kY, 6, 10, 21), 50);
    r.tz = test::tz_of("<+05>-5"); // user selects UTC+5: it is now 02:00 on June 11
    const StepUpdate j = r.jump(utc(kY, 6, 10, 21));
    EXPECT_TRUE(j.rolled_over);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 50U);
    EXPECT_EQ(r.state.today_day, day_num(kY, 6, 11));
}

// ---- multi-day sleep (Critical), sensor reset ----
TEST(StepSleep, MultiDaySleepAttributesAllStepsToTheWakeDay) {
    Rig r(test::kBerlin);
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 21), 3000);
    const StepUpdate u = r.walk(utc(kY, 6, 14, 7), 2500);
    EXPECT_TRUE(u.rolled_over);
    EXPECT_EQ(r.state.today, 2500U);
    ASSERT_EQ(r.state.history_count, 4U);
    EXPECT_EQ(r.state.history[3].steps, 3000U);
    EXPECT_EQ(r.state.history[0].day, day_num(kY, 6, 13));
}

TEST(StepSleep, SensorResetAcrossMidnightStillRollsAndKeepsCounting) {
    Rig r;
    (void)r.read(utc(kY, 6, 10, 23, 58), 9000);
    (void)r.read(utc(kY, 6, 10, 23, 59), 9050);
    const StepUpdate u = r.read(utc(kY, 6, 11, 0, 0, 30), 12); // power loss at midnight
    EXPECT_TRUE(u.rolled_over);
    EXPECT_EQ(u.delta, 12U);
    EXPECT_EQ(test::steps_on(r.state, day_num(kY, 6, 10)), 62U);
    EXPECT_EQ(r.read(utc(kY, 6, 11, 0, 1, 30), 30).delta, 18U);
    EXPECT_EQ(r.state.today, 18U);
}

// ---- once-per-day flush ----
TEST(StepFlush, AppWritingOnFlushDueUsesAtMostTwoWritesPerLocalDay) {
    Rig r(test::kBerlin);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    constexpr int kDays = 8;
    const time::UnixSeconds start = utc(kY, 6, 10, 0, 30);
    (void)r.walk(start, 0);
    std::uint32_t flushes = 0;
    for (time::UnixSeconds t = start + 60; t <= start + (kDays * test::kDayS); t += 60) {
        const StepUpdate u = r.walk(t, 1);
        if (u.flush_due) {
            ASSERT_TRUE(store.save(r.state));
            ++flushes;
        }
    }
    EXPECT_EQ(flushes, static_cast<std::uint32_t>(kDays));
    // hist + today per flush, plus `ver` once: no NVS access on the minute path.
    EXPECT_LE(kv.write_count(), (2U * kDays) + 1U);
    EXPECT_EQ(kv.commit_count(), static_cast<std::uint32_t>(kDays));
}

TEST(StepFlush, TimeJumpingBackAndForthInOneDayFlushesOnce) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    std::uint32_t flushes = 0;
    flushes += r.walk(utc(kY, 6, 11, 9), 5).flush_due ? 1U : 0U;
    flushes += r.jump(utc(kY, 6, 10, 9)).flush_due ? 1U : 0U;
    flushes += r.jump(utc(kY, 6, 11, 9)).flush_due ? 1U : 0U;
    flushes += r.walk(utc(kY, 6, 11, 10), 5).flush_due ? 1U : 0U;
    EXPECT_EQ(flushes, 1U);
}

TEST(StepFlush, MinutePathNeverReportsFlushWithinTheSameDay) {
    Rig r;
    (void)r.walk(utc(kY, 6, 10, 0, 0), 0);
    for (int minute = 1; minute < 1440; ++minute) {
        const StepUpdate u = r.walk(utc(kY, 6, 10, 0, 0) + (minute * test::kMinuteS), 1);
        EXPECT_FALSE(u.flush_due) << minute;
        EXPECT_FALSE(u.rolled_over) << minute;
    }
}

} // namespace
} // namespace qz::steps
