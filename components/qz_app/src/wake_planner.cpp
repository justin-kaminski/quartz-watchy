// Wake planning (ARCHITECTURE.md sections 8.4, 11, 4). Everything is UTC-microsecond integer
// math on minute boundaries; the local zone (and DST) never enters, so a zone change cannot move
// a wake. UTC deltas become raw RTC durations through TimeKeeper::rtc_at_utc (drift corrected).
#include "qz/app/wake_planner.hpp"

#include "qz/core/assert.hpp"

#include <algorithm>
#include <limits>

namespace qz::app {

namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
/// UTC beyond year 9999 (or before 1970) is treated as "time unusable": keeps all sums far from
/// overflow and refuses to align wakes to garbage.
constexpr std::int64_t kMaxSaneUtcUs = 253'402'300'799LL * 1'000'000;

constexpr std::int64_t kRoundingSlackUs = 1000; ///< drift-mapping rounding allowance
constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}
constexpr std::int64_t floor_to(std::int64_t x, std::int64_t period) noexcept {
    return floor_div(x, period) * period;
}
constexpr std::int64_t ceil_to(std::int64_t x, std::int64_t period) noexcept {
    return floor_div(x + period - 1, period) * period;
}
constexpr bool sane_utc(std::int64_t utc_us) noexcept {
    return utc_us >= 0 && utc_us <= kMaxSaneUtcUs;
}
/// a - b without signed overflow (saturating).
constexpr std::int64_t sat_sub(std::int64_t a, std::int64_t b) noexcept {
    if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (b < 0 && a > kInt64Max + b) {
        return kInt64Max;
    }
    return a - b;
}

} // namespace

WakePlanner::WakePlanner(WakeTiming& timing,
                         const time::TimeKeeper& keeper,
                         const PlannerTuning& tuning) noexcept
    : timing_(timing), keeper_(keeper), tuning_(tuning) {
    QZ_ASSERT(tuning_.latency_min_us > 0 && tuning_.latency_min_us <= tuning_.latency_max_us);
    QZ_ASSERT(tuning_.latency_alpha_div >= 1);
    QZ_ASSERT(tuning_.minute_us > 0 && tuning_.min_timer_us > 0);
    QZ_ASSERT(tuning_.waveform_us >= 0 && tuning_.render_and_spi_us >= 0);
}

std::int64_t WakePlanner::latency_us() const noexcept {
    return std::clamp<std::int64_t>(
        timing_.ewma_latency_us, tuning_.latency_min_us, tuning_.latency_max_us);
}

std::int64_t WakePlanner::lead_us() const noexcept {
    return latency_us() + tuning_.render_and_spi_us + (tuning_.waveform_us / 2);
}

bool WakePlanner::on_timer_wake(std::int64_t boot_rtc_us) noexcept {
    const std::int64_t scheduled = timing_.scheduled_wake_rtc_us;
    if (scheduled <= 0) {
        return false;
    }
    timing_.scheduled_wake_rtc_us = 0; // consumed: one sample per scheduled wake
    if (boot_rtc_us < scheduled) {
        return false; // woke "before" the schedule: not a latency measurement
    }
    const std::int64_t sample = boot_rtc_us - scheduled;
    if (sample > tuning_.latency_sample_max_us) {
        return false;
    }
    const std::int64_t target =
        std::clamp<std::int64_t>(sample, tuning_.latency_min_us, tuning_.latency_max_us);
    const std::int64_t current = latency_us();
    const std::int64_t diff = target - current;
    std::int64_t step = diff / tuning_.latency_alpha_div;
    if (step == 0 && diff != 0) {
        step = diff > 0 ? 1 : -1; // exact convergence instead of stalling within alpha_div us
    }
    timing_.ewma_latency_us = static_cast<std::int32_t>(current + step);
    return true;
}

time::UnixMicros WakePlanner::tick_target_utc_us(std::int64_t now_rtc_us) const noexcept {
    if (!keeper_.valid()) {
        return 0;
    }
    const time::UnixMicros now_utc = keeper_.utc_at_rtc(now_rtc_us);
    if (!sane_utc(now_utc)) {
        return 0;
    }
    // Update midpoint if the panel were triggered as soon as the frame is rendered.
    const std::int64_t midpoint = now_utc + tuning_.render_and_spi_us + (tuning_.waveform_us / 2);
    const std::int64_t next = ceil_to(midpoint, tuning_.minute_us);
    if (next - lead_us() <= now_utc) {
        // The timer wake for `next` would be due already (a deep-sleep round trip cannot make
        // it): show that minute and hold the update until its boundary.
        return next;
    }
    return floor_to(midpoint, tuning_.minute_us);
}

