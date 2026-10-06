// qz_conn tunables (ARCHITECTURE.md sections 12 and 13). One table; no magic numbers in logic.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qz::conn::tuning {

// ---- scheduling (section 12) ----
inline constexpr std::int64_t kBackoffBaseS = std::int64_t{15} * 60; ///< first retry delay (spec)
inline constexpr std::int64_t kBackoffCapMaxS = std::int64_t{12} * 3600; ///< absolute cap (spec)
inline constexpr std::int32_t kJitterPermille = 100;                     ///< +-10 % (spec)
inline constexpr std::uint8_t kMaxBackoffShift = 16; ///< keeps 2^n far from overflow
inline constexpr std::int64_t kPiggybackWindowS =
    std::int64_t{2} * 3600;                                  ///< other job due within 2 h (spec)
inline constexpr std::int64_t kMinIntervalS = kBackoffBaseS; ///< defensive clamp for intervals
inline constexpr std::int64_t kStaleIntervals = 2;           ///< [ASSUMED] stale after 2 intervals

// ---- session ----
/// SNTP answers earlier than 2025-01-01 are rejected as corrupt (server or fake returned an
/// epoch-ish value). [ASSUMED] firmware is never run with a real date before this.
inline constexpr std::int64_t kMinPlausibleUtcUs = std::int64_t{1'735'689'600} * 1'000'000;

// ---- provisioning (section 13) ----
inline constexpr std::int64_t kProvisionTimeoutUs =
    std::int64_t{5} * 60 * 1'000'000; ///< 5 min (spec)
inline constexpr std::size_t kTokenLength = 10;
inline constexpr std::size_t kDecodeBufBytes = 128; ///< decoded size of any single field

} // namespace qz::conn::tuning
