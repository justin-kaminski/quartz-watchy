// Tunables of qz_power (one constexpr table per component, ARCHITECTURE.md section 2).
// Private to the component: not installed, not part of the contract.
//
// Tags: [R1 sN] docs/research/hardware.md section N; [ARCH sN] docs/ARCHITECTURE.md section N;
// [TUNE] calibrate on hardware (HARDWARE_BRINGUP.md: B5 display refresh, B6 battery curve and
// thresholds, B9 power measurements); [ASSUMED] no source or measurement behind it yet.
// Nothing in this file counts as hardware-verified until the owner has run those bring-up steps.
#pragma once

#include "qz/power/power.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qz::power::detail {

// ---- LiPo curve (default_curve, percent_from_mv) ---------------------------------------------

/// A usable curve: at least two points, strictly descending mV, non-increasing percent.
constexpr bool curve_is_valid(std::span<const CurvePoint> curve) noexcept {
    if (curve.size() < 2) {
        return false;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (curve[i - 1].mv <= curve[i].mv || curve[i - 1].percent < curve[i].percent) {
            return false;
        }
    }
    return true;
}

/// Open-circuit-voltage curve of one LiPo cell: 11 points, descending, 100 % at the top and 0 % at
/// the bottom [ARCH s11: 3300..4200 mV].
/// [ASSUMED] a generic textbook single-cell shape; nothing here was measured on the 402030 cell.
/// [TUNE] replace with the curve measured in bring-up B6. Above 3706 mV the ADC is outside its
/// calibrated range [R1 s4], so the upper points stay uncharacterised until B6. The board has no
/// protection IC [R1 s3]: 0 % at 3300 mV means "charge now", not "cell empty" (cutoff 2.75 V).
inline constexpr std::array<CurvePoint, 11> kDefaultCurve = {{
    {.mv = 4200, .percent = 100},
    {.mv = 4110, .percent = 90},
    {.mv = 4020, .percent = 80},
    {.mv = 3950, .percent = 70},
    {.mv = 3870, .percent = 60},
    {.mv = 3840, .percent = 50},
    {.mv = 3800, .percent = 40},
    {.mv = 3770, .percent = 30},
    {.mv = 3730, .percent = 20},
    {.mv = 3690, .percent = 10},
    {.mv = 3300, .percent = 0},
}};
static_assert(curve_is_valid(kDefaultCurve), "curve: strictly descending mV, non-increasing %");
static_assert(kDefaultCurve.front().percent == 100 && kDefaultCurve.back().percent == 0,
              "curve must span 100 % .. 0 %");

/// status() rounds the percentage to 5 % up to this voltage and to 10 % above it [ARCH s11]. The
/// ADC is calibrated up to 2900 mV at the pin = 3706 mV at the battery [R1 s4].
inline constexpr std::uint16_t kCoarsePercentAboveMv = 3700;
inline constexpr std::uint8_t kFinePercentStep = 5;
inline constexpr std::uint8_t kCoarsePercentStep = 10;

// ---- Measurement filter ----------------------------------------------------------------------

/// robust_mean_mv drops one min and one max from sets of at least this many samples [ARCH s11:
/// 16 samples, drop min/max, mean]. Smaller sets use the plain mean.
inline constexpr std::size_t kRobustMeanMinSamples = 3;

/// EWMA across samples, alpha = 1 / kEwmaDivisor [ARCH s11: 1/4].
inline constexpr std::uint32_t kEwmaDivisor = 4;

/// A reading below this cannot come from a cell that is powering the SoC (it needs 3.0 V [R1 s3]):
/// it is a failed measurement (ADC error, empty sample set), not a battery state, and on_sample()
/// ignores it. Without this, one failed read would enter Critical, which has no timer wake.
/// [ASSUMED]
inline constexpr std::uint16_t kMinPlausibleMv = 2000;

// ---- Level thresholds and sampling [ARCH s11] ------------------------------------------------
// Mirrors of the defaults of Thresholds in power.hpp (the app may override them). The
// static_asserts below fail the build if the two ever drift apart.

inline constexpr std::uint16_t kLowEnterMv = 3600;      ///< [TUNE B6]
inline constexpr std::uint16_t kLowExitMv = 3700;       ///< [TUNE B6]
inline constexpr std::uint16_t kSaverEnterMv = 3500;    ///< [TUNE B6]
inline constexpr std::uint16_t kSaverExitMv = 3600;     ///< [TUNE B6]
inline constexpr std::uint16_t kCriticalEnterMv = 3400; ///< [TUNE B6]
inline constexpr std::uint16_t kCriticalExitMv = 3600;  ///< [TUNE B6], or USB present
inline constexpr std::uint32_t kSamplePeriodS = 600;    ///< every 10 min and on button wakes

/// Every hysteresis band must be at least this wide: five times the +-20 mV noise that the filter
/// has to ride out without flapping (ROADMAP WP-11). A B6 re-tune that narrows a band fails here.
inline constexpr std::uint16_t kMinBandMv = 100;

static_assert(Thresholds{}.low_enter_mv == kLowEnterMv && Thresholds{}.low_exit_mv == kLowExitMv &&
                  Thresholds{}.saver_enter_mv == kSaverEnterMv &&
                  Thresholds{}.saver_exit_mv == kSaverExitMv &&
                  Thresholds{}.critical_enter_mv == kCriticalEnterMv &&
                  Thresholds{}.critical_exit_mv == kCriticalExitMv &&
                  Thresholds{}.sample_period_s == kSamplePeriodS,
              "tuning.hpp and Thresholds{} in power.hpp disagree");
