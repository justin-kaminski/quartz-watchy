// Tunables and fixed limits of TimeKeeper and the ISO-8601 parser (ARCHITECTURE.md section 8).
// The drift policy itself (min interval, residual limit, drift clamp, gain) is the public
// DriftPolicy in timekeeper.hpp; this table holds what is not caller-configurable.
#pragma once

#include <cstdint>

namespace qz::time::tuning {

/// Parts per billion in one whole: drift_ppb scales elapsed microseconds by (1 + ppb / 1e9).
inline constexpr std::int64_t kPpbScale = 1'000'000'000;

/// Hard bound on |drift_ppb| used by the time mapping whatever the state holds (RTC memory is
/// untrusted until validated). Keeps the rate 1e9 + drift in [5e8, 1.5e9], so the mapping is
/// invertible and every intermediate product stays inside int64. DriftPolicy::max_drift_ppb
/// (200 ppm by default) is the much tighter operating clamp.
inline constexpr std::int32_t kHardDriftLimitPpb = 500'000'000;

/// While no drift estimate exists yet (drift_ppb == 0) the first eligible sync applies the whole
/// residual instead of residual / DriftPolicy::gain_divisor. With the specified gain of 1/2 alone
/// a 50 ppm error would still be 12.5 ppm off after the third sync; the bootstrap step is what
/// makes "within +-1 ppm in 3 syncs" (ROADMAP WP-04) reachable. [TUNE] confirm on hardware.
inline constexpr std::int32_t kBootstrapGainDivisor = 1;

/// A residual beyond this is a clock fault outright (and keeps residual * 1e9 inside int64):
/// 9e9 us = 2.5 h, versus the 14.4 s that is already 500 ppm over the 8 h minimum window.
inline constexpr std::int64_t kMaxResidualForPpbUs = 9'000'000'000;

/// parse_iso8601 accepts instants from 1970-01-01T00:00:00Z up to (excluding) the start of this
/// year; the same window as is_valid() (years 1970..2199).
inline constexpr std::int32_t kIsoEndYear = 2200;

/// RFC 3339 numeric offsets: hours 00..23, minutes 00..59.
inline constexpr std::int32_t kIsoMaxOffsetHours = 23;
inline constexpr std::int32_t kIsoMaxOffsetMinutes = 59;

} // namespace qz::time::tuning
