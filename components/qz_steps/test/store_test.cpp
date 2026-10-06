// StepHistoryStore: round trips, compare-first wear policy, corruption handling, erase.
#include "qz/steps/step_tracker.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>

namespace qz::steps {
namespace {

using test::day_num;
using test::Rig;
using test::utc;

constexpr std::int32_t kY = 2026;
constexpr std::string_view kNs = "qz_steps";

std::optional<Errc> code_of(const Status& s) {
    return s ? std::nullopt : std::optional<Errc>(s.error().code);
}

/// Three days of history plus 321 steps today (June 13).
void fill(Rig& r) {
    (void)r.walk(utc(kY, 6, 10, 9), 0);
    (void)r.walk(utc(kY, 6, 10, 18), 1000);
    (void)r.walk(utc(kY, 6, 11, 18), 2000);
    (void)r.walk(utc(kY, 6, 12, 18), 3000);
    (void)r.walk(utc(kY, 6, 13, 18), 321);
}

TEST(StepStore, SaveThenLoadRestoresHistoryAndToday) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));

    StepState restored{};
    StepTracker::reset(restored);
    ASSERT_TRUE(store.load(restored));
    ASSERT_EQ(restored.history_count, 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(restored.history[i].day, r.state.history[i].day);
        EXPECT_EQ(restored.history[i].steps, r.state.history[i].steps);
    }
    EXPECT_EQ(restored.history[0].day, day_num(kY, 6, 12));
    EXPECT_EQ(restored.today_day, day_num(kY, 6, 13));
    EXPECT_EQ(restored.today, 321U);
    EXPECT_EQ(restored.today_valid, 1U);
    EXPECT_EQ(restored.last_flush_day, day_num(kY, 6, 13));
    EXPECT_EQ(restored.has_baseline, 0U); // the hardware baseline is not persisted
    EXPECT_EQ(restored.pending, 0U);
}

TEST(StepStore, RestoredStateContinuesCountingAndFillsMissedDays) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));

    Rig after;
    ASSERT_TRUE(store.load(after.state));                     // RTC memory lost, NVS kept
    EXPECT_EQ(after.read(utc(kY, 6, 13, 19), 777).delta, 0U); // new baseline
    EXPECT_EQ(after.read(utc(kY, 6, 13, 19, 1), 787).delta, 10U);
    EXPECT_EQ(after.state.today, 331U);
    const StepUpdate u = after.read(utc(kY, 6, 15, 8), 800); // two days later
    EXPECT_TRUE(u.rolled_over);
    EXPECT_EQ(after.state.history[0].day, day_num(kY, 6, 14));
    EXPECT_EQ(after.state.history[0].steps, 0U);
    EXPECT_EQ(after.state.history[1].day, day_num(kY, 6, 13));
    EXPECT_EQ(after.state.history[1].steps, 331U);
}

TEST(StepStore, FlushBeforeControlledRebootMidDayPersistsTodayWithoutADayRollover) {
    Rig r;
    fill(r);
    (void)r.walk(utc(kY, 6, 13, 19), 679); // today = 1000, no rollover flush involved
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state)); // reboot / Critical flush: caller invokes save directly
    StepState restored{};
    ASSERT_TRUE(store.load(restored));
    EXPECT_EQ(restored.today, 1000U);
    EXPECT_EQ(restored.history_count, 3U);
}

TEST(StepStore, UnchangedSaveWritesNothing) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    const std::uint32_t writes = kv.write_count();
    const std::uint32_t commits = kv.commit_count();
    EXPECT_EQ(writes, 3U); // ver + hist + today
    ASSERT_TRUE(store.save(r.state));
    EXPECT_EQ(kv.write_count(), writes);
    EXPECT_EQ(kv.commit_count(), commits);
    (void)r.walk(utc(kY, 6, 13, 19), 1); // today changes: only the today blob is rewritten
    ASSERT_TRUE(store.save(r.state));
    EXPECT_EQ(kv.write_count(), writes + 1U);
    EXPECT_EQ(kv.commit_count(), commits + 1U);
}

TEST(StepStore, SaveWithoutValidDayStoresOnlyHistoryBlob) {
    Rig r;
    (void)r.walk_no_time(0);
    (void)r.walk_no_time(10);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    EXPECT_EQ(kv.entry_count(kNs), 2U); // ver + hist
    StepState restored{};
    ASSERT_TRUE(store.load(restored));
    EXPECT_EQ(restored.today_valid, 0U);
    EXPECT_EQ(restored.history_count, 0U);
}

TEST(StepStore, LoadFromEmptyStoreIsNotFoundAndLeavesStateUntouched) {
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    StepState st{};
    st.today = 99;
    EXPECT_EQ(code_of(store.load(st)), Errc::kNotFound);
    EXPECT_EQ(st.today, 99U);
}

