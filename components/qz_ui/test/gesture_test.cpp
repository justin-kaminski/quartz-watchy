// GestureRecognizer timing table (WP-14): click / hold / repeat / debounce / seeded wake press.
#include "qz/ui/ui.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace qz::ui {
namespace {

// gtest macros inflate the cognitive-complexity score of table-driven checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)

using model::Button;
using model::InputEvent;
using model::InputKind;

constexpr std::uint8_t kMenu = 1U << 0U;
constexpr std::uint8_t kBack = 1U << 1U;
constexpr std::uint8_t kUp = 1U << 2U;
constexpr std::uint8_t kDown = 1U << 3U;

constexpr std::int64_t us(std::int64_t ms) {
    return ms * 1000;
}

struct Rec {
    Rec() = default;
    explicit Rec(GestureTiming t) : r(t) {}
    GestureRecognizer r;
    std::vector<InputEvent> all;

    /// Samples at `ms`; returns just this call's events (also appended to `all`).
    std::vector<InputEvent> at(std::uint8_t mask, std::int64_t ms) {
        StaticVector<InputEvent, 8> out;
        r.sample(mask, us(ms), out);
        std::vector<InputEvent> v(out.begin(), out.end());
        all.insert(all.end(), v.begin(), v.end());
        return v;
    }
};

void expect_event(
    const InputEvent& e, Button b, InputKind k, std::uint32_t held_ms, std::int64_t t_ms) {
    EXPECT_EQ(e.button, b);
    EXPECT_EQ(e.kind, k);
    EXPECT_EQ(e.held_ms, held_ms);
    EXPECT_EQ(e.t_us, us(t_ms));
}

TEST(Gesture, ShortPressIsClickAfterReleaseDebounce) {
    Rec g;
    EXPECT_TRUE(g.at(kMenu, 0).empty());
    EXPECT_TRUE(g.at(kMenu, 25).empty()); // press accepted, nothing to report yet
    EXPECT_TRUE(g.at(0, 100).empty());    // release not yet debounced
    const auto ev = g.at(0, 125);
    ASSERT_EQ(ev.size(), 1U);
    expect_event(ev[0], Button::kMenu, InputKind::kClick, 100, 100);
}

TEST(Gesture, ClickJustBelowHoldThreshold) {
    Rec g;
    (void)g.at(kBack, 0);
    (void)g.at(kBack, 30);
    (void)g.at(0, 699);
    const auto ev = g.at(0, 730);
    ASSERT_EQ(ev.size(), 1U);
    expect_event(ev[0], Button::kBack, InputKind::kClick, 699, 699);
}

TEST(Gesture, HoldAtThresholdAndNoClickOnRelease) {
    Rec g;
    (void)g.at(kMenu, 0);
    (void)g.at(kMenu, 30);
    EXPECT_TRUE(g.at(kMenu, 699).empty());
    const auto ev = g.at(kMenu, 700);
    ASSERT_EQ(ev.size(), 1U);
    expect_event(ev[0], Button::kMenu, InputKind::kHold, 700, 700);
    (void)g.at(0, 900);
    EXPECT_TRUE(g.at(0, 930).empty()); // release after a Hold is silent
    EXPECT_EQ(g.all.size(), 1U);
}

TEST(Gesture, ReleaseExactlyAtThresholdCountsAsHold) {
    Rec g;
    (void)g.at(kDown, 0);
    (void)g.at(kDown, 30);
    const auto at_release = g.at(0, 700); // raw release at 700 ms, still debouncing
    ASSERT_EQ(at_release.size(), 1U);
    expect_event(at_release[0], Button::kDown, InputKind::kHold, 700, 700);
    EXPECT_TRUE(g.at(0, 730).empty());
}

TEST(Gesture, RepeatEvery150msAfterHoldForUpAndDown) {
    Rec g;
    (void)g.at(kUp, 0);
    (void)g.at(kUp, 30);
    const auto ev = g.at(kUp, 1000);
    ASSERT_EQ(ev.size(), 3U);
    expect_event(ev[0], Button::kUp, InputKind::kHold, 700, 700);
    expect_event(ev[1], Button::kUp, InputKind::kRepeat, 850, 850);
    expect_event(ev[2], Button::kUp, InputKind::kRepeat, 1000, 1000);
    EXPECT_TRUE(g.at(kUp, 1149).empty());
    ASSERT_EQ(g.at(kUp, 1150).size(), 1U);
}

TEST(Gesture, EventsDoNotDependOnSamplingCadence) {
    Rec coarse;
    Rec fine;
    (void)coarse.at(kDown, 0);
    (void)coarse.at(kDown, 30);
    (void)coarse.at(kDown, 2000);
    (void)coarse.at(kDown, 2000); // the first call fills the 8-event buffer
    (void)fine.at(kDown, 0);
    for (std::int64_t t = 10; t <= 2000; t += 10) {
        (void)fine.at(kDown, t);
    }
    ASSERT_EQ(coarse.all.size(), fine.all.size());
    for (std::size_t i = 0; i < coarse.all.size(); ++i) {
        EXPECT_EQ(coarse.all[i].kind, fine.all[i].kind) << i;
        EXPECT_EQ(coarse.all[i].held_ms, fine.all[i].held_ms) << i;
        EXPECT_EQ(coarse.all[i].t_us, fine.all[i].t_us) << i;
    }
    EXPECT_EQ(coarse.all.size(), 1U + ((2000U - 700U) / 150U));
}

TEST(Gesture, MenuAndBackDoNotRepeatButMenuReportsLongHoldOnce) {
    Rec g;
    (void)g.at(kMenu | kBack, 0); // BACK+MENU is not the reset chord
    (void)g.at(kMenu | kBack, 30);
    const auto early = g.at(kMenu | kBack, 1500);
    ASSERT_EQ(early.size(), 2U); // one Hold each, no repeats
    EXPECT_EQ(early[0].kind, InputKind::kHold);
    EXPECT_EQ(early[1].kind, InputKind::kHold);
    const auto long_hold = g.at(kMenu | kBack, 3000);
    ASSERT_EQ(long_hold.size(), 1U);
    expect_event(long_hold[0], Button::kMenu, InputKind::kRepeat, 3000, 3000);
    EXPECT_TRUE(g.at(kMenu | kBack, 6000).empty());
}

TEST(Gesture, DebounceSwallowsGlitchesAndUsesLastEdgeAsPressTime) {
    Rec glitch;
    EXPECT_TRUE(glitch.at(kUp, 0).empty());
    EXPECT_TRUE(glitch.at(0, 10).empty());
    EXPECT_TRUE(glitch.at(0, 60).empty());
    EXPECT_FALSE(glitch.r.any_pressed());
    EXPECT_EQ(glitch.r.next_deadline_us(), -1);
    EXPECT_TRUE(glitch.all.empty());

    Rec bounce;
    (void)bounce.at(kMenu, 0);
    (void)bounce.at(0, 10);
    (void)bounce.at(kMenu, 20); // press time becomes 20 ms
    (void)bounce.at(kMenu, 45);
    const auto hold = bounce.at(kMenu, 720);
    ASSERT_EQ(hold.size(), 1U);
    expect_event(hold[0], Button::kMenu, InputKind::kHold, 700, 720);
}

TEST(Gesture, ReleaseBounceIsIgnored) {
    Rec g;
    (void)g.at(kBack, 0);
    (void)g.at(kBack, 30);
    (void)g.at(0, 100);
    (void)g.at(kBack, 110); // bounced back down before the release settled
    EXPECT_TRUE(g.at(kBack, 140).empty());
    (void)g.at(0, 200);
    const auto ev = g.at(0, 230);
    ASSERT_EQ(ev.size(), 1U);
    expect_event(ev[0], Button::kBack, InputKind::kClick, 200, 200);
}

TEST(Gesture, SeededWakePressContinuesFromWakeTime) {
    Rec held;
    held.r.seed(kMenu, us(1000));
    EXPECT_TRUE(held.r.any_pressed());
    EXPECT_TRUE(held.at(kMenu, 1100).empty());
    const auto hold = held.at(kMenu, 1700);
    ASSERT_EQ(hold.size(), 1U);
    expect_event(hold[0], Button::kMenu, InputKind::kHold, 700, 1700);

    Rec late; // first sample long after the press: the Hold is reported with its nominal time
    late.r.seed(kMenu, us(0));
    const auto ev = late.at(kMenu, 900);
    ASSERT_EQ(ev.size(), 1U);
    expect_event(ev[0], Button::kMenu, InputKind::kHold, 700, 700);

    Rec quick; // released soon after waking: a Click, timed from the seeded press
    quick.r.seed(kUp, us(0));
    (void)quick.at(0, 200);
    const auto click = quick.at(0, 230);
    ASSERT_EQ(click.size(), 1U);
    expect_event(click[0], Button::kUp, InputKind::kClick, 200, 200);
}

TEST(Gesture, ButtonsAreIndependent) {
    Rec g;
    (void)g.at(kUp | kDown, 0);
    (void)g.at(kUp | kDown, 30);
    (void)g.at(kDown, 100); // UP released
    const auto ev = g.at(kDown, 130);
    ASSERT_EQ(ev.size(), 1U);
    expect_event(ev[0], Button::kUp, InputKind::kClick, 100, 100);
    const auto hold = g.at(kDown, 700);
    ASSERT_EQ(hold.size(), 1U);
    EXPECT_EQ(hold[0].button, Button::kDown);
}

TEST(Gesture, BackPlusUpChordIsNeverInterpreted) {
    Rec g;
    (void)g.at(kBack, 0);
    (void)g.at(kBack | kUp, 10);
    (void)g.at(kBack | kUp, 45);
    EXPECT_TRUE(g.at(kBack | kUp, 2000).empty()); // no Hold, no Repeat
    (void)g.at(0, 2500);
    EXPECT_TRUE(g.at(0, 2530).empty()); // no Click on release either
    EXPECT_TRUE(g.all.empty());
    // Afterwards both buttons work again.
    (void)g.at(kBack, 3000);
    (void)g.at(kBack, 3030);
    (void)g.at(0, 3100);
    const auto ev = g.at(0, 3130);
    ASSERT_EQ(ev.size(), 1U);
    EXPECT_EQ(ev[0].button, Button::kBack);
}

TEST(Gesture, SeededChordIsSuppressedToo) {
    Rec g;
    g.r.seed(kBack | kUp, us(0));
    EXPECT_TRUE(g.at(kBack | kUp, 2000).empty());
}

TEST(Gesture, NextDeadlineTracksDebounceHoldRepeatAndLongHold) {
    Rec g;
    EXPECT_EQ(g.r.next_deadline_us(), -1);
    (void)g.at(kUp, 0);
    EXPECT_EQ(g.r.next_deadline_us(), us(25)); // debounce expiry
    (void)g.at(kUp, 25);
    EXPECT_EQ(g.r.next_deadline_us(), us(700)); // hold
    (void)g.at(kUp, 700);
    EXPECT_EQ(g.r.next_deadline_us(), us(850)); // first repeat
    (void)g.at(kUp, 850);
    EXPECT_EQ(g.r.next_deadline_us(), us(1000));
    (void)g.at(0, 1100);
    EXPECT_EQ(g.r.next_deadline_us(), us(1125)); // release debounce
    (void)g.at(0, 1125);
    EXPECT_EQ(g.r.next_deadline_us(), -1);
    EXPECT_FALSE(g.r.any_pressed());

    Rec m;
    (void)m.at(kMenu, 0);
    (void)m.at(kMenu, 25);
    (void)m.at(kMenu, 700);
    EXPECT_EQ(m.r.next_deadline_us(), us(3000)); // MENU long hold
    (void)m.at(kMenu, 3000);
    EXPECT_EQ(m.r.next_deadline_us(), -1);

    Rec b;
    (void)b.at(kBack, 0);
    (void)b.at(kBack, 25);
    (void)b.at(kBack, 700);
    EXPECT_EQ(b.r.next_deadline_us(), -1); // BACK: nothing more to time
    EXPECT_TRUE(b.r.any_pressed());
}

TEST(Gesture, CustomTimingIsHonoured) {
    Rec g{GestureTiming{10, 300, 50, 1000}};
    (void)g.at(kUp, 0);
    const auto ev = g.at(kUp, 400);
    ASSERT_EQ(ev.size(), 3U);
    expect_event(ev[0], Button::kUp, InputKind::kHold, 300, 300);
    expect_event(ev[1], Button::kUp, InputKind::kRepeat, 350, 350);
    expect_event(ev[2], Button::kUp, InputKind::kRepeat, 400, 400);
}

TEST(Gesture, FullOutputBufferLosesNothing) {
    Rec g;
    (void)g.at(kUp, 0);
    (void)g.at(kUp, 30);
    std::size_t total = 0;
    std::int64_t last_t = -1;
    for (int round = 0; round < 10; ++round) {
        const auto ev = g.at(kUp, 5000);
        EXPECT_LE(ev.size(), 8U);
        for (const auto& e : ev) {
            EXPECT_GT(e.t_us, last_t);
            last_t = e.t_us;
        }
        total += ev.size();
    }
    EXPECT_EQ(total, 1U + ((5000U - 700U) / 150U));
    // The release is reported once the backlog is drained.
    (void)g.at(0, 5100);
    EXPECT_TRUE(g.at(0, 5200).empty());
    EXPECT_FALSE(g.r.any_pressed());
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::ui
