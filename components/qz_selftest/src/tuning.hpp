// Self-test tunables (ARCHITECTURE.md section 18). One table; [TUNE] = adjust after bring-up.
#pragma once

#include <cstdint>
#include <string_view>

namespace qz::selftest::tuning {

/// Battery voltage window for drivers/battery_adc [R1 s4: cell 3.0..4.4 V; ARCH s18].
inline constexpr std::int32_t kBatteryMinMv = 3000;
inline constexpr std::int32_t kBatteryMaxMv = 4400;
/// Mirror of board::kBatteryDividerNum/Den (qz_board is not a dependency of qz_selftest).
/// [TECH-DEBT] keep in sync until selftest may depend on qz_board.
inline constexpr std::uint16_t kBatteryDividerNum = 460;
inline constexpr std::uint16_t kBatteryDividerDen = 360;

/// Slow-clock check: external crystal, calibration within +-500 ppm of 32768 Hz [ARCH s18].
inline constexpr std::int64_t kSlowClockNominalHz = 32768;
inline constexpr std::int64_t kSlowClockTolerancePpm = 500;

/// Plausible SSD1681 internal-sensor range, deci-degrees C.
inline constexpr std::int16_t kPanelTempMinDc = -400;
inline constexpr std::int16_t kPanelTempMaxDc = 850;

/// NVS namespace every self-test write goes to (never a real namespace).
inline constexpr std::string_view kTestNamespace = "qz_test";

/// Interactive prompts: poll period and give-up time [TUNE].
inline constexpr std::uint32_t kPromptPollMs = 20;
inline constexpr std::uint32_t kPromptTimeoutMs = 10'000;
inline constexpr std::uint32_t kBuzzMs = 300; ///< vibration pulse for interactive/vibration

} // namespace qz::selftest::tuning
