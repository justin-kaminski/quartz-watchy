// WP-22: virtual-time scenarios beyond the connectivity weeks: DST, step rollover, sync backoff,
// battery drain/recovery, crystal drift, power loss and the awake-time budget.
#include "sim_harness.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <vector>

// Scenario arithmetic mixes int literals with int64 microsecond constants and gtest macros inflate
// function complexity: those checks are relaxed for this file only.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions,readability-magic-numbers)
namespace qz::app {
namespace {

using namespace sim;

/// 2027-01-15 05:00Z = 23:00 CST (America/Chicago) on the 14th; local midnight is at 06:00Z.
constexpr std::int64_t kStart = kT0Utc - 3 * kHourUs;
constexpr std::int64_t kFirstMidnight = kStart + kHourUs;

/// Stores a custom settings block plus credentials before the first boot.
void provision_with(Env& env, const settings::Settings& cfg, bool creds = true) {
    ASSERT_TRUE(static_cast<bool>(settings::SettingsStore(env.kv).save(cfg, settings::defaults())));
    if (creds) {
        hal::WifiCredentials c;
        ASSERT_TRUE(c.ssid.assign("HomeNet"));
        ASSERT_TRUE(c.password.assign("correct-horse"));
        ASSERT_TRUE(static_cast<bool>(settings::CredentialStore(env.kv).save(c)));
    }
}

// ---- DST: the displayed minute is the true minute at every update --------------------------

struct DstCase {
    const char* zone;
    std::int64_t transition_utc_us;
    std::int32_t before_offset_s;
    std::int32_t after_offset_s;
    int expected_jump_min; ///< local minute-of-day delta across the transition (+61 / -59)
};

void run_dst(const DstCase& c) {
    Sim sim(false);
    const std::int64_t start = c.transition_utc_us - 90 * kMin;
    boot_manual(sim, start, c.zone);

    FlipStats flips;
    int prev = -1;
    std::vector<int> jumps;
    std::uint32_t wrong_minute = 0;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        observe_flip(s, o, flips, 1); // every update: the frame is the face of the true minute
        if (o.cause != model::WakeCause::kTimer || !o.panel_updated) {
            return;
        }
        const std::int64_t boundary = o.true_utc_us / kMin * kMin;
        const std::int32_t offset =
            boundary < c.transition_utc_us ? c.before_offset_s : c.after_offset_s;
        const time::LocalDateTime local = s.env.api().time_info().local;
        const int shown = local.time.hour * 60 + local.time.minute;
        if (shown != local_minute_of_day(boundary, offset)) {
            ++wrong_minute;
        }
        if (prev >= 0) {
            int diff = shown - prev;
            if (diff < -720) {
                diff += 1440;
            }
            if (diff != 1) {
                jumps.push_back(diff);
            }
        }
        prev = shown;
    };
    sim.run_until(c.transition_utc_us + 90 * kMin);