TEST(StepStore, WrongVersionOrBlobSizeIsCorruptAndLeavesStateUntouched) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    StepState st{};
    st.today = 99;

    ASSERT_TRUE(kv.set_u32(kNs, "ver", 77));
    EXPECT_EQ(code_of(store.load(st)), Errc::kCorrupt);
    ASSERT_TRUE(kv.set_u32(kNs, "ver", 1));

    const std::array<std::uint8_t, 5> junk{1, 2, 3, 4, 5};
    ASSERT_TRUE(kv.set_blob(kNs, "hist", junk));
    EXPECT_EQ(code_of(store.load(st)), Errc::kCorrupt);
    ASSERT_TRUE(store.save(r.state));
    ASSERT_TRUE(kv.set_blob(kNs, "today", junk));
    EXPECT_EQ(code_of(store.load(st)), Errc::kCorrupt);
    EXPECT_EQ(st.today, 99U);
    EXPECT_EQ(st.history_count, 0U);
}

TEST(StepStore, HistoryNotNewestFirstIsCorrupt) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    std::array<std::uint8_t, 56> blob{};
    ASSERT_TRUE(kv.get_blob(kNs, "hist", blob));
    for (std::size_t i = 0; i < 8; ++i) { // swap entries 0 and 1
        std::swap(blob[i], blob[8 + i]);
    }
    ASSERT_TRUE(kv.set_blob(kNs, "hist", blob));
    StepState st{};
    EXPECT_EQ(code_of(store.load(st)), Errc::kCorrupt);
}

TEST(StepStore, TodayNotAfterNewestHistoryDayIsCorrupt) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    std::array<std::uint8_t, 8> today{};
    ASSERT_TRUE(kv.get_blob(kNs, "today", today));
    today[0] = 1; // day 1 << newest history day
    today[1] = 0;
    today[2] = 0;
    today[3] = 0;
    ASSERT_TRUE(kv.set_blob(kNs, "today", today));
    StepState st{};
    EXPECT_EQ(code_of(store.load(st)), Errc::kCorrupt);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(StepStore, FullSevenDayHistoryRoundTrips) {
    Rig r;
    (void)r.walk(utc(kY, 6, 1, 8), 0);
    for (std::uint8_t d = 1; d <= 9; ++d) {
        (void)r.walk(utc(kY, 6, d, 12), d * 100U);
    }
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    StepState restored{};
    ASSERT_TRUE(store.load(restored));
    ASSERT_EQ(restored.history_count, 7U);
    for (std::size_t i = 0; i < 7; ++i) {
        EXPECT_EQ(restored.history[i].day, r.state.history[i].day);
        EXPECT_EQ(restored.history[i].steps, r.state.history[i].steps);
    }
}

TEST(StepStore, NegativeAndLargeValuesSurviveSerialization) {
    StepState st{};
    st.history_count = 2;
    st.history[0] = {-5, 0xFFFFFFFFU};
    st.history[1] = {-9, 0x80000001U};
    st.today_day = 100'000;
    st.today = 0x12345678U;
    st.today_valid = 1;
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(st));
    StepState back{};
    ASSERT_TRUE(store.load(back));
    EXPECT_EQ(back.history[0].day, -5);
    EXPECT_EQ(back.history[0].steps, 0xFFFFFFFFU);
    EXPECT_EQ(back.history[1].day, -9);
    EXPECT_EQ(back.history[1].steps, 0x80000001U);
    EXPECT_EQ(back.today, 0x12345678U);
    EXPECT_EQ(back.today_day, 100'000);
}

TEST(StepStore, EraseRemovesEverythingAndIsIdempotent) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    ASSERT_TRUE(store.erase());
    EXPECT_EQ(kv.entry_count(kNs), 0U);
    StepState st{};
    EXPECT_EQ(code_of(store.load(st)), Errc::kNotFound);
    ASSERT_TRUE(store.erase()); // already empty
}

TEST(StepStore, WriteFailureIsReported) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    StepHistoryStore store(kv);
    kv.fail_writes_after(1);
    EXPECT_EQ(code_of(store.save(r.state)), Errc::kIo);
}

TEST(StepStore, SaveDoesNotTouchOtherNamespaces) {
    Rig r;
    fill(r);
    testkit::FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32("qz_set", "goal", 8000));
    StepHistoryStore store(kv);
    ASSERT_TRUE(store.save(r.state));
    ASSERT_TRUE(store.erase());
    EXPECT_EQ(*kv.get_u32("qz_set", "goal"), 8000U);
}

} // namespace
} // namespace qz::steps
