// WakePlanner (qz/app/wake_planner.hpp): latency EWMA, minute-boundary alignment (virtual-time
// simulation with crystal error), cadences, wake sources, safe mode, RTC persistence.
#include "qz/app/wake_planner.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace qz::app {
namespace {

using hal::ResetReason;
using testkit::FakeSleepSystem;
using testkit::VirtualClock;

constexpr std::int64_t kUs = 1'000'000;
constexpr std::int64_t kMs = 1000;
constexpr std::int64_t kMin = 60 * kUs;
constexpr std::int64_t kT0 = 1'800'000'000LL * kUs;              // 2027-01-15 08:00:00Z
constexpr std::int64_t kUsSpringForward = 1'805'007'600LL * kUs; // 2027-03-14 07:00:00Z
constexpr std::int64_t kUsFallBack = 1'825'567'200LL * kUs;      // 2027-11-07 06:00:00Z

static_assert(kT0 % (5 * kMin) == 0 && kUsSpringForward % (5 * kMin) == 0 &&
              kUsFallBack % (5 * kMin) == 0);

class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        s_ ^= s_ << 13U;
        s_ ^= s_ >> 7U;
        s_ ^= s_ << 17U;
        return s_;
    }
    /// Uniform in [lo, hi].
    std::int64_t range(std::int64_t lo, std::int64_t hi) {
        return lo + static_cast<std::int64_t>(next() % static_cast<std::uint64_t>(hi - lo + 1));
    }

private:
    std::uint64_t s_;
};

power::PolicyDecision decision_for(model::PowerLevel level) {
    power::PowerState ps;
    ps.level = level;
    return power::PowerPolicy(ps, power::Thresholds{}).decision();
}

struct Rig {
    VirtualClock clk;
    FakeSleepSystem sys{clk};
    time::TimeKeeperState tks;
    time::TimeKeeper tk{tks, clk};
    WakeTiming wt;
    PlannerTuning tun;
    WakePlanner pl{wt, tk, tun};

    Rig() { clk.advance_us(5 * kUs); }

