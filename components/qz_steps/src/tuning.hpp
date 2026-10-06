// Step-tracking tunables and storage vocabulary (ARCHITECTURE.md sections 7 and 10).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qz::steps::tuning {

/// A first sample after local midnight whose previous reading is at most this old belongs to the
/// day that just ended (ARCHITECTURE section 10: "gap is <= 2 min"). [TUNE]
inline constexpr std::int64_t kBoundaryGraceS = 120;

// ---- NVS namespace qz_steps (ARCHITECTURE section 7) ----
inline constexpr std::string_view kNamespace = "qz_steps";
inline constexpr std::string_view kVerKey = "ver";
inline constexpr std::string_view kHistKey = "hist";
inline constexpr std::string_view kTodayKey = "today";
inline constexpr std::uint32_t kSchemaVersion = 1;

/// Blob entry: {day i32 LE, steps u32 LE}.
inline constexpr std::size_t kEntryBytes = 8;
/// Empty history slots carry this day number so the fixed 7-entry blob also encodes the count.
inline constexpr std::int32_t kEmptyDay = INT32_MIN;

} // namespace qz::steps::tuning
