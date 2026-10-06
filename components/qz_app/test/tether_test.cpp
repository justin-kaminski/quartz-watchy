// TetherPolicy (ARCHITECTURE.md section 17, DECISIONS D-12): transition table and the invariant
// "no event sequence on battery creates the console or delays sleep".
#include "qz/app/app.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace qz::app {
namespace {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint32_t next() {
        s_ ^= s_ << 13U;
        s_ ^= s_ >> 7U;
        s_ ^= s_ << 17U;
        return static_cast<std::uint32_t>(s_ >> 16U);
    }

private:
    std::uint64_t s_;
};

TEST(Tether, StartsUntetheredAndGatesTheConsole) {
    const TetherPolicy t;
    EXPECT_EQ(t.state(), TetherState::kUntethered);
    EXPECT_FALSE(t.console_allowed());
    EXPECT_TRUE(t.may_deep_sleep());
}

TEST(Tether, WakeNeedsTwoPresentReads) {
    struct Case {
        bool first;
        bool second;
        TetherState expected;
    };
    for (const Case c : {Case{false, false, TetherState::kUntethered},
                         Case{true, false, TetherState::kUntethered},
                         Case{false, true, TetherState::kUntethered},
                         Case{true, true, TetherState::kTethered}}) {
        TetherPolicy t;
        EXPECT_EQ(t.on_wake(c.first, c.second), c.expected);
        EXPECT_EQ(t.state(), c.expected);
        EXPECT_EQ(t.console_allowed(), c.expected == TetherState::kTethered);
        EXPECT_EQ(t.may_deep_sleep(), c.expected != TetherState::kTethered);
    }
}

TEST(Tether, TetheredStaysAwakeAndAllowsTheConsole) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    EXPECT_TRUE(t.console_allowed());
    EXPECT_FALSE(t.may_deep_sleep());
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(t.on_poll(true), TetherState::kTethered);
    }
}

TEST(Tether, TwoConsecutiveAbsentPollsDetach) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    EXPECT_EQ(t.on_poll(false), TetherState::kTethered);
    EXPECT_TRUE(t.console_allowed());
    EXPECT_EQ(t.on_poll(false), TetherState::kDetaching);
    EXPECT_FALSE(t.console_allowed());
    EXPECT_TRUE(t.may_deep_sleep());
}

TEST(Tether, APresentPollResetsTheAbsentCount) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(t.on_poll(false), TetherState::kTethered); // one absent ...
        EXPECT_EQ(t.on_poll(true), TetherState::kTethered);  // ... then back: never detaches
    }
    EXPECT_EQ(t.on_poll(false), TetherState::kTethered);
    EXPECT_EQ(t.on_poll(false), TetherState::kDetaching);
}

TEST(Tether, DetachingIsStickyUntilDetachedEvenIfUsbReturns) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    (void)t.on_poll(false); // state is read back through state(); return value not needed
    (void)t.on_poll(false);
    ASSERT_EQ(t.state(), TetherState::kDetaching);
    EXPECT_EQ(t.on_poll(true), TetherState::kDetaching); // no re-tether without the wake debounce
    EXPECT_FALSE(t.console_allowed());
    t.on_detached();
    EXPECT_EQ(t.state(), TetherState::kUntethered);
    EXPECT_FALSE(t.console_allowed());
}

TEST(Tether, SleepOnceStopsTheConsoleAndAllowsDeepSleep) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    t.request_sleep();
    EXPECT_EQ(t.state(), TetherState::kSleepOnce);
    EXPECT_FALSE(t.console_allowed());
    EXPECT_TRUE(t.may_deep_sleep());
    EXPECT_EQ(t.on_poll(true), TetherState::kSleepOnce); // polls do not undo it
    EXPECT_EQ(t.on_poll(false), TetherState::kSleepOnce);
    t.on_detached();
    EXPECT_EQ(t.state(), TetherState::kUntethered);
}

TEST(Tether, NextWakeReevaluatesUsbAfterSleepOnce) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    t.request_sleep();
    EXPECT_EQ(t.on_wake(true, true), TetherState::kTethered); // still plugged: back to tethered
    t.request_sleep();
    EXPECT_EQ(t.on_wake(false, false), TetherState::kUntethered); // unplugged meanwhile
}

TEST(Tether, SleepRequestOnBatteryIsIgnored) {
    TetherPolicy t;
    t.request_sleep();
    EXPECT_EQ(t.state(), TetherState::kUntethered);
    ASSERT_EQ(t.on_wake(false, false), TetherState::kUntethered);
    t.request_sleep();
    EXPECT_EQ(t.state(), TetherState::kUntethered);
}

TEST(Tether, PollsNeverCreateTheConsoleWithoutTheWakeDebounce) {
    TetherPolicy t;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(t.on_poll(true), TetherState::kUntethered);
    }
    (void)t.on_wake(true, false);
    EXPECT_EQ(t.on_poll(true), TetherState::kUntethered);
    EXPECT_FALSE(t.console_allowed());
}

