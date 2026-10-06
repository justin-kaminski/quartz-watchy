// Calendar math on 64-bit time: proleptic Gregorian, no leap seconds. Howard Hinnant's civil-date
// algorithms (public domain) evaluated on int64 intermediates, so every function is total: no
// input, including out-of-range fields, can overflow or trap. Results for fields outside the
// documented ranges are unspecified but defined (callers validate with is_valid()).
#include "qz/time/civil.hpp"

#include "time_math.hpp"

#include <array>
#include <cstdint>

namespace qz::time {
namespace {

constexpr std::int64_t kYearsPerEra = 400;
constexpr std::int64_t kDaysPerEra = 146'097;        ///< days in 400 Gregorian years
constexpr std::int64_t kEraEpochShiftDays = 719'468; ///< days from 0000-03-01 to 1970-01-01
constexpr std::int64_t kEpochWeekday = 4;            ///< 1970-01-01 was a Thursday
constexpr std::int64_t kDaysPerWeek = 7;
constexpr std::int32_t kFirstValidYear = 1970; ///< is_valid() window, ARCHITECTURE.md s8
constexpr std::int32_t kLastValidYear = 2199;
constexpr std::uint8_t kMonthsPerYear = 12;
constexpr std::uint8_t kFebruary = 2;
constexpr std::int64_t kFebruaryNumber = 2; ///< kFebruary for int64 month arithmetic
constexpr std::uint8_t kLeapFebruaryLength = 29;
constexpr std::uint8_t kHoursPerDay = 24;
constexpr std::uint8_t kMinutesPerHour = 60;
constexpr std::uint8_t kSecondsPerMinuteField = 60;
constexpr std::array<std::uint8_t, kMonthsPerYear> kMonthLengths{
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

} // namespace

DayNumber days_from_civil(const CivilDate& date) noexcept {
    const std::int64_t month = date.month;
    // The algorithm counts years from March so the leap day is the last day of the year.
    const std::int64_t year = std::int64_t{date.year} - ((month <= kFebruaryNumber) ? 1 : 0);
    const std::int64_t era = detail::floor_div(year, kYearsPerEra);
    const std::int64_t year_of_era = year - (era * kYearsPerEra); // [0, 399]
    const std::int64_t shifted_month =
        (month > kFebruaryNumber) ? (month - 3) : (month + 9);                         // March = 0
    const std::int64_t day_of_year = (((153 * shifted_month) + 2) / 5) + date.day - 1; // [0, 365]
    const std::int64_t day_of_era =
        (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
    return detail::saturate_to_i32((era * kDaysPerEra) + day_of_era - kEraEpochShiftDays);
}

CivilDate civil_from_days(DayNumber days) noexcept {
    const std::int64_t shifted_days = std::int64_t{days} + kEraEpochShiftDays;
    const std::int64_t era = detail::floor_div(shifted_days, kDaysPerEra);
    const std::int64_t day_of_era = shifted_days - (era * kDaysPerEra); // [0, 146096]
    const std::int64_t year_of_era =
        (day_of_era - (day_of_era / 1'460) + (day_of_era / 36'524) - (day_of_era / 146'096)) /
        365; // [0, 399]
    const std::int64_t day_of_year =
        day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100)); // [0, 365]
    const std::int64_t shifted_month = ((5 * day_of_year) + 2) / 153;                 // March = 0
    const std::int64_t day = day_of_year - (((153 * shifted_month) + 2) / 5) + 1;     // [1, 31]
    const std::int64_t month = (shifted_month < 10) ? (shifted_month + 3) : (shifted_month - 9);
    const std::int64_t year =
        year_of_era + (era * kYearsPerEra) + ((month <= kFebruaryNumber) ? 1 : 0);
    return CivilDate{static_cast<std::int32_t>(year),
                     static_cast<std::uint8_t>(month),
                     static_cast<std::uint8_t>(day)};
}

Weekday weekday_from_days(DayNumber days) noexcept {
    return static_cast<Weekday>(
        detail::floor_mod(std::int64_t{days} + kEpochWeekday, kDaysPerWeek));
}

bool is_leap_year(std::int32_t year) noexcept {
    return ((year % 4) == 0) && (((year % 100) != 0) || ((year % 400) == 0));
}

std::uint8_t days_in_month(std::int32_t year, std::uint8_t month) noexcept {
    if (month < 1 || month > kMonthsPerYear) {
        return 0;
    }
    if (month == kFebruary && is_leap_year(year)) {
        return kLeapFebruaryLength;
    }
    return kMonthLengths[month - 1U];
}

bool is_valid(const CivilDate& date, const CivilTime& time) noexcept {
    if (date.year < kFirstValidYear || date.year > kLastValidYear) {
        return false;
    }
    // days_in_month() is 0 for a month outside 1..12, which rejects every day value.
    if (date.day < 1 || date.day > days_in_month(date.year, date.month)) {
        return false;
    }
    return time.hour < kHoursPerDay && time.minute < kMinutesPerHour &&
           time.second < kSecondsPerMinuteField;
}

DayNumber day_of(UnixSeconds t) noexcept {
    return detail::saturate_to_i32(detail::floor_div(t, kSecondsPerDay));
}

UnixSeconds floor_to_minute(UnixSeconds t) noexcept {
    const std::int64_t into_minute = detail::floor_mod(t, kSecondsPerMinute);
    // Only the 59 lowest int64 values would underflow; they saturate.
    if (t < (std::numeric_limits<UnixSeconds>::min() + into_minute)) {
        return std::numeric_limits<UnixSeconds>::min();
    }
    return t - into_minute;
}

} // namespace qz::time