std::int64_t WakePlanner::trigger_wait_us(std::int64_t now_rtc_us,
                                          time::UnixMicros target_utc_us) const noexcept {
    if (target_utc_us <= 0) {
        return 0;
    }
    const std::int64_t trigger_rtc = keeper_.rtc_at_utc(target_utc_us - (tuning_.waveform_us / 2));
    const std::int64_t wait = sat_sub(trigger_rtc, now_rtc_us);
    if (wait > tuning_.early_slack_us && wait <= tuning_.max_light_sleep_us) {
        return wait;
    }
    return 0;
}

void WakePlanner::on_crash(std::int64_t now_rtc_us) noexcept {
    const auto bump = [this]() noexcept {
        if (timing_.crash_count_window < std::numeric_limits<std::uint16_t>::max()) {
            ++timing_.crash_count_window;
        }
    };
    if (safe_mode(now_rtc_us)) {
        bump(); // already safe: keep the entry time (24 h reference)
        return;
    }
    const bool window_open =
        timing_.crash_count_window != 0 && now_rtc_us >= timing_.crash_window_start_rtc_us &&
        sat_sub(now_rtc_us, timing_.crash_window_start_rtc_us) <= tuning_.crash_window_us;
    if (window_open) {
        bump();
    } else {
        timing_.crash_window_start_rtc_us = now_rtc_us;
        timing_.crash_count_window = 1;
    }
    if (timing_.crash_count_window >= tuning_.crash_limit) {
        timing_.safe_mode = 1;
        timing_.crash_window_start_rtc_us = now_rtc_us; // entry time while safe_mode != 0
    }
}

bool WakePlanner::safe_mode(std::int64_t now_rtc_us) noexcept {
    if (timing_.safe_mode == 0) {
        return false;
    }
    const std::int64_t entered = timing_.crash_window_start_rtc_us;
    // now < entered: the RTC restarted since (power loss would also have invalidated this state).
    if (now_rtc_us < entered || sat_sub(now_rtc_us, entered) >= tuning_.safe_mode_hold_us) {
        timing_.safe_mode = 0;
        timing_.crash_count_window = 0;
        timing_.crash_window_start_rtc_us = 0;
        return false;
    }
    return true;
}

void WakePlanner::on_usb_attach() noexcept {
    timing_.safe_mode = 0;
    timing_.crash_count_window = 0;
    timing_.crash_window_start_rtc_us = 0;
}

WakePlan WakePlanner::plan(const PlanRequest& request) noexcept {
    WakePlan out;
    const bool critical = request.level == model::PowerLevel::kCritical;
    const bool safe = safe_mode(request.now_rtc_us);

    // Wake sources. Buttons always (the only way out of Critical besides USB).
    out.sleep.wake_on_buttons = true;
    out.sleep.wake_on_accel =
        request.tap_wake && request.decision.tap_wake_allowed && !critical && !safe;
    out.sleep.wake_on_usb = request.usb_wake && !request.usb_present;
    out.sleep.wake_on_epd_idle = false; // light sleep only
    out.sleep.timer_us = -1;

    std::int64_t timer_us = -1;
    if (request.sleep_override_us > 0) {
        out.kind = WakeKind::kSleepOverride;
        timer_us = std::max(request.sleep_override_us, tuning_.min_timer_us);
    } else if (critical || request.decision.display_period_min == 0) {
        out.kind = WakeKind::kNone; // Critical: buttons + USB only
    } else {
        const std::int64_t period_us =
            static_cast<std::int64_t>(request.decision.display_period_min) * tuning_.minute_us;
        const time::UnixMicros now_utc = keeper_.utc_at_rtc(request.now_rtc_us);
        bool aligned = false;
        if (keeper_.valid() && sane_utc(now_utc)) {
            const std::int64_t lead = lead_us();
            const std::int64_t boundary = ceil_to(now_utc + tuning_.min_timer_us, period_us);
            const std::int64_t wake_rtc = keeper_.rtc_at_utc(boundary - lead);
            const std::int64_t raw = sat_sub(wake_rtc, request.now_rtc_us);
            // raw >= min_timer - lead (+- rounding) by construction and <= ~period; anything
            // else means the time base is inconsistent: do not trust it for alignment.
            if (raw >= tuning_.min_timer_us - lead - kRoundingSlackUs && raw <= 2 * period_us) {
                aligned = true;
                out.kind = WakeKind::kMinuteTick;
                out.target_utc_us = boundary;
                timer_us = std::max(raw, tuning_.min_timer_us);
            }
        }
        if (!aligned) {
            out.kind = WakeKind::kInvalidTime;
            timer_us = tuning_.invalid_time_tick_us;
        }
    }

    out.sleep.timer_us = timer_us;
    out.wake_rtc_us = timer_us > 0 ? request.now_rtc_us + timer_us : 0;
    timing_.scheduled_wake_rtc_us = out.wake_rtc_us;
    return out;
}

} // namespace qz::app
