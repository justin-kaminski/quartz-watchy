// Calendar math on 64-bit time (proleptic Gregorian, no leap seconds). Pure, reentrant.
#pragma once

#include "qz/core/result.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::time {

using UnixSeconds = std::int64_t; ///< seconds since 1970-01-01T00:00:00Z
using UnixMicros = std::int64_t;  ///< microseconds since 1970-01-01T00:00:00Z
using DayNumber = std::int32_t;   ///< civil day index, 0 = 1970-01-01 (local or UTC per context)

inline constexpr std::int64_t kUsPerSecond = 1'000'000;
inline constexpr std::int64_t kSecondsPerMinute = 60;
inline constexpr std::int64_t kSecondsPerDay = 86'400;

enum class Weekday : std::uint8_t {
    kSunday = 0,
    kMonday,
    kTuesday,
    kWednesday,
    kThursday,
    kFriday,
    kSaturday
};
enum class HourFormat : std::uint8_t { k24h = 0, k12h = 1 };

struct CivilDate {
    std::int32_t year = 1970;
    std::uint8_t month = 1; ///< 1..12
    std::uint8_t day = 1;   ///< 1..31
    constexpr auto operator<=>(const CivilDate&) const noexcept = default;
};

struct CivilTime {
    std::uint8_t hour = 0;   ///< 0..23
    std::uint8_t minute = 0; ///< 0..59
    std::uint8_t second = 0; ///< 0..59
    constexpr auto operator<=>(const CivilTime&) const noexcept = default;
};

/// A UTC instant expressed in some zone.
struct LocalDateTime {
    CivilDate date;
    CivilTime time;
    Weekday weekday = Weekday::kThursday;
    std::int32_t utc_offset_s = 0; ///< seconds east of UTC, DST included
    bool is_dst = false;
};

[[nodiscard]] DayNumber days_from_civil(const CivilDate& date) noexcept;
[[nodiscard]] CivilDate civil_from_days(DayNumber days) noexcept;
[[nodiscard]] Weekday weekday_from_days(DayNumber days) noexcept;
[[nodiscard]] bool is_leap_year(std::int32_t year) noexcept;
[[nodiscard]] std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) noexcept;
/// Valid month/day/hour/minute/second ranges (year 1970..2199).
[[nodiscard]] bool is_valid(const CivilDate& date, const CivilTime& time) noexcept;
/// Floor division helpers for negative times.
[[nodiscard]] DayNumber day_of(UnixSeconds t) noexcept;
[[nodiscard]] UnixSeconds floor_to_minute(UnixSeconds t) noexcept;

// ---- formatting (English names in v1) ----
[[nodiscard]] std::string_view weekday_name(Weekday wd, bool abbreviated) noexcept;
[[nodiscard]] std::string_view month_name(std::uint8_t month, bool abbreviated) noexcept;
/// "HH:MM" (24h) or "H:MM" (12h, *is_pm set when non-null). Returns chars written (no NUL).
std::size_t
format_hhmm(std::span<char> out, const CivilTime& t, HourFormat fmt, bool* is_pm) noexcept;
/// "YYYY-MM-DDTHH:MM:SSZ". Returns chars written.
std::size_t format_iso8601_utc(std::span<char> out, UnixSeconds t) noexcept;

} // namespace qz::time