    EXPECT_EQ(wrong_minute, 0U) << c.zone;
    EXPECT_EQ(flips.frame_mismatches, 0U) << c.zone;
    EXPECT_GE(flips.updates, 175U) << "one update per minute for three hours";
    ASSERT_EQ(jumps.size(), 1U) << "exactly one discontinuity: the transition";
    EXPECT_EQ(jumps[0], c.expected_jump_min) << c.zone;
    EXPECT_GE(flips.min_late_us, 0);
    EXPECT_LT(flips.max_late_partial_us, 700'000);
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
}

TEST(SimDst, ChicagoSpringForward) {
    run_dst({"America/Chicago", utc_us_of(2027, 3, 14, 8, 0), -6 * 3600, -5 * 3600, 61});
}
TEST(SimDst, ChicagoFallBack) {
    run_dst({"America/Chicago", utc_us_of(2027, 11, 7, 7, 0), -5 * 3600, -6 * 3600, -59});
}
TEST(SimDst, BerlinSpringForward) {
    run_dst({"Europe/Berlin", utc_us_of(2027, 3, 28, 1, 0), 1 * 3600, 2 * 3600, 61});
}
TEST(SimDst, BerlinFallBack) {
    run_dst({"Europe/Berlin", utc_us_of(2027, 10, 31, 1, 0), 2 * 3600, 1 * 3600, -59});
}

// ---- steps: midnight rollover across DST ---------------------------------------------------

struct Burst {
    std::int64_t at_utc_us;
    std::uint32_t steps;
};
struct DayTotal {
    int year, month, day;
    std::uint32_t steps;
};

void run_steps(const char* zone,
               std::int64_t start,
               std::int64_t end,
               const std::vector<Burst>& bursts,
               const std::vector<DayTotal>& history,
               std::uint32_t today_steps) {
    Sim sim(false);
    boot_manual(sim, start, zone);
    for (const Burst& b : bursts) {
        sim.add_steps_at(b.at_utc_us, b.steps);
    }
    sim.run_until(end);
    const model::StepsSummary steps = sim.env.api().steps();
    EXPECT_EQ(steps.today, today_steps) << zone;
    ASSERT_EQ(steps.history_count, history.size()) << zone;
    for (std::size_t i = 0; i < history.size(); ++i) {
        const DayTotal& d = history[i];
        EXPECT_EQ(
            steps.history[i].day,
            time::days_from_civil(
                {d.year, static_cast<std::uint8_t>(d.month), static_cast<std::uint8_t>(d.day)}))
            << zone << " entry " << i;
        EXPECT_EQ(steps.history[i].steps, d.steps) << zone << " entry " << i;
    }
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
}

TEST(SimSteps, ChicagoSpringForwardDayIsTwentyThreeHoursLong) {
    // Local midnights: 2027-03-14 00:00 CST = 06:00Z and 03-15 00:00 CDT = 05:00Z.
    run_steps("America/Chicago",
              utc_us_of(2027, 3, 14, 4, 0),
              utc_us_of(2027, 3, 15, 6, 0),
              {{utc_us_of(2027, 3, 14, 5, 30), 100},  // 23:30 CST on the 13th
               {utc_us_of(2027, 3, 14, 17, 0), 1000}, // 12:00 CDT
               {utc_us_of(2027, 3, 15, 4, 30), 700},  // 23:30 CDT
               {utc_us_of(2027, 3, 15, 5, 30), 50}},  // 00:30 CDT on the 15th
              {{2027, 3, 14, 1700}, {2027, 3, 13, 100}},
              50);
}

TEST(SimSteps, ChicagoFallBackDayIsTwentyFiveHoursLong) {
    // Local midnights: 2027-11-07 00:00 CDT = 05:00Z and 11-08 00:00 CST = 06:00Z.
    run_steps("America/Chicago",
              utc_us_of(2027, 11, 7, 3, 0),
              utc_us_of(2027, 11, 8, 7, 0),
              {{utc_us_of(2027, 11, 7, 4, 30), 100},  // 23:30 CDT on the 6th
               {utc_us_of(2027, 11, 7, 18, 0), 1000}, // 12:00 CST
               {utc_us_of(2027, 11, 8, 5, 30), 700},  // 23:30 CST
               {utc_us_of(2027, 11, 8, 6, 30), 50}},  // 00:30 CST on the 8th
              {{2027, 11, 7, 1700}, {2027, 11, 6, 100}},
              50);
}

TEST(SimSteps, HavanaSpringForwardAtMidnightStartsTheDayAtOneAm) {
    // 05:00Z: 23:59 CST on the 13th jumps to 01:00 CDT on the 14th; steps taken in the last seconds
    // before the jump belong to the 13th.
    run_steps("America/Havana",
              utc_us_of(2027, 3, 14, 4, 0),
              utc_us_of(2027, 3, 14, 6, 0),
              {{utc_us_of(2027, 3, 14, 4, 30), 300},
               {utc_us_of(2027, 3, 14, 4, 59) + 50 * kUs, 77},
               {utc_us_of(2027, 3, 14, 5, 30), 40}},
              {{2027, 3, 13, 377}},
              40);
}

TEST(SimSteps, HavanaFallBackRepeatsMidnightHourWithoutRollingBack) {
    // 05:00Z: 01:00 CDT on the 7th falls back to 00:00 CST on the 7th; the day number never goes
    // back and the repeated hour keeps counting into the 7th.
    run_steps("America/Havana",
              utc_us_of(2027, 11, 7, 3, 30),
              utc_us_of(2027, 11, 7, 6, 30),
              {{utc_us_of(2027, 11, 7, 3, 45), 100},
               {utc_us_of(2027, 11, 7, 4, 30), 40},
               {utc_us_of(2027, 11, 7, 5, 30), 60}},
              {{2027, 11, 6, 100}},
              100);
}

// ---- sync backoff ---------------------------------------------------------------------------

TEST(SimBackoff, FailedSyncsBackOffExponentiallyAndNeverRetryEveryWake) {
    Sim sim;
    sim.env.clock.set_true_utc_us(kStart);
    settings::Settings cfg = settings::defaults();
    cfg.connectivity = model::ConnectivityMode::kTimeOnly;
    cfg.sync_interval_h = 6;
    provision_with(sim.env, cfg);
    sim.env.net.script_connect(ok(), 100);                     // the boot sync works
    sim.env.net.script_connect(Error{Errc::kTimeout}, 10'000); // then the access point is gone
    sim.cold_boot();
    ASSERT_TRUE(sim.env.api().time_info().valid);
    ASSERT_EQ(sim.env.net.connect_calls(), 1U);

    std::vector<std::int64_t> attempts{sim.now_utc_us()};
    std::uint32_t seen = 1;
    std::uint32_t max_streak = 0;
    std::uint32_t consecutive_wake_retries = 0;
    bool previous_wake_attempted = false;
    FlipStats flips;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        observe_flip(s, o, flips, 101);
        const bool attempted = s.env.net.connect_calls() != seen;
        if (attempted) {
            attempts.push_back(o.true_utc_us - o.awake_us);
            seen = s.env.net.connect_calls();
        }
        if (attempted && previous_wake_attempted) {
            ++consecutive_wake_retries;
        }
        previous_wake_attempted = attempted;
        max_streak = std::max<std::uint32_t>(max_streak, s.env.api().sync_info().fail_streak);
    };
    // The network comes back during hour 22: the retry after that succeeds.
    sim.act(kStart + 22 * kHourUs, [](Sim& s) { s.env.net.script_connect(ok(), 100); });
    sim.run_until(kStart + 27 * kHourUs);

    ASSERT_GE(attempts.size(), 9U);
    EXPECT_EQ(consecutive_wake_retries, 0U) << "never a retry on every wake";
    EXPECT_NEAR(static_cast<double>(attempts[1] - attempts[0]) / kHourUs, 6.0, 0.1);
    // Gap after failing attempt k: min(15 min x 2^(k-1), min(interval, 12 h)) +-10 % jitter.
    for (std::size_t k = 1; k + 1 < attempts.size(); ++k) {
        const std::int64_t base_min = std::min<std::int64_t>(15LL << (k - 1), 6 * 60);
        const std::int64_t gap = attempts[k + 1] - attempts[k];
        EXPECT_GE(gap, base_min * kMin * 9 / 10 - kMin) << "gap " << k;
        EXPECT_LE(gap, base_min * kMin * 11 / 10 + 2 * kMin) << "gap " << k;
    }
    EXPECT_GE(max_streak, 6U);
    // Recovery: success resets the streak and the next sync is a full interval away.
    const console::SyncInfo sync = sim.env.api().sync_info();
    EXPECT_EQ(sync.indicator, model::SyncIndicator::kOk);
    EXPECT_EQ(sync.fail_streak, 0);
    EXPECT_GE(sync.next_time_sync - sim.now_utc_us() / kUs, 5 * 3600);
    EXPECT_EQ(flips.frame_mismatches, 0U) << "the face stays right through every failed session";
    EXPECT_EQ(sim.env.net.violations(), 0U);
    EXPECT_FALSE(sim.env.net.radio_on());
}

// ---- battery drain and recovery ---------------------------------------------------------------

TEST(SimBattery, DrainsThroughLowSaverCriticalAndRecoversOnUsb) {
    Sim sim;
    sim.env.clock.set_true_utc_us(kStart);
    settings::Settings cfg = settings::defaults();
    cfg.connectivity = model::ConnectivityMode::kTimeOnly;
    cfg.sync_interval_h = 6;
    provision_with(sim.env, cfg);
    // 3700 mV falling 25 mV/h (Low ~4 h, Saver ~8 h, Critical ~12 h); charging from hour 14 on.
    sim.battery_curve = [](std::int64_t utc) -> std::optional<int> {
        const double hours = static_cast<double>(utc - kStart) / static_cast<double>(kHourUs);
        return hours < 14.0 ? std::max(3250, static_cast<int>(3700.0 - 25.0 * hours)) : 4000;
    };
    sim.cold_boot();
    ASSERT_TRUE(sim.env.api().time_info().valid);
    ASSERT_EQ(sim.env.net.connect_calls(), 1U);

    std::vector<model::PowerLevel> levels{model::PowerLevel::kNormal};
    std::uint32_t connects_when_low = 0;
    std::uint32_t saver_ticks = 0;
    bool saver_cadence_ok = true;
    FlipStats flips;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        observe_flip(s, o, flips, 13);
        const model::PowerLevel level = s.env.api().battery().level;
        if (level != levels.back()) {
            levels.push_back(level);
            if (level == model::PowerLevel::kLow) {
                connects_when_low = s.env.net.connect_calls();
            }
        }
        if (level == model::PowerLevel::kSaver && o.cause == model::WakeCause::kTimer) {
            ++saver_ticks;
            saver_cadence_ok = saver_cadence_ok && o.plan.timer_us > 3 * kMin;
        }
    };
    sim.usb_session_at(kStart + 14 * kHourUs, 60 * kMin);
    sim.run_until(kStart + 13 * kHourUs);

