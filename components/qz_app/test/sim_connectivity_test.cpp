// WP-22: seven virtual days of App wakes per connectivity mode (ARCHITECTURE.md sections 4, 12).
#include "sim_harness.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>

// Scenario arithmetic mixes int literals with int64 microsecond constants and gtest macros inflate
// function complexity: those checks are relaxed for this file only.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
namespace qz::app {
namespace {

using namespace sim;

/// 2027-01-15 05:00Z = 23:00 CST (America/Chicago) on the 14th: local midnight is at 06:00Z.
constexpr std::int64_t kStart = kT0Utc - 3 * kHourUs;
constexpr std::int64_t kFirstMidnight = kStart + kHourUs; ///< 06:00Z
constexpr std::int64_t kWeek = 7 * kDayUs;

constexpr const char* kWeatherBody =
    R"({"current":{"temperature_2m":12.3,"weather_code":3},"daily":{"temperature_2m_max":[15.1],"temperature_2m_min":[8.4]}})";
constexpr const char* kWeatherBodyLast =
    R"({"current":{"temperature_2m":20.0,"weather_code":0},"daily":{"temperature_2m_max":[22.0],"temperature_2m_min":[9.0]}})";

/// 3500 steps in three bursts every local day (08:00, 12:00, 17:30 local), starting on the 15th.
void script_daily_steps(Sim& sim, int days) {
    for (int d = 0; d < days; ++d) {
        const std::int64_t midnight = kFirstMidnight + d * kDayUs;
        sim.add_steps_at(midnight + 8 * kHourUs, 1000);
        sim.add_steps_at(midnight + 12 * kHourUs, 2000);
        sim.add_steps_at(midnight + 17 * kHourUs + 30 * kMin, 500);
    }
}

/// Partial updates land within 0.7 s after the true minute (ARCH 8.4: -250..+500 ms target plus
/// margin). A full refresh (every 31st minute) flashes for ~2 s and the wake-ahead lead assumes the
/// partial waveform, so those complete up to ~2 s late: bounded by the full waveform, never earlier
/// than the minute. [TECH-DEBT] lead for a planned full refresh.
void expect_flip_timing(const FlipStats& flips) {
    EXPECT_GE(flips.min_late_us, 0) << "never before the true minute";
    EXPECT_LT(flips.max_late_partial_us, 700'000);
    EXPECT_LT(flips.max_late_full_us, 2'200'000);
}

void print_summary(const char* name, const Sim& sim, double days, double real_s) {
    const AwakeTotals& t = sim.totals;
    const std::int64_t tick_us = t.mean_awake_us(model::WakeCause::kTimer);
    // The energy estimate is the WP-11 model with its [TUNE] constants: informational only.
    std::cout << "[sim] " << name << ": wakes=" << t.total_wakes() << " (timer "
              << t.count(model::WakeCause::kTimer) << "), mean timer awake " << tick_us / 1000
              << " ms, awake/day " << t.total_awake_us() / 1000 / static_cast<long long>(days)
              << " ms, est. battery " << sim.estimate_hours(days) << " h ("
              << static_cast<int>(sim.estimate_hours(days) / 24) << " d), real "
              << static_cast<int>(real_s * 100) / 100.0 << " s\n";
}

class Stopwatch {
public:
    [[nodiscard]] double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }

private:
    std::chrono::steady_clock::time_point t0_ = std::chrono::steady_clock::now();
};

// ---- Off ------------------------------------------------------------------------------------

TEST(SimConnectivity, OffModeSevenDaysNeverTouchesTheRadioAndBarelyTouchesNvs) {
    const Stopwatch watch;
    Sim sim;
    // Credentials and a location are present: Off must still mean off.
    provision_nvs(sim.env, model::ConnectivityMode::kOff, true, true);
    // A watch that has already stored its step history once: the one-time schema-version key is in
    // NVS, so the week below shows the steady state (history + today blob per local midnight).
    // [TECH-DEBT] the very first flush of a fresh device writes that key too: its first week costs
    // 15 writes, one above the ARCHITECTURE section 7 figure.
    ASSERT_TRUE(static_cast<bool>(steps::StepHistoryStore(sim.env.kv).save(steps::StepState{})));
    boot_manual(sim, kStart, "America/Chicago");
    sim.run_until(kFirstMidnight + 30 * kMin);
    const std::uint32_t nvs_before = sim.env.kv.write_count();
    const std::int64_t window_end = sim.now_utc_us() + kWeek;

    FlipStats flips;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        observe_flip(s, o, flips, 97);
    };
    script_daily_steps(sim, 7);
    sim.run_until(window_end);

    EXPECT_EQ(sim.env.net.radio_init_count(), 0U);
    EXPECT_TRUE(sim.env.net.calls().empty());
    EXPECT_EQ(sim.env.portal.starts, 0U);
    const std::uint32_t nvs_writes = sim.env.kv.write_count() - nvs_before;
    EXPECT_LE(nvs_writes, 14U) << "7 midnight flushes at most (flash wear, ARCH section 7)";
    EXPECT_GT(nvs_writes, 0U) << "history is flushed at the day rollover";

    // One partial update per minute (plus the periodic full refreshes), all of them on time.
    EXPECT_GE(flips.updates, 7U * 1440U - 10U);
    EXPECT_EQ(flips.frame_mismatches, 0U);
    EXPECT_GT(flips.frames_checked, 100U);
    expect_flip_timing(flips);
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
    EXPECT_EQ(sim.env.accel.protocol_violations(), 0U);
    EXPECT_EQ(sim.env.epd.aborted_updates(), 0U);
    EXPECT_NEAR(static_cast<double>(sim.env.epd.full_updates()), 7.0 * 1440.0 / 31.0, 20.0);

