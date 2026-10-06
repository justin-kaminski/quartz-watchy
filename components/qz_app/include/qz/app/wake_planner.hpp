// Next-wake planning (ARCHITECTURE.md sections 8.4, 11, 4): minute-boundary alignment with a
// measured wake-latency lead, Saver cadence, Critical = no timer, wake sources, crash counter and
// safe mode. Pure and integer-only; all state lives in app::WakeTiming (RTC memory).
#pragma once

#include "qz/app/rtc_state.hpp"
#include "qz/board/watchy_v3.hpp"
#include "qz/hal/system.hpp"
#include "qz/model/types.hpp"
#include "qz/power/power.hpp"
#include "qz/time/timekeeper.hpp"

#include <cstdint>

namespace qz::app {

/// Tunables (one table; every value [TUNE] unless a source is given).
struct PlannerTuning {
    // ARCH 8.4: EWMA alpha 1/4, clamped 100..1500 ms. The initial value (350 ms) is the default
    // of WakeTiming::ewma_latency_us.
    std::int32_t latency_min_us = 100'000;
    std::int32_t latency_max_us = 1'500'000;
    std::int32_t latency_alpha_div = 4;
    /// A measured "latency" above this is not a wake latency (stale schedule, wrong cause): the
    /// sample is dropped instead of dragging the EWMA.
    std::int64_t latency_sample_max_us = 10'000'000;
    std::int64_t render_and_spi_us = 40'000; ///< wake -> panel update triggered [TUNE: B9]
    std::int64_t waveform_us = 350'000;      ///< partial update waveform (ARCH s20 ~350 ms) [TUNE]
    std::int64_t early_slack_us = 50'000;    ///< ARCH 8.4: light-sleep only if > 50 ms early
    std::int64_t max_light_sleep_us = 5'000'000; ///< longer waits are not planned in-process
    std::int64_t min_timer_us = 10'000; ///< shortest timer handed to the RTC [ASSUMED] [TUNE]
    std::int64_t invalid_time_tick_us = 600'000'000; ///< ARCH s4: ticks every 10 min
    std::int64_t minute_us = 60'000'000;
    // Safe mode (ARCH s4): >= 3 crashes within 10 min; until USB attach or 24 h.
    std::uint16_t crash_limit = 3;
    std::int64_t crash_window_us = 600'000'000;
    std::int64_t safe_mode_hold_us = 24LL * 3600 * 1'000'000;
};

/// What kind of wake the plan arms.
enum class WakeKind : std::uint8_t {
    kNone,         ///< no timer (Critical)
    kMinuteTick,   ///< aligned to a UTC period boundary minus the lead
    kInvalidTime,  ///< time invalid / implausible: relative housekeeping tick
    kSleepOverride ///< console `sleep <s>`: exact relative duration
};

struct PlanRequest {
    std::int64_t now_rtc_us = 0; ///< raw RTC "now"; call immediately before deep_sleep
    model::PowerLevel level = model::PowerLevel::kNormal;
    power::PolicyDecision decision{}; ///< display_period_min: 1 / 5 / 0 (no timer)
    bool tap_wake = false;            ///< user setting
    bool usb_wake = true;             ///< BuildFeatures::usb_wake
    /// USB VBUS reads present now. The USB wake is level-triggered: armed while VBUS is already
    /// high it would end the sleep immediately, so it is left off [ASSUMED: EXT0 is level
    /// triggered, ARCH s5].
    bool usb_present = false;
    std::int64_t sleep_override_us = 0; ///< > 0: console `sleep <s>`, exact duration in RTC us
};

struct WakePlan {
    hal::SleepPlan sleep;
    WakeKind kind = WakeKind::kNone;
    std::int64_t wake_rtc_us = 0;       ///< absolute RTC instant of the timer wake (0 = none)
    time::UnixMicros target_utc_us = 0; ///< boundary the tick wake is for (kMinuteTick only)
};

/// Not thread-safe. Holds references; the app owns the lifetimes.
class WakePlanner {
public:
    WakePlanner(WakeTiming& timing,
                const time::TimeKeeper& keeper,
                const PlannerTuning& tuning = {}) noexcept;

    // ---- wake latency (ARCH 8.4) ----
    /// First thing after a TIMER wake: latency = boot_rtc_us - scheduled_wake_rtc_us feeds the
    /// EWMA. Returns false (state untouched) when nothing was scheduled or the sample is
    /// negative / implausibly large. Call once per wake.
    bool on_timer_wake(std::int64_t boot_rtc_us) noexcept;
    /// Current wake-latency estimate, clamped to the tuning range.
    [[nodiscard]] std::int64_t latency_us() const noexcept;
    /// wake_at = boundary - lead_us(): latency + render/SPI + waveform / 2.
    [[nodiscard]] std::int64_t lead_us() const noexcept;

    // ---- the frame of a tick wake ----
    /// The UTC minute boundary the frame rendered now must show: the boundary whose wake time
    /// (boundary - lead) has already passed if the update would otherwise land before it,
    /// else the minute current at the update midpoint. Returns 0 when UTC is invalid/implausible.
    [[nodiscard]] time::UnixMicros tick_target_utc_us(std::int64_t now_rtc_us) const noexcept;
    /// Light-sleep time (raw RTC us) to hold a ready frame until target - waveform / 2; 0 =
    /// trigger now (not more than early_slack early, already late, or an implausible wait).
    [[nodiscard]] std::int64_t trigger_wait_us(std::int64_t now_rtc_us,
                                               time::UnixMicros target_utc_us) const noexcept;

    // ---- crash counter / safe mode (ARCH s4) ----
    /// A panic/watchdog reset was detected at this wake. Enters safe mode on the crash_limit-th
    /// crash within the window (fixed window opened by the first crash).
    void on_crash(std::int64_t now_rtc_us) noexcept;
    /// True while safe mode holds; clears it once safe_mode_hold_us has elapsed.
    [[nodiscard]] bool safe_mode(std::int64_t now_rtc_us) noexcept;
    /// USB attach ends safe mode and forgets the crash window.
    void on_usb_attach() noexcept;

    // ---- planning ----
    /// Plans the deep sleep and records the scheduled wake in WakeTiming (0 when no timer).
    [[nodiscard]] WakePlan plan(const PlanRequest& request) noexcept;

private:
    WakeTiming& timing_;
    const time::TimeKeeper& keeper_;
    PlannerTuning tuning_;
};

/// EXT0/EXT1 configuration the platform arms for a plan (qz_board).
[[nodiscard]] constexpr board::WakeConfig wake_config_for(const hal::SleepPlan& plan) noexcept {
    return board::wake_config(plan.wake_on_accel, plan.wake_on_usb);
}

} // namespace qz::app