    // Critical: no timer, buttons + USB only, the Charge me screen on the panel.
    ASSERT_GE(levels.size(), 4U);
    EXPECT_EQ(levels[1], model::PowerLevel::kLow);
    EXPECT_EQ(levels[2], model::PowerLevel::kSaver);
    EXPECT_EQ(levels[3], model::PowerLevel::kCritical);
    EXPECT_EQ(levels.size(), 4U) << "monotone drain: no flapping between levels";
    EXPECT_EQ(sim.plan.timer_us, -1);
    EXPECT_TRUE(sim.plan.wake_on_buttons);
    EXPECT_TRUE(sim.plan.wake_on_usb);
    EXPECT_FALSE(sim.plan.wake_on_accel);
    EXPECT_FALSE(
        same_frame(sim.env.epd.displayed(), expected_face(sim.env, sim.now_utc_us() / kUs)))
        << "Charge me, not the face";
    // The sync that fell due at hour 6 (Low/Saver) was refused, not queued up behind the display.
    EXPECT_EQ(sim.env.net.connect_calls(), 1U);
    EXPECT_EQ(connects_when_low, 1U);
    EXPECT_GT(saver_ticks, 20U);
    EXPECT_TRUE(saver_cadence_ok) << "Saver wakes every 5 minutes, not every minute";