TEST(Tether, DetachedFromTetheredAlsoStopsTheConsole) {
    TetherPolicy t;
    ASSERT_EQ(t.on_wake(true, true), TetherState::kTethered);
    t.on_detached(); // e.g. the console port failed
    EXPECT_FALSE(t.console_allowed());
    EXPECT_TRUE(t.may_deep_sleep());
}

enum class Event : std::uint8_t {
    kWakeAbsentAbsent,
    kWakePresentAbsent,
    kWakeAbsentPresent,
    kWakePresentPresent,
    kPollAbsent,
    kPollPresent,
    kRequestSleep,
    kDetached,
    kEventCount
};
using enum Event;
constexpr std::uint32_t kEventTotal = static_cast<std::uint32_t>(Event::kEventCount);

void apply(TetherPolicy& t, Event e) {
    switch (e) {
        case kWakeAbsentAbsent:
            (void)t.on_wake(false, false);
            break;
        case kWakePresentAbsent:
            (void)t.on_wake(true, false);
            break;
        case kWakeAbsentPresent:
            (void)t.on_wake(false, true);
            break;
        case kWakePresentPresent:
            (void)t.on_wake(true, true);
            break;
        case kPollAbsent:
            (void)t.on_poll(false);
            break;
        case kPollPresent:
            (void)t.on_poll(true);
            break;
        case kRequestSleep:
            t.request_sleep();
            break;
        case kDetached:
            t.on_detached();
            break;
        case kEventCount:
            break;
    }
}

// Battery: the USB pin never reads present twice in a row. A single glitchy read (one of the two
// reads of on_wake) is allowed; polls on battery read absent (a present poll IS "USB present").
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TetherInvariant, NoEventSequenceOnBatteryAllowsTheConsoleOrKeepsTheWatchAwake) {
    Rng rng(0xC0FFEE);
    constexpr Event kBatteryEvents[] = {kWakeAbsentAbsent,
                                        kWakePresentAbsent,
                                        kWakeAbsentPresent,
                                        kPollAbsent,
                                        kRequestSleep,
                                        kDetached};
    for (int seq = 0; seq < 20000; ++seq) {
        TetherPolicy t;
        const int len = 1 + static_cast<int>(rng.next() % 64U);
        for (int i = 0; i < len; ++i) {
            const Event e = kBatteryEvents[rng.next() % std::size(kBatteryEvents)];
            apply(t, e);
            ASSERT_FALSE(t.console_allowed()) << "seq " << seq << " step " << i;
            ASSERT_TRUE(t.may_deep_sleep()) << "seq " << seq << " step " << i;
            ASSERT_EQ(t.state(), TetherState::kUntethered) << "seq " << seq << " step " << i;
        }
    }
}

// Whatever USB does: the console state is entered only by a wake with two present reads, and
// console_allowed() / may_deep_sleep() are exact complements of kTethered.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(TetherInvariant, ConsoleStateIsReachedOnlyThroughTheWakeDebounce) {
    Rng rng(0xBADC0DE);
    for (int seq = 0; seq < 20000; ++seq) {
        TetherPolicy t;
        const int len = 1 + static_cast<int>(rng.next() % 64U);
        for (int i = 0; i < len; ++i) {
            const TetherState before = t.state();
            const auto e = static_cast<Event>(rng.next() % kEventTotal);
            apply(t, e);
            const TetherState after = t.state();
            if (after == TetherState::kTethered && before != TetherState::kTethered) {
                ASSERT_EQ(e, kWakePresentPresent) << "seq " << seq << " step " << i;
            }
            ASSERT_EQ(t.console_allowed(), after == TetherState::kTethered);
            ASSERT_EQ(t.may_deep_sleep(), after != TetherState::kTethered);
            // A stuck-awake state needs USB: leaving kTethered never takes a present read.
            if (before == TetherState::kTethered && after != TetherState::kTethered) {
                ASSERT_NE(e, kPollPresent);
            }
        }
    }
}

// Tethered but the cable is pulled and nothing else happens: the watch always gets to sleep.
TEST(TetherInvariant, UnpluggingAlwaysEndsInSleep) {
    Rng rng(42);
    for (int seq = 0; seq < 2000; ++seq) {
        TetherPolicy t;
        (void)t.on_wake(true, true);
        const int plugged_polls = static_cast<int>(rng.next() % 20U);
        for (int i = 0; i < plugged_polls; ++i) {
            (void)t.on_poll(rng.next() % 3U != 0U); // flaky USB while plugged
        }
        (void)t.on_poll(false);
        (void)t.on_poll(false); // two clean absent polls in a row
        ASSERT_EQ(t.state(), TetherState::kDetaching) << "seq " << seq;
        ASSERT_TRUE(t.may_deep_sleep()) << "seq " << seq;
        ASSERT_FALSE(t.console_allowed());
    }
}

} // namespace
} // namespace qz::app