    // Seven local days of 3500 steps, newest first; the last rollover (06:00Z day 8) just closed
    // the seventh.
    const model::StepsSummary steps = sim.env.api().steps();
    EXPECT_EQ(steps.today, 0U);
    EXPECT_EQ(steps.history_count, 7);
    for (std::size_t i = 0; i < steps.history_count; ++i) {
        EXPECT_EQ(steps.history[i].steps, 3500U) << "day " << i;
    }
    print_summary("Off 7d", sim, 7.0, watch.seconds());
}

// ---- Time only ------------------------------------------------------------------------------

TEST(SimConnectivity, TimeOnlySevenDaysSyncsOncePerIntervalAndKeepsTheFace) {
    const Stopwatch watch;
    Sim sim;
    sim.env.clock.set_true_utc_us(kStart);
    provision_nvs(sim.env, model::ConnectivityMode::kTimeOnly);
    sim.cold_boot(); // the first session sets the time
    ASSERT_TRUE(sim.env.api().time_info().valid);
    set_timezone(sim, "America/Chicago");
    ASSERT_EQ(sim.env.net.connect_calls(), 1U);

    FlipStats flips;
    std::uint32_t radio_wakes = 0;
    std::int64_t longest_radio_wake_us = 0;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        observe_flip(s, o, flips, 211);
        const console::DeviceApi& api = s.env.api();
        if ((api.wake_record(api.wake_record_count() - 1).flags & model::kWakeFlagRadio) != 0) {
            ++radio_wakes;
            longest_radio_wake_us = std::max(longest_radio_wake_us, o.awake_us);
        }
    };
    script_daily_steps(sim, 7);
    sim.run_until(kStart + kWeek);

    // Boot sync + one per 24 h; never an HTTP request in time-only mode.
    EXPECT_GE(sim.env.net.connect_calls(), 7U);
    EXPECT_LE(sim.env.net.connect_calls(), 9U);
    EXPECT_EQ(sim.env.net.sntp_calls(), sim.env.net.connect_calls());
    EXPECT_EQ(sim.env.net.http_calls(), 0U);
    EXPECT_EQ(sim.env.net.radio_init_count(), sim.env.net.connect_calls());
    EXPECT_EQ(sim.env.net.shutdown_calls(), sim.env.net.connect_calls()) << "teardown every time";
    EXPECT_FALSE(sim.env.net.radio_on());
    EXPECT_EQ(sim.env.net.violations(), 0U);
    EXPECT_EQ(radio_wakes + 1, sim.env.net.connect_calls()) << "sessions only on flagged wakes";
    EXPECT_LT(longest_radio_wake_us, 30 * kUs) << "session hard budget (ARCH section 12)";
    EXPECT_EQ(sim.env.api().sync_info().indicator, model::SyncIndicator::kOk);
    EXPECT_EQ(sim.env.api().sync_info().fail_streak, 0);
    EXPECT_EQ(flips.frame_mismatches, 0U);
    expect_flip_timing(flips);
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
    print_summary("TimeOnly 7d", sim, 7.0, watch.seconds());
}

// ---- Time + weather -------------------------------------------------------------------------

TEST(SimConnectivity, TimeWeatherSevenDaysFetchesHourlyAndSurvivesAFailedFetch) {
    const Stopwatch watch;
    Sim sim;
    sim.env.clock.set_true_utc_us(kStart);
    provision_nvs(sim.env, model::ConnectivityMode::kTimeWeather, true, true);
    // Scripted network: 30 good forecasts, one transport failure, then a new forecast (sticky).
    for (int i = 0; i < 30; ++i) {
        sim.env.net.script_http(200, kWeatherBody, 300);
    }
    sim.env.net.script_http_error(Error{Errc::kTimeout}, 500);
    sim.env.net.script_http(200, kWeatherBodyLast, 300);
    sim.cold_boot();
    ASSERT_TRUE(sim.env.api().time_info().valid);
    set_timezone(sim, "America/Chicago");

    FlipStats flips;
    bool saw_failed_indicator = false;
    sim.on_wake = [&](Sim& s, const WakeObs& o) {
        observe_flip(s, o, flips, 211);
        if (s.env.api().sync_info().indicator == model::SyncIndicator::kLastFailed) {
            saw_failed_indicator = true;
        }
    };
    script_daily_steps(sim, 7);
    sim.run_until(kStart + kWeek);

    // ~hourly weather (the interval defaults to 60 min), daily time sync, piggybacked.
    EXPECT_GE(sim.env.net.http_calls(), 150U);
    EXPECT_LE(sim.env.net.http_calls(), 175U);
    EXPECT_GE(sim.env.net.sntp_calls(), 7U);
    EXPECT_LE(sim.env.net.sntp_calls(), 9U);
    EXPECT_EQ(sim.env.net.violations(), 0U);
    EXPECT_FALSE(sim.env.net.radio_on());
    EXPECT_EQ(sim.env.net.shutdown_calls(), sim.env.net.connect_calls());
    EXPECT_TRUE(saw_failed_indicator) << "the failed fetch showed up as a sync failure";
    const console::WeatherInfo wx = sim.env.api().weather();
    EXPECT_EQ(wx.freshness, model::WeatherFreshness::kFresh);
    EXPECT_EQ(wx.report.temp_dc, 200) << "the last scripted forecast is the one on screen";
    EXPECT_EQ(sim.env.api().sync_info().fail_streak, 0);
    EXPECT_EQ(flips.frame_mismatches, 0U);
    expect_flip_timing(flips);
    EXPECT_EQ(sim.env.epd.violation_count(), 0U);
    print_summary("TimeWeather 7d", sim, 7.0, watch.seconds());
}

} // namespace
} // namespace qz::app
// NOLINTEND(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