    // USB at hour 14 (tethered for an hour, voltage rising), then back on battery: normal again,
    // the overdue sync runs and the face is live.
    sim.run_until(kStart + 17 * kHourUs);
    EXPECT_EQ(sim.env.api().battery().level, model::PowerLevel::kNormal);
    EXPECT_EQ(levels.back(), model::PowerLevel::kNormal);
    EXPECT_LE(levels.size(), 8U);
    EXPECT_EQ(sim.env.net.connect_calls(), 2U);
    EXPECT_EQ(sim.env.api().sync_info().indicator, model::SyncIndicator::kOk);
    EXPECT_GT(sim.plan.timer_us, 0);
    EXPECT_LT(sim.plan.timer_us, kMin);
    EXPECT_TRUE(same_frame(sim.env.epd.displayed(),
                           expected_face(sim.env, sim.now_utc_us() / kMin * kMin / kUs)));
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
}

// ---- crystal drift ----------------------------------------------------------------------------

void run_drift(int ppm) {
    Sim sim;
    sim.env.clock.set_true_utc_us(kStart);
    provision_nvs(sim.env, model::ConnectivityMode::kTimeOnly);
    sim.set_crystal_ppm(ppm);
    sim.cold_boot();
    ASSERT_TRUE(sim.env.api().time_info().valid);

    std::int64_t day1_max_err_us = 0;
    std::int64_t corrected_max_err_us = 0;
    FlipStats flips;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        const std::int64_t err = s.env.api().time_info().utc_us - s.now_utc_us();
        const std::int64_t since = o.true_utc_us - kStart;
        if (since < 24 * kHourUs - 5 * kMin) {
            day1_max_err_us = std::max(day1_max_err_us, static_cast<std::int64_t>(std::llabs(err)));
        } else if (since > 25 * kHourUs) {
            corrected_max_err_us =
                std::max(corrected_max_err_us, static_cast<std::int64_t>(std::llabs(err)));
            observe_flip(s, o, flips, 0);
        }
    };
    sim.run_until(kStart + 47 * kHourUs);