    /// Sets wall time (ground truth + TimeKeeper anchor) to `utc_us` now.
    void set_time(std::int64_t utc_us) {
        clk.set_true_utc_us(utc_us);
        (void)tk.set_utc(utc_us, time::TimeSource::kManual);
    }
    /// Declares the crystal fast/slow (+ = RTC fast) and teaches TimeKeeper the matching drift.
    void set_crystal_ppb(std::int32_t ppb) {
        clk.set_crystal_error_ppb(ppb);
        const std::int64_t p = ppb;
        tk.set_drift_ppb(static_cast<std::int32_t>(-p + ((p * p) / 1'000'000'000)));
    }
    [[nodiscard]] PlanRequest request(model::PowerLevel level = model::PowerLevel::kNormal) const {
        PlanRequest r;
        r.now_rtc_us = clk.rtc_us();
        r.level = level;
        r.decision = decision_for(level);
        return r;
    }
    /// RTC instant at which `utc` happens on the current (drift-free or corrected) mapping.
    [[nodiscard]] std::int64_t rtc_of(std::int64_t utc) const { return tk.rtc_at_utc(utc); }
};

// ---------------------------------------------------------------- latency EWMA

TEST(WakePlannerLatency, DefaultLeadComposition) {
    Rig const r;
    EXPECT_EQ(r.pl.latency_us(), 350'000);
    EXPECT_EQ(r.pl.lead_us(), 350'000 + r.tun.render_and_spi_us + (r.tun.waveform_us / 2));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerLatency, ConvergesExactlyAndMonotonicallyUpward) {
    Rig r;
    std::int64_t previous = r.pl.latency_us();
    int steps = 0;
    for (; steps < 200 && r.pl.latency_us() != 420'000; ++steps) {
        r.wt.scheduled_wake_rtc_us = 10 * kUs;
        ASSERT_TRUE(r.pl.on_timer_wake((10 * kUs) + 420'000));
        ASSERT_GE(r.pl.latency_us(), previous);
        ASSERT_LE(r.pl.latency_us(), 420'000) << "overshoot at step " << steps;
        previous = r.pl.latency_us();
    }
    EXPECT_EQ(r.pl.latency_us(), 420'000);
    EXPECT_LT(steps, 60);
}

TEST(WakePlannerLatency, FirstStepsFollowAlphaOneQuarter) {
    Rig r;
    r.wt.scheduled_wake_rtc_us = kUs;
    ASSERT_TRUE(r.pl.on_timer_wake(kUs + 420'000));
    EXPECT_EQ(r.wt.ewma_latency_us, 350'000 + 17'500); // (420-350)/4
    r.wt.scheduled_wake_rtc_us = kUs;
    ASSERT_TRUE(r.pl.on_timer_wake(kUs + 420'000));
    EXPECT_EQ(r.wt.ewma_latency_us, 367'500 + 13'125); // (420-367.5)/4
}

TEST(WakePlannerLatency, ConvergesDownwardToo) {
    Rig r;
    for (int i = 0; i < 100; ++i) {
        r.wt.scheduled_wake_rtc_us = kUs;
        ASSERT_TRUE(r.pl.on_timer_wake(kUs + 130'000));
        ASSERT_GE(r.pl.latency_us(), 130'000);
    }
    EXPECT_EQ(r.pl.latency_us(), 130'000);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerLatency, SamplesAreClampedToTheDocumentedRange) {
    Rig r;
    for (int i = 0; i < 200; ++i) {
        r.wt.scheduled_wake_rtc_us = kUs;
        ASSERT_TRUE(r.pl.on_timer_wake(kUs + 5'000)); // 5 ms: far below the floor
        ASSERT_GE(r.wt.ewma_latency_us, 100'000);
    }
    EXPECT_EQ(r.wt.ewma_latency_us, 100'000);
    for (int i = 0; i < 200; ++i) {
        r.wt.scheduled_wake_rtc_us = kUs;
        ASSERT_TRUE(r.pl.on_timer_wake(kUs + (8 * kUs))); // 8 s: plausible but beyond the cap
        ASSERT_LE(r.wt.ewma_latency_us, 1'500'000);
    }
    EXPECT_EQ(r.wt.ewma_latency_us, 1'500'000);
}

TEST(WakePlannerLatency, ImplausibleSamplesAreDroppedAndTheScheduleIsConsumed) {
    Rig r;
    // nothing scheduled
    EXPECT_FALSE(r.pl.on_timer_wake(99 * kUs));
    EXPECT_EQ(r.wt.ewma_latency_us, 350'000);
    // negative: woke before the schedule
    r.wt.scheduled_wake_rtc_us = 50 * kUs;
    EXPECT_FALSE(r.pl.on_timer_wake(49 * kUs));
    EXPECT_EQ(r.wt.ewma_latency_us, 350'000);
    EXPECT_EQ(r.wt.scheduled_wake_rtc_us, 0);
    // too large
    r.wt.scheduled_wake_rtc_us = 50 * kUs;
    EXPECT_FALSE(r.pl.on_timer_wake((50 * kUs) + r.tun.latency_sample_max_us + 1));
    EXPECT_EQ(r.wt.ewma_latency_us, 350'000);
    // exactly the limit is still a (clamped) sample
    r.wt.scheduled_wake_rtc_us = 50 * kUs;
    EXPECT_TRUE(r.pl.on_timer_wake((50 * kUs) + r.tun.latency_sample_max_us));
    EXPECT_GT(r.wt.ewma_latency_us, 350'000);
    // one sample per schedule
    const std::int32_t after = r.wt.ewma_latency_us;
    EXPECT_FALSE(r.pl.on_timer_wake(60 * kUs));
    EXPECT_EQ(r.wt.ewma_latency_us, after);
    // extreme values must not overflow
    r.wt.scheduled_wake_rtc_us = 1;
    EXPECT_FALSE(r.pl.on_timer_wake(std::numeric_limits<std::int64_t>::min()));
    r.wt.scheduled_wake_rtc_us = std::numeric_limits<std::int64_t>::max();
    EXPECT_FALSE(r.pl.on_timer_wake(std::numeric_limits<std::int64_t>::max() - 1));
    r.wt.scheduled_wake_rtc_us = 1;
    EXPECT_FALSE(r.pl.on_timer_wake(std::numeric_limits<std::int64_t>::max()));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerLatency, CorruptStoredValueIsClampedOnUse) {
    Rig r;
    for (const std::int32_t bad : {std::int32_t{0},
                                   std::int32_t{-5},
                                   std::numeric_limits<std::int32_t>::min(),
                                   std::numeric_limits<std::int32_t>::max(),
                                   std::int32_t{99'999},
                                   std::int32_t{1'500'001}}) {
        r.wt.ewma_latency_us = bad;
        EXPECT_GE(r.pl.latency_us(), 100'000);
        EXPECT_LE(r.pl.latency_us(), 1'500'000);
        r.wt.scheduled_wake_rtc_us = kUs;
        ASSERT_TRUE(r.pl.on_timer_wake(kUs + 400'000));
        EXPECT_GE(r.wt.ewma_latency_us, 100'000);
        EXPECT_LE(r.wt.ewma_latency_us, 1'500'000);
    }
}

TEST(WakePlannerLatency, RandomSamplesNeverEscapeTheRange) {
    Rig r;
    Rng rng(77);
    for (int i = 0; i < 20000; ++i) {
        r.wt.scheduled_wake_rtc_us = 7 * kUs;
        (void)r.pl.on_timer_wake((7 * kUs) + rng.range(-1 * kUs, 20 * kUs));
        ASSERT_GE(r.wt.ewma_latency_us, 100'000);
        ASSERT_LE(r.wt.ewma_latency_us, 1'500'000);
    }
}

// ------------------------------------------------- end-to-end alignment (virtual time)

struct TickStats {
    std::vector<std::int64_t> targets;
    std::vector<std::int64_t> error_us; ///< update midpoint (true UTC) - target boundary
    std::vector<std::int64_t> waits;
};

/// Runs `n` minute ticks through the real planner, sleep fake and clock, the way App will:
/// plan -> deep sleep -> wake after `latency(k)` -> measure -> pick the frame -> render ->
/// light-sleep to the boundary -> trigger -> waveform -> plan again.
template<class LatencyFn>
TickStats
run_ticks(Rig& r, int n, LatencyFn latency, model::PowerLevel level = model::PowerLevel::kNormal) {
    TickStats st;
    for (int k = 0; k < n; ++k) {
        const WakePlan p = r.pl.plan(r.request(level));
        EXPECT_GT(p.sleep.timer_us, 0);
        r.sys.deep_sleep(p.sleep);
        r.clk.advance_rtc_us(p.sleep.timer_us);
        r.clk.advance_us(latency(k));
        hal::WakeSources src;
        src.timer = true;
        r.sys.set_wake(ResetReason::kDeepSleep, src);
        EXPECT_TRUE(r.pl.on_timer_wake(r.sys.boot_rtc_us()));
        const std::int64_t target = r.pl.tick_target_utc_us(r.clk.rtc_us());
        r.clk.advance_us(r.tun.render_and_spi_us);
        const std::int64_t wait = r.pl.trigger_wait_us(r.clk.rtc_us(), target);
        if (wait > 0) {
            r.clk.advance_rtc_us(wait);
        }
        st.targets.push_back(target);
        st.waits.push_back(wait);
        st.error_us.push_back(r.clk.true_utc_us() + (r.tun.waveform_us / 2) - target);
        r.clk.advance_us(r.tun.waveform_us + (3 * kMs)); // panel waveform + housekeeping
    }
    return st;
}

void expect_consecutive(const TickStats& st, std::int64_t period_us) {
    for (std::size_t k = 1; k < st.targets.size(); ++k) {
        ASSERT_EQ(st.targets[k] - st.targets[k - 1], period_us) << "tick " << k;
        ASSERT_EQ(st.targets[k] % period_us, 0) << "tick " << k;
    }
}

TEST(WakePlannerAlignment, ConstantLatencyConvergesToExactBoundaryHits) {
    Rig r;
    r.set_time(kT0 + (17 * kUs) + 123'456); // arbitrary phase inside the minute
    const TickStats st = run_ticks(r, 120, [](int) { return std::int64_t{420'000}; });
    expect_consecutive(st, kMin);
    EXPECT_EQ(st.targets.front(), kT0 + kMin) << "first boundary after the starting instant";
    for (std::size_t k = 60; k < st.error_us.size(); ++k) {
        ASSERT_LE(std::abs(st.error_us[k]), 5) << "tick " << k << " (settled, drift-free)";
    }
    EXPECT_EQ(r.wt.ewma_latency_us, 420'000);
    // The initial estimate (350 ms) was 70 ms short: first flip is late by ~70 ms, inside
    // -250..+500.
    EXPECT_NEAR(static_cast<double>(st.error_us[0]), 70'000.0, 6'000.0);
}

TEST(WakePlannerAlignment, SlowWakeConvergesGeometrically) {
    Rig r;
    r.set_time(kT0 + (3 * kUs));
    const TickStats st = run_ticks(r, 80, [](int) { return std::int64_t{1'200'000}; });
    expect_consecutive(st, kMin);
    std::int64_t previous = std::numeric_limits<std::int64_t>::max();
    for (std::size_t k = 0; k < 40; ++k) {
        ASSERT_GE(st.error_us[k], -50'000);
        ASSERT_LE(st.error_us[k], previous) << "error must not grow, tick " << k;
        previous = st.error_us[k];
    }
    EXPECT_GT(st.error_us[0], 800'000); // starts late (visible), by design before it has learned
    EXPECT_LE(std::abs(st.error_us.back()), 5);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerAlignment, FastWakeUsesLightSleepToHitTheBoundary) {
    Rig r;
    r.set_time(kT0 + (9 * kUs));
    const TickStats st = run_ticks(r, 60, [](int) { return std::int64_t{120'000}; });
    expect_consecutive(st, kMin);
    // Initially 350 ms assumed vs 120 ms actual: early wake -> in-process wait (> 50 ms slack).
    EXPECT_GT(st.waits[0], r.tun.early_slack_us);
    EXPECT_LE(std::abs(st.error_us[0]), 5) << "waited exactly to boundary - waveform/2";
    for (std::size_t k = 0; k < st.error_us.size(); ++k) {
        ASSERT_GE(st.error_us[k], -static_cast<std::int64_t>(r.tun.early_slack_us)) << k;
        ASSERT_LE(st.error_us[k], 5) << k; // never late once it has to wait; settles at zero wait
    }
    EXPECT_EQ(st.waits.back(), 0);
    EXPECT_EQ(r.wt.ewma_latency_us, 120'000);
}

TEST(WakePlannerAlignment, JitteredLatencyStaysWithinTolerance) {
    Rig r;
    r.set_time(kT0 + (31 * kUs));
    Rng rng(2026);
    const TickStats st =
        run_ticks(r, 300, [&](int) { return 400'000 + rng.range(-12 * kMs, 12 * kMs); });
    expect_consecutive(st, kMin);
    for (std::size_t k = 40; k < st.error_us.size(); ++k) {
        ASSERT_LE(std::abs(st.error_us[k]), 30 * kMs) << "tick " << k;
        ASSERT_GE(st.error_us[k], -static_cast<std::int64_t>(r.tun.early_slack_us));
    }
}

TEST(WakePlannerAlignment, DriftCorrectedTimerKeepsTheBoundaryUnderCrystalError) {
    for (const std::int32_t ppb : {100'000, -150'000, 20'000, 190'000, -190'000}) {
        Rig r;
        r.set_time(kT0 + (5 * kUs));
        r.set_crystal_ppb(ppb);
        const TickStats st = run_ticks(r, 150, [](int) { return std::int64_t{380'000}; });
        expect_consecutive(st, kMin);
        for (std::size_t k = 50; k < st.error_us.size(); ++k) {
            // (latency is measured in RTC us but applied in UTC us: up to 200 ppm of ~400 ms)
            ASSERT_LE(std::abs(st.error_us[k]), 100) << "ppb " << ppb << " tick " << k;
        }
    }
}

TEST(WakePlannerAlignment, UncorrectedCrystalErrorWouldMissTheBoundary) {
    // Control experiment for the test above: with drift_ppb left at 0 and a +100 ppm crystal the
    // planner cannot hit the boundary any more (it only knows what TimeKeeper tells it).
    Rig r;
    r.set_time(kT0 + (5 * kUs));
    r.clk.set_crystal_error_ppb(100'000);
    const TickStats st = run_ticks(r, 40, [](int) { return std::int64_t{380'000}; });
    EXPECT_GT(std::abs(st.error_us.back()), 3 * kMs);
}

TEST(WakePlannerAlignment, WakesStayOnUtcMinutesAcrossDstTransitions) {
    // The planner knows nothing of local time: around both US transitions the sequence of
    // targets is the gapless, duplicate-free UTC minute sequence.
    for (const std::int64_t transition : {kUsSpringForward, kUsFallBack}) {
        Rig r;
        r.set_time(transition - (3 * kMin) - (22 * kUs));
        const TickStats st = run_ticks(r, 10, [](int) { return std::int64_t{350'000}; });
        expect_consecutive(st, kMin);
        bool crossed = false;
        for (const std::int64_t t : st.targets) {
            crossed = crossed || (t == transition);
        }
        EXPECT_TRUE(crossed) << "the run must include the transition minute itself";
        for (std::size_t k = 2; k < st.error_us.size(); ++k) {
            ASSERT_LE(std::abs(st.error_us[k]), 5 * kMs);
        }
    }
}

TEST(WakePlannerAlignment, PlanIsIndependentOfAnythingButUtc) {
    // Same UTC instant, same plan: there is no zone input to differ on. (Documents the contract.)
    Rig a;
    Rig b;
    a.set_time(kUsSpringForward - (7 * kUs));
    b.set_time(kUsSpringForward - (7 * kUs));
    const WakePlan pa = a.pl.plan(a.request());
    const WakePlan pb = b.pl.plan(b.request());
    EXPECT_EQ(pa.sleep.timer_us, pb.sleep.timer_us);
    EXPECT_EQ(pa.target_utc_us, pb.target_utc_us);
}

TEST(WakePlannerAlignment, SaverTicksEveryFiveUtcMinutes) {
    Rig r;
    r.set_time(kT0 + (2 * kMin) + (40 * kUs));
    const TickStats st =
        run_ticks(r, 40, [](int) { return std::int64_t{350'000}; }, model::PowerLevel::kSaver);
    expect_consecutive(st, 5 * kMin);
    EXPECT_EQ(st.targets.front(), kT0 + (5 * kMin));
    for (std::size_t k = 10; k < st.error_us.size(); ++k) {
        ASSERT_LE(std::abs(st.error_us[k]), 5);
    }
}

// ------------------------------------------------------------------ plan() details

TEST(WakePlannerPlan, NormalPlanSleepsUntilBoundaryMinusLead) {
    Rig r;
    r.set_time(kT0);
    r.clk.advance_us(30 * kUs); // 30 s into the minute
    const PlanRequest req = r.request();
    const WakePlan p = r.pl.plan(req);
    EXPECT_EQ(p.kind, WakeKind::kMinuteTick);
    EXPECT_EQ(p.target_utc_us, kT0 + kMin);
    EXPECT_EQ(p.sleep.timer_us, (30 * kUs) - r.pl.lead_us());
    EXPECT_EQ(p.wake_rtc_us, req.now_rtc_us + p.sleep.timer_us);
    EXPECT_EQ(r.wt.scheduled_wake_rtc_us, p.wake_rtc_us);
    EXPECT_TRUE(p.sleep.wake_on_buttons);
    EXPECT_FALSE(p.sleep.wake_on_epd_idle);
}

TEST(WakePlannerPlan, TimerDurationConvertsUtcToRawRtcWithDrift) {
    Rig r;
    r.set_time(kT0);
    r.tk.set_drift_ppb(150'000); // RTC slow: 1 s of UTC needs fewer RTC ticks
    r.clk.advance_us(10 * kUs);
    const PlanRequest req = r.request();
    const WakePlan p = r.pl.plan(req);
    const std::int64_t now_utc = r.tk.utc_at_rtc(req.now_rtc_us);
    const std::int64_t expected_rtc = r.tk.rtc_at_utc(kT0 + kMin - r.pl.lead_us()) - req.now_rtc_us;
    EXPECT_EQ(p.sleep.timer_us, expected_rtc);
    const std::int64_t naive = (kT0 + kMin - r.pl.lead_us()) - now_utc;
    EXPECT_LT(p.sleep.timer_us, naive - (4 * kMs)); // ~ 50 s * 150 ppm = 7.5 ms shorter
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerPlan, RandomSweepHoldsTheBoundaryInvariants) {
    Rng rng(5150);
    for (int i = 0; i < 20000; ++i) {
        Rig r;
        r.set_time(kT0 + (rng.range(0, 3LL * 24 * 3600) * kUs) + rng.range(0, 999'999));
        r.tk.set_drift_ppb(static_cast<std::int32_t>(rng.range(-200'000, 200'000)));
        r.clk.advance_us((rng.range(0, 7LL * 24 * 3600) * kUs / 8) + rng.range(0, 999'999));
        r.wt.ewma_latency_us = static_cast<std::int32_t>(rng.range(-50'000, 1'700'000));
        const bool saver = (rng.next() % 3U) == 0U;
        const model::PowerLevel level =
            saver ? model::PowerLevel::kSaver : model::PowerLevel::kNormal;
        const PlanRequest req = r.request(level);
        const WakePlan p = r.pl.plan(req);
        const std::int64_t period = saver ? 5 * kMin : kMin;
        const std::int64_t now_utc = r.tk.utc_at_rtc(req.now_rtc_us);

        ASSERT_EQ(p.kind, WakeKind::kMinuteTick) << i;
        ASSERT_GE(p.sleep.timer_us, r.tun.min_timer_us) << i;
        ASSERT_LE(p.sleep.timer_us, period + r.tun.min_timer_us) << i;
        ASSERT_EQ(p.wake_rtc_us, req.now_rtc_us + p.sleep.timer_us) << i;
        ASSERT_EQ(p.target_utc_us % period, 0) << i;
        ASSERT_GE(p.target_utc_us, now_utc + r.tun.min_timer_us - 1) << i;      // in the future
        ASSERT_LT(p.target_utc_us - period, now_utc + r.tun.min_timer_us) << i; // the earliest one
        // Unless the timer was floored at the minimum, the wake lands at boundary - lead.
        const std::int64_t wake_utc = r.tk.utc_at_rtc(p.wake_rtc_us);
        if (p.sleep.timer_us > r.tun.min_timer_us) {
            ASSERT_LE(std::abs(wake_utc - (p.target_utc_us - r.pl.lead_us())), 3) << i;
        } else {
            // Floored: the natural wake time is already due or less than min_timer away.
            ASSERT_LE(p.target_utc_us - r.pl.lead_us(), now_utc + r.tun.min_timer_us + 3) << i;
        }
    }
}

TEST(WakePlannerPlan, NearBoundaryDegradesToALateFlipNeverToAMissedMinute) {
    Rig r;
    r.set_time(kT0);
    // 100 ms before the boundary: the deep-sleep round trip cannot make it on time, but the
    // minute must still be updated (late) rather than skipped.
    r.clk.advance_us(kMin - (100 * kMs));
    const WakePlan p = r.pl.plan(r.request());
    EXPECT_EQ(p.kind, WakeKind::kMinuteTick);
    EXPECT_EQ(p.target_utc_us, kT0 + kMin);
    EXPECT_EQ(p.sleep.timer_us, r.tun.min_timer_us);
    // 5 ms before: timer-in-the-future rule pushes it to the next boundary.
    Rig q;
    q.set_time(kT0);
    q.clk.advance_us(kMin - (5 * kMs));
    const WakePlan p2 = q.pl.plan(q.request());
    EXPECT_EQ(p2.target_utc_us, kT0 + (2 * kMin));
    // Exactly on the boundary.
    Rig s;
    s.set_time(kT0);
    s.clk.advance_us(kMin);
    EXPECT_EQ(s.pl.plan(s.request()).target_utc_us, kT0 + (2 * kMin));
}

TEST(WakePlannerPlan, SaverUsesFiveMinuteBoundariesAndLowDoesNot) {
    Rig r;
    r.set_time(kT0 + (4 * kMin) + (59 * kUs));
    const WakePlan saver = r.pl.plan(r.request(model::PowerLevel::kSaver));
    EXPECT_EQ(saver.target_utc_us, kT0 + (5 * kMin));
    Rig q;
    q.set_time(kT0 + (4 * kMin) + (59 * kUs));
    const WakePlan low = q.pl.plan(q.request(model::PowerLevel::kLow));
    EXPECT_EQ(low.target_utc_us, kT0 + (5 * kMin)); // 1 s boundary away: same minute here ...
    Rig s;
    s.set_time(kT0 + (1 * kMin) + (10 * kUs));
    EXPECT_EQ(s.pl.plan(s.request(model::PowerLevel::kLow)).target_utc_us, kT0 + (2 * kMin));
    Rig t;
    t.set_time(kT0 + (1 * kMin) + (10 * kUs));
    EXPECT_EQ(t.pl.plan(t.request(model::PowerLevel::kSaver)).target_utc_us, kT0 + (5 * kMin));
}

TEST(WakePlannerPlan, CriticalHasNoTimerAndOnlyButtonAndUsbSources) {
    Rig r;
    r.set_time(kT0);
    PlanRequest req = r.request(model::PowerLevel::kCritical);
    req.tap_wake = true;
    r.wt.scheduled_wake_rtc_us = 12345;
    const WakePlan p = r.pl.plan(req);
    EXPECT_EQ(p.kind, WakeKind::kNone);
    EXPECT_EQ(p.sleep.timer_us, -1);
    EXPECT_EQ(p.wake_rtc_us, 0);
    EXPECT_EQ(r.wt.scheduled_wake_rtc_us, 0) << "a stale schedule must not survive";
    EXPECT_TRUE(p.sleep.wake_on_buttons);
    EXPECT_TRUE(p.sleep.wake_on_usb);
    EXPECT_FALSE(p.sleep.wake_on_accel);
    // A decision with no display period is "no timer" regardless of the level label.
    PlanRequest odd = r.request();
    odd.decision.display_period_min = 0;
    EXPECT_EQ(r.pl.plan(odd).sleep.timer_us, -1);
}

TEST(WakePlannerPlan, CriticalHasNoTimerEvenWithInvalidTime) {
    Rig r; // time never set
    const WakePlan p = r.pl.plan(r.request(model::PowerLevel::kCritical));
    EXPECT_EQ(p.sleep.timer_us, -1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerPlan, InvalidTimeTicksEveryTenMinutesRelativeToNow) {
    for (const auto level :
         {model::PowerLevel::kNormal, model::PowerLevel::kLow, model::PowerLevel::kSaver}) {
        Rig r; // never set: invalid
        r.clk.advance_us(1234 * kMs);
        const PlanRequest req = r.request(level);
        const WakePlan p = r.pl.plan(req);
        EXPECT_EQ(p.kind, WakeKind::kInvalidTime);
        EXPECT_EQ(p.sleep.timer_us, 10 * kMin);
        EXPECT_EQ(p.wake_rtc_us, req.now_rtc_us + (10 * kMin));
        EXPECT_EQ(p.target_utc_us, 0);
        EXPECT_TRUE(p.sleep.wake_on_buttons);
    }
}

TEST(WakePlannerPlan, PowerLossMakesTheTimeBaseInvalidAndFallsBack) {
    Rig r;
    r.clk.advance_us(100 * kUs);
    r.set_time(kT0);
    r.clk.power_loss(); // RTC restarts below the anchor
    const WakePlan p = r.pl.plan(r.request());
    EXPECT_EQ(p.kind, WakeKind::kInvalidTime);
}

TEST(WakePlannerPlan, ImplausibleUtcFallsBackInsteadOfAligningToGarbage) {
    for (const std::int64_t bad_anchor : {std::numeric_limits<std::int64_t>::max() / 2,
                                          std::numeric_limits<std::int64_t>::min() / 2,
                                          std::int64_t{-1} * kUs,
                                          std::int64_t{300'000'000'000LL} * kUs}) {
        Rig r;
        r.set_time(kT0);
        r.tks.anchor_utc_us = bad_anchor;
        const WakePlan p = r.pl.plan(r.request());
        EXPECT_EQ(p.kind, WakeKind::kInvalidTime) << bad_anchor;
        EXPECT_EQ(p.sleep.timer_us, 10 * kMin);
        EXPECT_EQ(r.pl.tick_target_utc_us(r.clk.rtc_us()), 0);
    }
}

TEST(WakePlannerPlan, SleepOverrideIsExactAndKeepsNormalSources) {
    Rig r;
    r.set_time(kT0 + (7 * kUs));
    PlanRequest req = r.request();
    req.sleep_override_us = 15 * kUs;
    req.tap_wake = true;
    req.usb_present = true; // `sleep` runs tethered
    const WakePlan p = r.pl.plan(req);
    EXPECT_EQ(p.kind, WakeKind::kSleepOverride);
    EXPECT_EQ(p.sleep.timer_us, 15 * kUs);
    EXPECT_EQ(p.wake_rtc_us, req.now_rtc_us + (15 * kUs));
    EXPECT_EQ(r.wt.scheduled_wake_rtc_us, p.wake_rtc_us);
    EXPECT_FALSE(p.sleep.wake_on_usb)
        << "USB is high now: a level-triggered wake would end the sleep at once";
    EXPECT_TRUE(p.sleep.wake_on_accel) << "Normal level + tap_wake setting: the usual sources";
    // Tiny requests are raised to the minimum the RTC can be given; Critical honours it too.
    PlanRequest tiny = r.request(model::PowerLevel::kCritical);
    tiny.sleep_override_us = 1;
    EXPECT_EQ(r.pl.plan(tiny).sleep.timer_us, r.tun.min_timer_us);
    // Zero / negative are "no override".
    PlanRequest none = r.request();
    none.sleep_override_us = -5;
    EXPECT_EQ(r.pl.plan(none).kind, WakeKind::kMinuteTick);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerPlan, WakeSourceMatrixMatchesTheBoardConfig) {
    Rig r;
    r.set_time(kT0);
    constexpr std::uint64_t kButtons = (1ULL << 7) | (1ULL << 6) | (1ULL << 0) | (1ULL << 8);
    for (const auto level : {model::PowerLevel::kNormal,
                             model::PowerLevel::kLow,
                             model::PowerLevel::kSaver,
                             model::PowerLevel::kCritical}) {
        for (unsigned bits = 0; bits < 16; ++bits) {
            PlanRequest req = r.request(level);
            req.tap_wake = (bits & 1U) != 0U;
            req.usb_wake = (bits & 2U) != 0U;
            req.usb_present = (bits & 4U) != 0U;
            const bool safe = (bits & 8U) != 0U;
            r.wt.safe_mode = safe ? 1 : 0;
            r.wt.crash_window_start_rtc_us = r.clk.rtc_us();
            const WakePlan p = r.pl.plan(req);

            const bool want_accel = req.tap_wake && req.decision.tap_wake_allowed &&
                                    level != model::PowerLevel::kCritical && !safe;
            const bool want_usb = req.usb_wake && !req.usb_present;
            ASSERT_TRUE(p.sleep.wake_on_buttons);
            ASSERT_EQ(p.sleep.wake_on_accel, want_accel)
                << "level " << static_cast<int>(level) << " bits " << bits;
            ASSERT_EQ(p.sleep.wake_on_usb, want_usb)
                << "level " << static_cast<int>(level) << " bits " << bits;

            const board::WakeConfig cfg = wake_config_for(p.sleep);
            ASSERT_EQ(cfg.ext1_mask & kButtons, kButtons);
            ASSERT_EQ(((cfg.ext1_mask >> board::kAccelInt1) & 1U) != 0U, want_accel);
            const bool usb_armed =
                cfg.ext0_enabled || ((cfg.ext1_mask >> board::kUsbDetect) & 1U) != 0U;
            ASSERT_EQ(usb_armed, want_usb);
        }
    }
    // Low and Saver and Critical forbid the tap wake in the policy itself: a sanity pin.
    EXPECT_TRUE(decision_for(model::PowerLevel::kNormal).tap_wake_allowed);
    EXPECT_FALSE(decision_for(model::PowerLevel::kLow).tap_wake_allowed);
    EXPECT_FALSE(decision_for(model::PowerLevel::kSaver).tap_wake_allowed);
    EXPECT_FALSE(decision_for(model::PowerLevel::kCritical).tap_wake_allowed);
}

TEST(WakePlannerPlan, TapWakeNeedsSettingPolicyAndNoSafeMode) {
    Rig r;
    r.set_time(kT0);
    PlanRequest req = r.request();
    EXPECT_FALSE(r.pl.plan(req).sleep.wake_on_accel); // setting off
    req.tap_wake = true;
    EXPECT_TRUE(r.pl.plan(req).sleep.wake_on_accel);
    r.pl.on_crash(r.clk.rtc_us());
    r.pl.on_crash(r.clk.rtc_us());
    r.pl.on_crash(r.clk.rtc_us());
    EXPECT_FALSE(r.pl.plan(req).sleep.wake_on_accel) << "safe mode: no tap wake";
    r.pl.on_usb_attach();
    EXPECT_TRUE(r.pl.plan(req).sleep.wake_on_accel);
}

// -------------------------------------------------------- tick target / trigger wait

TEST(WakePlannerTick, TargetTableAroundABoundary) {
    Rig r;
    r.set_time(kT0);
    const std::int64_t r0 = r.clk.rtc_us();
    const std::int64_t boundary = kT0 + kMin;
    const std::int64_t lead = r.pl.lead_us(); // 565 ms with defaults
    const std::int64_t mid = r.tun.render_and_spi_us + (r.tun.waveform_us / 2); // 215 ms
    struct Case {
        std::int64_t delta_us; ///< now relative to the boundary
        std::int64_t expect;
    };
    const Case cases[] = {
        {-10 * kUs, kT0},  // far too early: show the current minute
        {-lead - 1, kT0},  // wake time of the boundary not reached yet
        {-lead, boundary}, // exactly at wake time: in-process boundary
        {-mid, boundary},  // midpoint lands exactly on the boundary
        {-1000, boundary},
        {0, boundary},   // on the boundary
        {mid, boundary}, // a bit late
        {30 * kUs, boundary},
        {kMin - lead - 1, boundary},    // still the previous boundary's minute
        {kMin - lead, boundary + kMin}, // the next boundary is now due in-process
        {kMin - 1000, boundary + kMin},
    };
    for (const Case& c : cases) {
        const std::int64_t now_rtc = r0 + (boundary - kT0) + c.delta_us;
        EXPECT_EQ(r.pl.tick_target_utc_us(now_rtc), c.expect) << "delta " << c.delta_us;
    }
}

TEST(WakePlannerTick, ButtonWakeJustBeforeABoundaryWaitsForIt) {
    // 12:34:59.7 -> the frame must be 12:35, held until the boundary; the deep-sleep plan made
    // afterwards continues with 12:36.
    Rig r;
    r.set_time(kT0);
    r.clk.advance_us(kMin - (300 * kMs));
    const std::int64_t target = r.pl.tick_target_utc_us(r.clk.rtc_us());
    EXPECT_EQ(target, kT0 + kMin);
    r.clk.advance_us(r.tun.render_and_spi_us);
    const std::int64_t wait = r.pl.trigger_wait_us(r.clk.rtc_us(), target);
    EXPECT_EQ(wait, (300 * kMs) - r.tun.render_and_spi_us - (r.tun.waveform_us / 2));
    r.clk.advance_rtc_us(wait);
    EXPECT_EQ(r.clk.true_utc_us() + (r.tun.waveform_us / 2), target);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerTick, TriggerWaitRules) {
    Rig r;
    r.set_time(kT0);
    const std::int64_t half = r.tun.waveform_us / 2;
    const std::int64_t target = kT0 + kMin;
    const auto now_at = [&](std::int64_t before_trigger_us) {
        return r.rtc_of(target - half) - before_trigger_us;
    };
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(0), target), 0);
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(r.tun.early_slack_us), target), 0)
        << "exactly 50 ms early";
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(r.tun.early_slack_us + 1), target),
              r.tun.early_slack_us + 1);
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(2 * kUs), target), 2 * kUs);
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(r.tun.max_light_sleep_us), target),
              r.tun.max_light_sleep_us);
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(r.tun.max_light_sleep_us + 1), target), 0)
        << "implausibly early";
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(-500 * kMs), target), 0) << "already late";
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(kMin), target), 0);
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(1 * kUs), 0), 0) << "no target";
    EXPECT_EQ(r.pl.trigger_wait_us(now_at(1 * kUs), -5), 0);
}

TEST(WakePlannerTick, TriggerWaitIsDriftCorrected) {
    Rig r;
    r.set_time(kT0);
    r.tk.set_drift_ppb(-200'000); // RTC fast by 200 ppm: slightly more RTC ticks per UTC second
    const std::int64_t target = kT0 + kMin;
    const std::int64_t trigger_utc = target - (r.tun.waveform_us / 2);
    const std::int64_t now = r.rtc_of(trigger_utc - (3 * kUs)); // 3 UTC seconds before the trigger
    const std::int64_t wait = r.pl.trigger_wait_us(now, target);
    EXPECT_EQ(wait, r.rtc_of(trigger_utc) - now);
    EXPECT_NEAR(static_cast<double>(wait), 3'000'600.0, 2.0); // 3 s x (1 + 200 ppm)
}

TEST(WakePlannerTick, InvalidTimeHasNoTarget) {
    Rig const r;
    EXPECT_EQ(r.pl.tick_target_utc_us(r.clk.rtc_us()), 0);
    EXPECT_EQ(r.pl.trigger_wait_us(r.clk.rtc_us(), 0), 0);
}

// ------------------------------------------------------------------ safe mode

TEST(WakePlannerSafeMode, ThirdCrashWithinTenMinutesEnters) {
    Rig r;
    EXPECT_FALSE(r.pl.safe_mode(1000 * kUs));
    r.pl.on_crash(1000 * kUs);
    EXPECT_EQ(r.wt.crash_count_window, 1);
    r.pl.on_crash((1000 * kUs) + (5 * kMin));
    EXPECT_EQ(r.wt.crash_count_window, 2);
    EXPECT_FALSE(r.pl.safe_mode((1000 * kUs) + (5 * kMin)));
    r.pl.on_crash((1000 * kUs) + (10 * kMin)); // exactly at the end of the window: still inside
    EXPECT_EQ(r.wt.safe_mode, 1);
    EXPECT_TRUE(r.pl.safe_mode((1000 * kUs) + (10 * kMin)));
}

TEST(WakePlannerSafeMode, CrashesOutsideTheWindowRestartIt) {
    Rig r;
    r.pl.on_crash(1000 * kUs);
    r.pl.on_crash((1000 * kUs) + (9 * kMin));
    r.pl.on_crash((1000 * kUs) + (10 * kMin) + 1); // 1 us too late: new window
    EXPECT_EQ(r.wt.crash_count_window, 1);
    EXPECT_EQ(r.wt.crash_window_start_rtc_us, (1000 * kUs) + (10 * kMin) + 1);
    EXPECT_FALSE(r.pl.safe_mode((1000 * kUs) + (10 * kMin) + 1));
    // Slow trickle of crashes never trips it.
    Rig q;
    for (std::int64_t i = 0; i < 100; ++i) {
        q.pl.on_crash((1000 * kUs) + (i * 11 * kMin));
        ASSERT_FALSE(q.pl.safe_mode((1000 * kUs) + (i * 11 * kMin)));
    }
}

TEST(WakePlannerSafeMode, ExpiresAfter24HoursOrOnUsbAttach) {
    Rig r;
    const std::int64_t t = 5000 * kUs;
    r.pl.on_crash(t);
    r.pl.on_crash(t + kUs);
    r.pl.on_crash(t + (2 * kUs)); // safe mode entered at t + 2 s
    const std::int64_t entered = t + (2 * kUs);
    constexpr std::int64_t kDay = 24LL * 3600 * kUs;
    EXPECT_TRUE(r.pl.safe_mode(entered + kDay - 1));
    // More crashes while safe do not extend the hold.
    r.pl.on_crash(entered + kDay - 10);
    EXPECT_EQ(r.wt.crash_window_start_rtc_us, entered);
    EXPECT_FALSE(r.pl.safe_mode(entered + kDay));
    EXPECT_EQ(r.wt.safe_mode, 0);
    EXPECT_EQ(r.wt.crash_count_window, 0);

    Rig u;
    u.pl.on_crash(t);
    u.pl.on_crash(t);
    u.pl.on_crash(t);
    ASSERT_TRUE(u.pl.safe_mode(t + kUs));
    u.pl.on_usb_attach();
    EXPECT_FALSE(u.pl.safe_mode(t + kUs));
    EXPECT_EQ(u.wt.crash_count_window, 0);
    // ... and a fresh window starts from scratch.
    u.pl.on_crash(t + (2 * kUs));
    u.pl.on_crash(t + (3 * kUs));
    EXPECT_FALSE(u.pl.safe_mode(t + (3 * kUs)));
}

TEST(WakePlannerSafeMode, RtcRestartClearsAStaleSafeMode) {
    Rig r;
    r.pl.on_crash(900'000 * kUs);
    r.pl.on_crash(900'000 * kUs);
    r.pl.on_crash(900'000 * kUs);
    ASSERT_TRUE(r.pl.safe_mode(900'001 * kUs));
    EXPECT_FALSE(r.pl.safe_mode(5 * kUs)) << "RTC reads before the entry time: it restarted";
    EXPECT_EQ(r.wt.safe_mode, 0);
    // Same for a stale crash window.
    r.wt.crash_count_window = 2;
    r.wt.crash_window_start_rtc_us = 900'000 * kUs;
    r.pl.on_crash(10 * kUs);
    EXPECT_EQ(r.wt.crash_count_window, 1);
}

TEST(WakePlannerSafeMode, CrashCounterSaturates) {
    Rig r;
    r.wt.crash_count_window = std::numeric_limits<std::uint16_t>::max();
    r.wt.safe_mode = 1;
    r.wt.crash_window_start_rtc_us = 100 * kUs;
    r.pl.on_crash(200 * kUs);
    EXPECT_EQ(r.wt.crash_count_window, std::numeric_limits<std::uint16_t>::max());
    EXPECT_EQ(r.wt.crash_window_start_rtc_us, 100 * kUs);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(WakePlannerSafeMode, RandomCrashSequencesFollowAReferenceModel) {
    Rng rng(31337);
    for (int seq = 0; seq < 3000; ++seq) {
        Rig r;
        std::int64_t now = 1000 * kUs;
        // Reference: fixed 10-min window opened by its first crash; 3rd crash enters safe mode
        // (24 h hold, ended by USB attach).
        std::int64_t win_start = -1;
        int count = 0;
        bool safe = false;
        std::int64_t entered = 0;
        for (int step = 0; step < 40; ++step) {
            now += rng.range(1, 6 * kMin);
            if (rng.next() % 11U == 0U) {
                r.pl.on_usb_attach();
                safe = false;
                count = 0;
                win_start = -1;
            } else {
                if (safe && now - entered >= 24LL * 3600 * kUs) {
                    safe = false;
                    count = 0;
                    win_start = -1;
                }
                r.pl.on_crash(now);
                if (!safe) {
                    if (count == 0 || now - win_start > 10 * kMin) {
                        win_start = now;
                        count = 1;
                    } else {
                        ++count;
                    }
                    if (count >= 3) {
                        safe = true;
                        entered = now;
                    }
                }
            }
            if (safe && now - entered >= 24LL * 3600 * kUs) {
                safe = false;
                count = 0;
                win_start = -1;
            }
            ASSERT_EQ(r.pl.safe_mode(now), safe) << "seq " << seq << " step " << step;
        }
    }
}

// ------------------------------------------------------------- RTC persistence

TEST(WakePlannerPersistence, ScheduleAndSafeModeSurviveACommitLoadCycle) {
    testkit::FakeRtcMemory mem;
    RtcStore store(mem.state_region(), mem.frame_region());
    VirtualClock clk;
    clk.advance_us(5 * kUs);
    RtcState state;
    time::TimeKeeper tk(state.time, clk);
    clk.set_true_utc_us(kT0);
    (void)tk.set_utc(kT0, time::TimeSource::kManual);
    {
        WakePlanner pl(state.wake, tk);
        pl.on_crash(clk.rtc_us());
        pl.on_crash(clk.rtc_us());
        pl.on_crash(clk.rtc_us());
        PlanRequest req;
        req.now_rtc_us = clk.rtc_us();
        req.decision = decision_for(model::PowerLevel::kNormal);
        const WakePlan p = pl.plan(req);
        ASSERT_GT(p.sleep.timer_us, 0);
        state.header.boot_count = 9;
        store.commit(state);
        clk.advance_rtc_us(p.sleep.timer_us);
        clk.advance_us(430'000);
    }
    // "Next boot": everything comes from RTC memory.
    RtcState loaded;
    ASSERT_TRUE(store.load(loaded));
    time::TimeKeeper const tk2(loaded.time, clk);
    WakePlanner pl2(loaded.wake, tk2);
    EXPECT_TRUE(pl2.safe_mode(clk.rtc_us()));
    EXPECT_TRUE(pl2.on_timer_wake(clk.rtc_us()));
    EXPECT_EQ(loaded.wake.ewma_latency_us, 350'000 + 20'000); // (430-350)/4
    EXPECT_EQ(loaded.wake.scheduled_wake_rtc_us, 0);
    EXPECT_EQ(loaded.header.boot_count, 9U);
    // A single corrupted byte in the stored WakeTiming forces a cold start instead of a bad lead.
    mem.state_region()[offsetof(RtcState, wake) + 8] ^= 0x40U;
    RtcState again;
    EXPECT_FALSE(store.load(again));
}

static_assert(noexcept(std::declval<WakePlanner&>().plan(PlanRequest{})));
static_assert(noexcept(std::declval<const WakePlanner&>().tick_target_utc_us(0)));

} // namespace
} // namespace qz::app
