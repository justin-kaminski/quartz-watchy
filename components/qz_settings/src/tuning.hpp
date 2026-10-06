// Settings tunables and storage vocabulary (ARCHITECTURE.md sections 7 and 13).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qz::settings::tuning {

// ---- value limits (SPEC / ARCHITECTURE section 7) ----
inline constexpr std::int64_t kLatMaxE5 = 9'000'000;   ///< +-90 degrees in 1e-5 deg
inline constexpr std::int64_t kLonMaxE5 = 18'000'000;  ///< +-180 degrees in 1e-5 deg
inline constexpr std::int64_t kStepGoalMax = 50'000;   ///< steps; 0 = goal off
inline constexpr std::int64_t kStepGoalMultiple = 500; ///< steps
inline constexpr std::int64_t kFaceIdMax = 255;        ///< uint8_t
inline constexpr std::int64_t kDegreeScale = 100'000;  ///< 1e-5 degrees per degree
inline constexpr std::size_t kDegreeFractionDigits = 5;
inline constexpr std::size_t kDegreeIntegerDigitsMax = 3;
inline constexpr std::size_t kUIntDigitsMax = 10; ///< digits of UINT32_MAX

// ---- credentials: IEEE 802.11 / WPA2-PSK (R1: SSID <= 32 octets, passphrase 8..63 chars or a
// 64-digit hex PSK; empty passphrase = open network, handled by the app policy) ----
inline constexpr std::size_t kSsidMax = 32;
inline constexpr std::size_t kPassphraseMin = 8;
inline constexpr std::size_t kPassphraseMax = 63;
inline constexpr std::size_t kPskHexLen = 64;

// ---- NVS key names outside the schema (ARCHITECTURE section 7) ----
inline constexpr std::string_view kVerKey = "ver";
inline constexpr std::string_view kTzPosixKey = "tzposix";
inline constexpr std::string_view kSsidKey = "ssid";
inline constexpr std::string_view kPassKey = "pass";

/// Every namespace erased by a factory reset (ARCHITECTURE section 7 table).
inline constexpr std::array<std::string_view, 5> kAllNamespaces{
    "qz_set", "qz_cred", "qz_steps", "qz_time", "qz_diag"};

} // namespace qz::settings::tuning