    EXPECT_GT(day1_max_err_us, 2 * kUs) << "without correction the crystal error exceeds 2 s/day";
    EXPECT_LT(corrected_max_err_us, 2 * kUs) << "after the second sync: < 2 s/day (ARCH 8.2)";
    const std::int32_t drift = sim.env.api().time_info().drift_ppb;
    EXPECT_NEAR(static_cast<double>(drift), -ppm * 1000.0, 5000.0)
        << "estimated drift cancels the crystal error";
    EXPECT_EQ(sim.env.net.connect_calls(), 2U) << "boot sync + the 24 h sync";
    // Minute flips stay on the boundary once the clock is disciplined.
    EXPECT_GE(flips.min_late_us, 0);
    EXPECT_LT(flips.max_late_partial_us, 700'000);
}

TEST(SimDrift, FastCrystalPlus40PpmIsCorrectedBelowTwoSecondsPerDay) {
    run_drift(40);
}
TEST(SimDrift, SlowCrystalMinus40PpmIsCorrectedBelowTwoSecondsPerDay) {
    run_drift(-40);
}

// ---- power loss -------------------------------------------------------------------------------

TEST(SimPowerLoss, OffModeLosesTimeButKeepsHistoryAndSettings) {
    Sim sim;
    provision_nvs(sim.env, model::ConnectivityMode::kOff);
    boot_manual(sim, kStart, "America/Chicago");
    sim.add_steps_at(kFirstMidnight + 8 * kHourUs, 500);            // 15th, 08:00 local
    sim.add_steps_at(kFirstMidnight + kDayUs + 30 * kMin, 400);     // 16th, 00:30 local
    const std::int64_t loss_at = kFirstMidnight + kDayUs + kHourUs; // 16th 01:00 local
    sim.power_loss_at(loss_at, 22 * kHourUs);
    sim.run_until(loss_at);

    // Cold boot after 22 dark hours: time unknown, face says so, ticks every 10 minutes.
    EXPECT_FALSE(sim.env.api().time_info().valid);
    EXPECT_EQ(sim.plan.timer_us, 600 * kUs);
    EXPECT_TRUE(same_frame(sim.env.epd.displayed(), expected_face(sim.env, 0, false)));
    const console::DeviceApi& api = sim.env.api();
    EXPECT_EQ(api.wake_record(api.wake_record_count() - 1).cause, model::WakeCause::kColdBoot);
    EXPECT_EQ(api.current_settings().tz_name.view(), "America/Chicago") << "settings live in NVS";
    // Flushed history survives; the unflushed bits of the day (400 steps) are gone.
    model::StepsSummary steps = sim.env.api().steps();
    ASSERT_EQ(steps.history_count, 2U); // the 15th (500) and the empty boot day before it
    EXPECT_EQ(steps.history[0].steps, 500U);
    EXPECT_EQ(steps.history[1].steps, 0U);
    EXPECT_EQ(steps.today, 0U);

    // Housekeeping ticks while invalid; steps taken meanwhile wait in the pending bucket.
    sim.run_until(sim.now_utc_us() + 30 * kMin);
    EXPECT_FALSE(sim.env.api().time_info().valid);
    sim.env.accel.add_steps(300);
    sim.run_until(sim.now_utc_us() + 10 * kMin);
    EXPECT_EQ(sim.env.api().steps().today, 0U) << "pending until the time is known";
    EXPECT_EQ(sim.env.net.radio_init_count(), 0U);

    // The owner sets the time (still the 16th, local 23:40): pending steps go to today.
    ASSERT_TRUE(static_cast<bool>(sim.env.api().set_time_utc(sim.now_utc_us() / kUs)));
    EXPECT_TRUE(sim.env.api().time_info().valid);
    EXPECT_EQ(sim.env.api().steps().today, 300U);
    // Across the midnight rollover; +5 s: the manual set has whole-second resolution.
    sim.run_until(kFirstMidnight + 2 * kDayUs + 20 * kMin + 5 * kUs);
    steps = sim.env.api().steps();
    EXPECT_EQ(steps.today, 0U);
    ASSERT_EQ(steps.history_count, 3U);
    EXPECT_EQ(steps.history[0].steps, 300U);
    EXPECT_EQ(steps.history[1].steps, 500U);
    EXPECT_EQ(steps.history[0].day, steps.history[1].day + 1);
    EXPECT_TRUE(same_frame(sim.env.epd.displayed(),
                           expected_face(sim.env, sim.now_utc_us() / kMin * kMin / kUs)));
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
    EXPECT_EQ(sim.env.accel.protocol_violations(), 0U);
}

TEST(SimPowerLoss, TimeOnlyModeResyncsOnBootAndShowsTheTimeAgain) {
    Sim sim;
    sim.env.clock.set_true_utc_us(kStart);
    settings::Settings cfg = settings::defaults();
    cfg.connectivity = model::ConnectivityMode::kTimeOnly;
    ASSERT_TRUE(cfg.tz_name.assign("America/Chicago"));
    ASSERT_TRUE(cfg.tz_posix.assign("CST6CDT,M3.2.0,M11.1.0"));
    provision_with(sim.env, cfg);
    sim.cold_boot();
    sim.add_steps_at(kFirstMidnight + 8 * kHourUs, 500);
    sim.add_steps_at(kFirstMidnight + kDayUs + 30 * kMin, 400);
    const std::int64_t loss_at = kFirstMidnight + kDayUs + kHourUs;
    sim.power_loss_at(loss_at, 22 * kHourUs);
    sim.run_until(loss_at);

    // The boot wake itself ran the session: time valid again, drift estimate kept in NVS, face
    // live.
    EXPECT_TRUE(sim.env.api().time_info().valid);
    EXPECT_EQ(sim.env.net.connect_calls(), 3U);
    EXPECT_LT(std::llabs(sim.env.api().time_info().utc_us - sim.now_utc_us()), 2 * kUs);
    EXPECT_TRUE(same_frame(sim.env.epd.displayed(),
                           expected_face(sim.env, sim.now_utc_us() / kMin * kMin / kUs)));
    const model::StepsSummary steps = sim.env.api().steps();
    ASSERT_EQ(steps.history_count, 2U);
    EXPECT_EQ(steps.history[0].steps, 500U);
    sim.run_until(sim.now_utc_us() + 20 * kMin);
    EXPECT_EQ(sim.env.net.connect_calls(), 3U) << "boot, the 24 h sync, the boot after the loss";
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
}

// ---- awake time and the minute-tick budget ----------------------------------------------------

TEST(SimBudget, AwakeTimeTotalsPerWakeTypeAndMinuteTickBudget) {
    Sim sim(false);
    boot_manual(sim, kStart, "America/Chicago");
    sim.press_at(kStart + 2 * kHourUs, hal::kButtonBitBack);
    sim.press_at(kStart + 5 * kHourUs, hal::kButtonBitBack);
    sim.press_at(kStart + 9 * kHourUs, hal::kButtonBitBack);
    sim.usb_session_at(kStart + 12 * kHourUs, 5 * kMin);

    std::int64_t max_partial_tick_us = 0;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        if (o.cause == model::WakeCause::kTimer && o.panel_updated && !o.full_update &&
            s.wakes > 12) {
            max_partial_tick_us = std::max(max_partial_tick_us, o.awake_us);
        }
    };
    sim.run_until(kStart + kDayUs);