static_assert(kLowExitMv - kLowEnterMv >= kMinBandMv, "Low hysteresis band too narrow");
static_assert(kSaverExitMv - kSaverEnterMv >= kMinBandMv, "Saver hysteresis band too narrow");
static_assert(kCriticalExitMv - kCriticalEnterMv >= kMinBandMv,
              "Critical hysteresis band too narrow");

// ---- Behaviour per level (decision) [ARCH s11, s14] ------------------------------------------

/// Partial updates between full refreshes in Normal and Low [ARCH s14, OPEN_QUESTIONS Q-02: N = 30;
/// vendor guidance is 5]. [TUNE B5] Note: PolicyDecision{} in power.hpp defaults to 60 (stale).
inline constexpr std::uint16_t kFullRefreshEveryNormal = 30;
/// Saver: face updates every 5 min [ARCH s11, OPEN_QUESTIONS Q-07] and a full refresh only every
/// 4 h, i.e. every 48th update.
inline constexpr std::uint16_t kSaverDisplayPeriodMin = 5;
inline constexpr std::uint16_t kSaverFullRefreshPeriodMin = 4 * 60;
static_assert(kSaverFullRefreshPeriodMin % kSaverDisplayPeriodMin == 0,
              "the 4 h full-refresh interval must be a whole number of Saver updates");
inline constexpr std::uint16_t kSaverFullRefreshEvery =
    kSaverFullRefreshPeriodMin / kSaverDisplayPeriodMin;
/// Critical shows the Charge-me screen once and a banner face on a button press: every update on
/// that path is a full refresh (1 = every update) [ARCH s11].
inline constexpr std::uint16_t kCriticalFullRefreshEvery = 1;

inline constexpr std::size_t kLevelCount = 4;

/// Indexed by model::PowerLevel (Normal, Low, Saver, Critical).
inline constexpr std::array<PolicyDecision, kLevelCount> kLevelPolicy = {{
    // Normal: everything per settings.
    {.radio_allowed = true,
     .tap_wake_allowed = true,
     .vibration_allowed = true,
     .display_period_min = 1,
     .full_refresh_every = kFullRefreshEveryNormal,
     .charge_me_screen = false},
    // Low: no radio sessions (incl. "Sync now"), tap wake and vibration off.
    {.radio_allowed = false,
     .tap_wake_allowed = false,
     .vibration_allowed = false,
     .display_period_min = 1,
     .full_refresh_every = kFullRefreshEveryNormal,
     .charge_me_screen = false},
    // Saver: Low, plus a 5-minute display cadence and rare full refreshes.
    {.radio_allowed = false,
     .tap_wake_allowed = false,
     .vibration_allowed = false,
     .display_period_min = kSaverDisplayPeriodMin,
     .full_refresh_every = kSaverFullRefreshEvery,
     .charge_me_screen = false},
    // Critical: no display timer (deep sleep, button + USB wake only), Charge-me screen.
    {.radio_allowed = false,
     .tap_wake_allowed = false,
     .vibration_allowed = false,
     .display_period_min = 0,
     .full_refresh_every = kCriticalFullRefreshEvery,
     .charge_me_screen = true},
}};
static_assert(kLevelCount == static_cast<std::size_t>(model::PowerLevel::kCritical) + 1U,
              "kLevelPolicy needs one row per PowerLevel");

// ---- Energy model (estimate_hours) [ARCH s11, s20] -------------------------------------------

/// Unit conversions (derived, not tunable).
inline constexpr std::uint64_t kUaPerMa = 1000;
inline constexpr std::uint64_t kMsPerDay = 86'400'000;
/// Awake mA*ms per day that average to 1 uA over the day: 1 uA x 86.4e6 ms = 86'400 mA*ms.
inline constexpr std::uint64_t kMaMsPerDayPerUa = kMsPerDay / kUaPerMa;

/// Reference constants for the estimate. The defaults of EstimateInputs in power.hpp mirror the
/// sleep floor and active current (checked below); the app overrides them from bring-up B9.
inline constexpr std::uint32_t kSleepFloorUa =
    50; ///< [TUNE B9] [ASSUMED] 35-55 uA with the BMA423 step counter running [R1 s13]
inline constexpr std::uint32_t kActiveMa =
    25; ///< [TUNE B9] [ASSUMED] minute wake at 80 MHz [ARCH s20]
/// Cell capacity for estimates: the cell datasheet minimum, not the marketing figure [R1 s3, ARCH
/// s11]. Note: EstimateInputs{}.capacity_mah is 200 in power.hpp; callers must pass this value.
inline constexpr std::uint32_t kCellCapacityMinMah = 170;

static_assert(EstimateInputs{}.sleep_floor_ua == kSleepFloorUa &&
                  EstimateInputs{}.active_ma == kActiveMa,
              "tuning.hpp and EstimateInputs{} in power.hpp disagree");
static_assert(kCellCapacityMinMah <= EstimateInputs{}.capacity_mah,
              "estimates must not assume more than the cell minimum");

} // namespace qz::power::detail
