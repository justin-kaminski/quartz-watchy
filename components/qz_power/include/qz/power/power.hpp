// Battery model and power policy (ARCHITECTURE.md section 11). Integer-only, pure.
#pragma once

#include "qz/model/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qz::power {

struct CurvePoint {
    std::uint16_t mv;
    std::uint8_t percent;
};

/// Default LiPo OCV curve, descending mV [TUNE: replace with bring-up B6 measurement].
[[nodiscard]] std::span<const CurvePoint> default_curve() noexcept;
/// Piecewise-linear interpolation, clamped to 0..100.
[[nodiscard]] std::uint8_t percent_from_mv(std::uint16_t mv,
                                           std::span<const CurvePoint> curve) noexcept;
/// Mean of samples after dropping one min and one max (needs >= 3 samples, else plain mean).
[[nodiscard]] std::uint16_t robust_mean_mv(std::span<const std::uint16_t> samples) noexcept;
/// Pin millivolts -> battery millivolts with the board divider.
[[nodiscard]] std::uint16_t
battery_mv_from_pin(std::uint16_t pin_mv, std::uint16_t num, std::uint16_t den) noexcept;

struct Thresholds {
    std::uint16_t low_enter_mv = 3600, low_exit_mv = 3700;
    std::uint16_t saver_enter_mv = 3500, saver_exit_mv = 3600;
    std::uint16_t critical_enter_mv = 3400, critical_exit_mv = 3600;
    std::uint32_t sample_period_s = 600; ///< battery sampled every 10 min (+ button wakes)
};

/// Persistent part, embedded in app::RtcState. Trivially copyable.
struct PowerState {
    std::uint16_t filtered_mv = 0;
    std::uint16_t fake_mv = 0; ///< console override, 0 = off
    model::PowerLevel level = model::PowerLevel::kNormal;
    std::uint8_t valid = 0;
    std::array<std::uint8_t, 2> reserved{};
    std::int64_t last_sample_rtc_us = 0;
    time::DayNumber stats_day = 0;
    std::uint32_t awake_ms_today = 0;
    std::array<std::uint16_t, model::kWakeCauseCount> wakes_today{};
    std::array<std::uint32_t, model::kWakeCauseCount> awake_ms_by_cause{};
};

/// Behaviour knobs derived from the level; the app consults these, never raw voltages.
struct PolicyDecision {
    bool radio_allowed = true;
    bool tap_wake_allowed = true;
    bool vibration_allowed = true;
    std::uint16_t display_period_min = 1;  ///< 1 normal/low, 5 saver, 0 = no timer (critical)
    std::uint16_t full_refresh_every = 60; ///< partial updates between full refreshes
    bool charge_me_screen = false;
};

class PowerPolicy {
public:
    PowerPolicy(PowerState& state, const Thresholds& thresholds) noexcept;
    /// New battery sample (already filtered by robust_mean_mv); applies EWMA (alpha 1/4) and
    /// hysteresis. USB present: Critical exits immediately. Returns the new level.
    model::PowerLevel
    on_sample(std::uint16_t battery_mv, bool usb_present, std::int64_t rtc_us) noexcept;
    [[nodiscard]] bool sample_due(std::int64_t rtc_us) const noexcept;
    [[nodiscard]] model::PowerLevel level() const noexcept;
    [[nodiscard]] PolicyDecision decision() const noexcept;
    [[nodiscard]] model::BatteryStatus status(bool usb_present, bool charging) const noexcept;
    /// Daily awake-time accounting (reset when local day changes).
    void record_wake(model::WakeCause cause, std::uint32_t awake_ms, time::DayNumber day) noexcept;

private:
    PowerState& state_;
    Thresholds thresholds_;
};

/// Battery-life estimate from measured constants (bring-up B9) and today's awake stats.
struct EstimateInputs {
    std::uint32_t capacity_mah = 200;
    std::uint32_t sleep_floor_ua = 50; ///< [TUNE]
    std::uint32_t active_ma = 25;      ///< [TUNE]
    std::uint32_t awake_ms_per_day = 0;
};
[[nodiscard]] std::uint32_t estimate_hours(const EstimateInputs& in) noexcept;

} // namespace qz::power