    const AwakeTotals& t = sim.totals;
    EXPECT_EQ(t.count(model::WakeCause::kColdBoot), 1U);
    EXPECT_EQ(t.count(model::WakeCause::kButton), 3U);
    EXPECT_EQ(t.count(model::WakeCause::kUsb), 1U);
    EXPECT_GE(t.count(model::WakeCause::kTimer), 1430U);

    std::cout << "[sim] awake per wake type over one day (virtual ms, waveform included):\n";
    const char* names[] = {"cold", "reset", "timer", "button", "accel", "usb", "tether", "unknown"};
    for (std::size_t i = 0; i < model::kWakeCauseCount; ++i) {
        if (t.wakes[i] != 0) {
            std::cout << "[sim]   " << names[i] << ": wakes=" << t.wakes[i]
                      << " total=" << t.awake_us[i] / 1000
                      << " mean=" << t.awake_us[i] / t.wakes[i] / 1000
                      << " max=" << t.max_awake_us[i] / 1000 << "\n";
        }
    }
    std::cout << "[sim] energy estimate (WP-11 model, [TUNE] constants): "
              << sim.estimate_hours(1.0) << " h (" << sim.estimate_hours(1.0) / 24 << " days)\n";

    // ARCH section 4: minute tick <= 60 ms CPU + panel waveform (partial: 260 ms in the fake). The
    // fakes cost no virtual CPU time, so the virtual awake time is waveform + the wake-ahead wait;
    // allow 200 ms for the latter [ASSUMED model].
    EXPECT_GT(max_partial_tick_us, 0);
    EXPECT_LE(max_partial_tick_us, (60 + 260 + 200) * 1000);
    // Cold boot: <= 2 s (CPU incl. BMA init) + the 2 s full-refresh waveform.
    EXPECT_LE(t.mean_awake_us(model::WakeCause::kColdBoot), 4 * kUs + 200'000);
    // A short button session (face: idle after 2 s) stays within a few seconds.
    EXPECT_LE(t.max_awake_us[static_cast<std::size_t>(model::WakeCause::kButton)], 6 * kUs);
    // Awake share of the day stays below 3 % (minute ticks dominate).
    EXPECT_LT(t.total_awake_us() - t.awake_us[static_cast<std::size_t>(model::WakeCause::kUsb)],
              kDayUs * 3 / 100);
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
}

} // namespace
} // namespace qz::app
// NOLINTEND(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions,readability-magic-numbers)
