// Power policy: level state machine with hysteresis, status, per-level decisions and daily awake
// accounting (ARCHITECTURE.md section 11). Pure and integer-only: no heap, no exceptions.
//
// Known limitation: the filter is not re-seeded when USB is plugged or unplugged (PowerState has no
// field for the previous USB state). A charge-lifted reading therefore decays out of the filter
// over a few samples (alpha 1/4, 10 min apart) after unplugging.
#include "qz/core/assert.hpp"
#include "qz/power/power.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace qz::power {
namespace {

using model::PowerLevel;

constexpr std::int64_t kUsPerSecond = 1'000'000;

/// The state lives in RTC memory: an out-of-range level (corruption, a layout change) reads as
/// Normal rather than indexing past the policy table.
constexpr PowerLevel sane_level(PowerLevel level) noexcept {
    return level <= PowerLevel::kCritical ? level : PowerLevel::kNormal;
}

/// Deeper levels engage at lower voltages, and every level has a hysteresis band (exit above
/// enter); without a band the level would flap on a single noisy sample.
constexpr bool thresholds_valid(const Thresholds& t) noexcept {
    return t.critical_enter_mv <= t.saver_enter_mv && t.saver_enter_mv <= t.low_enter_mv &&
           t.low_enter_mv < t.low_exit_mv && t.saver_enter_mv < t.saver_exit_mv &&
           t.critical_enter_mv < t.critical_exit_mv;
}

/// One filter step with alpha = 1 / kEwmaDivisor. The step is rounded away from the old value, so a
/// constant input is reached exactly (instead of stalling a few mV short) and never overshot.
constexpr std::uint16_t ewma_step(std::uint16_t filtered_mv, std::uint16_t sample_mv) noexcept {
    constexpr std::uint32_t kRoundUp = detail::kEwmaDivisor - 1U;
    const std::uint32_t old_mv = filtered_mv;
    if (sample_mv >= filtered_mv) {
        const std::uint32_t gap = static_cast<std::uint32_t>(sample_mv) - old_mv;
        return static_cast<std::uint16_t>(old_mv + ((gap + kRoundUp) / detail::kEwmaDivisor));
    }
    const std::uint32_t gap = old_mv - static_cast<std::uint32_t>(sample_mv);
    return static_cast<std::uint16_t>(old_mv - ((gap + kRoundUp) / detail::kEwmaDivisor));
}

/// Does `level` apply at `mv`? A level we are already in (or below) is kept until `exit_mv` is
/// reached; a shallower one is entered at `enter_mv`. The hysteresis is the gap between the two.
constexpr bool level_holds(PowerLevel current,
                           PowerLevel level,
                           std::uint16_t mv,
                           std::uint16_t enter_mv,
                           std::uint16_t exit_mv) noexcept {
    return current >= level ? mv < exit_mv : mv <= enter_mv;
}

/// New level for a filtered voltage: the deepest level that applies. Levels can be skipped in
/// either direction (a sudden drop goes straight to Critical; Critical leaves to Low or Normal,
/// never via Saver, because Saver and Critical share the exit voltage). USB present: Critical is
/// left at once and not entered again; the charger lifts the terminal voltage, so a reading taken
/// then says little about the cell [ARCH s11].
constexpr PowerLevel
next_level(const Thresholds& t, PowerLevel current, std::uint16_t mv, bool usb_present) noexcept {
    if (!usb_present &&
        level_holds(current, PowerLevel::kCritical, mv, t.critical_enter_mv, t.critical_exit_mv)) {
        return PowerLevel::kCritical;
    }
    if (level_holds(current, PowerLevel::kSaver, mv, t.saver_enter_mv, t.saver_exit_mv)) {
        return PowerLevel::kSaver;
    }
    if (level_holds(current, PowerLevel::kLow, mv, t.low_enter_mv, t.low_exit_mv)) {
        return PowerLevel::kLow;
    }
    return PowerLevel::kNormal;
}

/// Nearest multiple of `step`, halves up, at most 100.
constexpr std::uint8_t round_to_step(std::uint8_t percent, std::uint8_t step) noexcept {
    const std::uint32_t unit = step;
    const std::uint32_t rounded =
        ((static_cast<std::uint32_t>(percent) + (unit / 2U)) / unit) * unit;
    return static_cast<std::uint8_t>(std::min<std::uint32_t>(rounded, 100U));
}

/// The percentage shown to the user [ARCH s11]: 5 % steps up to kCoarsePercentAboveMv, 10 % steps
/// above (uncalibrated ADC range). Above the boundary it never drops below the value shown at the
/// boundary, so the figure is monotonic in the voltage (nearest-10 of 13 % would dip under the
/// nearest-5 of the same 13 %).
std::uint8_t display_percent(std::uint16_t mv) noexcept {
    const std::uint8_t exact = percent_from_mv(mv, default_curve());
    if (mv <= detail::kCoarsePercentAboveMv) {
        return round_to_step(exact, detail::kFinePercentStep);
    }
    const std::uint8_t at_boundary = round_to_step(
        percent_from_mv(detail::kCoarsePercentAboveMv, default_curve()), detail::kFinePercentStep);
    return std::max(round_to_step(exact, detail::kCoarsePercentStep), at_boundary);
}

constexpr std::uint32_t saturating_add(std::uint32_t a, std::uint32_t b) noexcept {
    return a > std::numeric_limits<std::uint32_t>::max() - b
               ? std::numeric_limits<std::uint32_t>::max()
               : a + b;
}

/// A cause outside the enum (corrupt caller value) is counted as kUnknown.
constexpr std::size_t cause_slot(model::WakeCause cause) noexcept {
    const auto slot = static_cast<std::size_t>(cause);
    return slot < model::kWakeCauseCount ? slot
                                         : static_cast<std::size_t>(model::WakeCause::kUnknown);
}

} // namespace

