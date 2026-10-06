// UI tunables (ARCHITECTURE.md section 15). One table; [TUNE] = calibrate by feel on hardware.
#pragma once

#include "qz/time/civil.hpp"

#include <cstddef>
#include <cstdint>

namespace qz::ui::tuning {

// ---- idle timeouts: no input for this long ends the interactive session ----
inline constexpr std::int64_t kIdleMenuMs = 30'000;   ///< any menu/editor (section 15)
inline constexpr std::int64_t kIdleFaceMs = 2'000;    ///< face, ChargeMe (section 15)
inline constexpr std::int64_t kIdleOverlayMs = 5'000; ///< StatusOverlay tap wake (section 15)
inline constexpr std::int64_t kIdleSyncMs =
    60'000; ///< SyncNow: covers the sync time budget [ASSUMED]
inline constexpr std::int64_t kIdleProvisioningMs =
    300'000; ///< phone-side setup takes minutes [ASSUMED]

// ---- navigation ----
inline constexpr std::size_t kStackDepth = 6; ///< Face, Menu, WeatherSettings, Location + slack

// ---- editors ----
inline constexpr std::uint32_t kStepGoalStep = 500;
inline constexpr std::uint32_t kStepGoalMax = 50'000;
inline constexpr std::uint32_t kAccelAfterMs = 2'000; ///< held this long: faster stepping [TUNE]
inline constexpr std::uint32_t kAccelFactor = 5;      ///< [TUNE]
inline constexpr std::int32_t kYearMin = 2025;        ///< [ASSUMED] earliest sensible manual date
inline constexpr std::int32_t kYearMax = 2099;
inline constexpr time::CivilDate kDefaultDate{2026, 1, 1}; ///< editor start while time is invalid
inline constexpr time::CivilTime kDefaultTime{12, 0, 0};

/// Location editor resolution: 0.01 degree (~1.1 km) is far finer than weather needs [ASSUMED].
inline constexpr std::int32_t kLocUnitE5 = 1000;     ///< 1e-5 degrees per editor unit
inline constexpr std::int32_t kLatMaxUnits = 9'000;  ///< 90.00
inline constexpr std::int32_t kLonMaxUnits = 18'000; ///< 180.00

// ---- system screens ----
inline constexpr std::uint32_t kFactoryResetHoldMs = 3'000; ///< hold MENU to confirm
inline constexpr std::uint8_t kDiagPageCount =
    6; ///< battery, time, sync, wakes, sensors, self-test

// ---- drawing (200x200) ----
inline constexpr std::int16_t kTitleBarH = 22;
inline constexpr std::int16_t kRowH = 20;
inline constexpr std::int16_t kHintBaseline = 197;

} // namespace qz::ui::tuning