PowerPolicy::PowerPolicy(PowerState& state, const Thresholds& thresholds) noexcept
    : state_(state), thresholds_(thresholds) {
    QZ_ASSERT(thresholds_valid(thresholds_));
}

PowerLevel
PowerPolicy::on_sample(std::uint16_t battery_mv, bool usb_present, std::int64_t rtc_us) noexcept {
    if (battery_mv < detail::kMinPlausibleMv) {
        // No usable reading: keep level and filter. A failed attempt neither postpones nor
        // satisfies the schedule (last_sample_rtc_us stays), so a due sample is retried next time.
        return level();
    }
    state_.filtered_mv = state_.valid == 0 ? battery_mv : ewma_step(state_.filtered_mv, battery_mv);
    state_.valid = 1;
    state_.last_sample_rtc_us = rtc_us;
    // The console override replaces the voltage that is evaluated and reported; the real filter
    // keeps tracking real readings, so clearing the override needs no re-seeding.
    const std::uint16_t evaluated_mv = state_.fake_mv != 0 ? state_.fake_mv : state_.filtered_mv;
    state_.level = next_level(thresholds_, level(), evaluated_mv, usb_present);
    return state_.level;
}

bool PowerPolicy::sample_due(std::int64_t rtc_us) const noexcept {
    if (state_.valid == 0) {
        return true;
    }
    if (rtc_us < state_.last_sample_rtc_us) {
        return true; // the RTC clock restarted: do not wait for a time that already passed
    }
    // rtc_us is a raw RTC counter (>= 0), so the difference cannot overflow.
    const std::int64_t period_us =
        static_cast<std::int64_t>(thresholds_.sample_period_s) * kUsPerSecond;
    return rtc_us - state_.last_sample_rtc_us >= period_us;
}

PowerLevel PowerPolicy::level() const noexcept {
    return sane_level(state_.level);
}

PolicyDecision PowerPolicy::decision() const noexcept {
    return detail::kLevelPolicy[static_cast<std::size_t>(level())];
}

model::BatteryStatus PowerPolicy::status(bool usb_present, bool charging) const noexcept {
    const bool faked = state_.fake_mv != 0;
    const std::uint16_t mv = faked ? state_.fake_mv : state_.filtered_mv;
    model::BatteryStatus out;
    out.mv = mv;
    out.percent = display_percent(mv);
    out.level = level();
    out.usb_present = usb_present;
    out.charging = usb_present && charging; // GPIO10 is high with USB present, charging or full
    out.valid = state_.valid != 0;
    out.faked = faked;
    return out;
}

void PowerPolicy::record_wake(model::WakeCause cause,
                              std::uint32_t awake_ms,
                              time::DayNumber day) noexcept {
    if (state_.stats_day != day) {
        state_.stats_day = day;
        state_.awake_ms_today = 0;
        state_.wakes_today.fill(0);
        state_.awake_ms_by_cause.fill(0);
    }
    const std::size_t slot = cause_slot(cause);
    state_.awake_ms_today = saturating_add(state_.awake_ms_today, awake_ms);
    state_.awake_ms_by_cause[slot] = saturating_add(state_.awake_ms_by_cause[slot], awake_ms);
    if (state_.wakes_today[slot] < std::numeric_limits<std::uint16_t>::max()) {
        ++state_.wakes_today[slot];
    }
}

} // namespace qz::power
