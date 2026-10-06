// Calendar math and formatting (qz/time/civil.hpp). The calendar functions are cross-checked
// against glibc gmtime_r for every day 1900-2200 and the ISO formatter against strftime.
#include "qz/time/civil.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace qz::time {
namespace {

using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;

/// uint8 field as int (implicit promotion, so integer-sign-comparison lints stay quiet).
constexpr int as_int(std::uint8_t value) {
    return value;
}

// Most of a TEST body's cognitive complexity is the expansion of the gtest assertion macros.
// NOLINTBEGIN(readability-function-cognitive-complexity)

/// A Weekday with an out-of-range raw value (the enum is only constrained by its uint8 type).
Weekday weekday_from_raw(std::uint8_t raw) {
    return std::bit_cast<Weekday>(raw);
}

std::string show(const CivilDate& d) {
    return std::to_string(d.year) + "-" + std::to_string(d.month) + "-" + std::to_string(d.day);
}

// --- known days (values from an independent Python datetime computation) ----------------------

struct KnownDay {
    CivilDate date;
    DayNumber days;
    Weekday weekday;
};

constexpr auto kKnownDays = std::to_array<KnownDay>({
    {{1900, 1, 1}, -25'567, Weekday::kMonday},
    {{1900, 3, 1}, -25'508, Weekday::kThursday},
    {{1969, 12, 31}, -1, Weekday::kWednesday},
    {{1970, 1, 1}, 0, Weekday::kThursday},
    {{2000, 1, 1}, 10'957, Weekday::kSaturday},
    {{2000, 2, 29}, 11'016, Weekday::kTuesday},
    {{2000, 3, 1}, 11'017, Weekday::kWednesday},
    {{2024, 2, 29}, 19'782, Weekday::kThursday},
    {{2038, 1, 19}, 24'855, Weekday::kTuesday},
    {{2100, 1, 1}, 47'482, Weekday::kFriday},
    {{2100, 3, 1}, 47'541, Weekday::kMonday},
    {{2199, 12, 31}, 84'005, Weekday::kTuesday},
    {{2200, 1, 1}, 84'006, Weekday::kWednesday},
    {{2200, 12, 31}, 84'370, Weekday::kWednesday},
});

AssertionResult check_known_day(const KnownDay& known) {
    if (days_from_civil(known.date) != known.days) {
        return AssertionFailure() << show(known.date)
                                  << ": days_from_civil=" << days_from_civil(known.date)
                                  << ", expected " << known.days;
    }
    if (civil_from_days(known.days) != known.date) {
        return AssertionFailure() << "day " << known.days << " -> "
                                  << show(civil_from_days(known.days)) << ", expected "
                                  << show(known.date);
    }
    if (weekday_from_days(known.days) != known.weekday) {
        return AssertionFailure() << show(known.date) << ": weekday "
                                  << static_cast<int>(weekday_from_days(known.days))
                                  << ", expected " << static_cast<int>(known.weekday);
    }
    return AssertionSuccess();
}

TEST(CivilCalendar, KnownDayNumbersAndWeekdays) {
    for (const KnownDay& known : kKnownDays) {
        EXPECT_TRUE(check_known_day(known));
    }
}

// --- every day 1900-2200 ------------------------------------------------------------------------

/// The calendar's own successor function, independent of days_from_civil / civil_from_days.
CivilDate next_day(const CivilDate& d) {
    CivilDate next = d;
    if (d.day < days_in_month(d.year, d.month)) {
        next.day = static_cast<std::uint8_t>(d.day + 1);
        return next;
    }
    next.day = 1;
    if (d.month < 12) {
        next.month = static_cast<std::uint8_t>(d.month + 1);
        return next;
    }
    next.month = 1;
    next.year = d.year + 1;
    return next;
}

AssertionResult check_day(DayNumber day, const CivilDate& expected) {
    const CivilDate date = civil_from_days(day);
    if (date != expected) {
        return AssertionFailure() << "day " << day << " -> " << show(date) << ", expected "
                                  << show(expected);
    }
    if (days_from_civil(date) != day) {
        return AssertionFailure() << show(date) << " -> " << days_from_civil(date) << ", expected "
                                  << day;
    }
    const auto stamp = static_cast<std::time_t>(std::int64_t{day} * kSecondsPerDay);
    std::tm ref{};
    if (gmtime_r(&stamp, &ref) == nullptr) {
        return AssertionFailure() << "gmtime_r failed for day " << day;
    }
    const bool same = ref.tm_year + 1900 == date.year && ref.tm_mon + 1 == as_int(date.month) &&
                      ref.tm_mday == as_int(date.day) &&
                      ref.tm_wday == static_cast<int>(weekday_from_days(day));
    if (!same) {
        return AssertionFailure() << show(date) << " weekday "
                                  << static_cast<int>(weekday_from_days(day)) << " but glibc says "
                                  << (ref.tm_year + 1900) << "-" << (ref.tm_mon + 1) << "-"
                                  << ref.tm_mday << " weekday " << ref.tm_wday;
    }
    return AssertionSuccess();
}

TEST(CivilCalendar, EveryDayFrom1900To2200RoundTripsAndMatchesGlibc) {
    const DayNumber first = days_from_civil(CivilDate{1900, 1, 1});
    const DayNumber last = days_from_civil(CivilDate{2200, 12, 31});
    ASSERT_EQ(first, -25'567);
    ASSERT_EQ(last, 84'370);
    CivilDate expected{1900, 1, 1};
    for (DayNumber day = first; day <= last; ++day) {
        const AssertionResult result = check_day(day, expected);
        if (!result) {
            ADD_FAILURE() << result.message();
            return;
        }
        expected = next_day(expected);
    }
    EXPECT_EQ(expected, (CivilDate{2201, 1, 1}));
}

TEST(CivilCalendar, ExtremeInputsAreTotal) {
    constexpr DayNumber kMin = std::numeric_limits<DayNumber>::min();
    constexpr DayNumber kMax = std::numeric_limits<DayNumber>::max();
    for (const DayNumber day : {kMin, kMin + 1, -1, 0, 1, kMax - 1, kMax}) {
        EXPECT_EQ(days_from_civil(civil_from_days(day)), day) << day;
        EXPECT_LE(static_cast<int>(weekday_from_days(day)), 6) << day;
    }
    // Year fields beyond the day-number range saturate rather than overflow.
    constexpr std::int32_t kMaxYear = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t kMinYear = std::numeric_limits<std::int32_t>::min();
    EXPECT_EQ(days_from_civil(CivilDate{kMaxYear, 12, 31}), kMax);
    EXPECT_EQ(days_from_civil(CivilDate{kMinYear, 1, 1}), kMin);
    // Fields outside their ranges have unspecified but defined results (no trap under UBSan).
    std::int64_t sink = 0;
    for (const std::uint8_t month : {std::uint8_t{0}, std::uint8_t{13}, std::uint8_t{255}}) {
        for (const std::uint8_t day : {std::uint8_t{0}, std::uint8_t{32}, std::uint8_t{255}}) {
            sink += days_from_civil(CivilDate{2024, month, day});
        }
    }
    EXPECT_NE(sink, 0);
}

TEST(CivilCalendar, LeapYearRule) {
    for (const std::int32_t year : {1600, 2000, 2024, 2096, 2400, 0, -4, -400}) {
        EXPECT_TRUE(is_leap_year(year)) << year;
    }
    for (const std::int32_t year : {1900, 2100, 2200, 2023, 2199, 1, -1, -100}) {
        EXPECT_FALSE(is_leap_year(year)) << year;
    }
}

TEST(CivilCalendar, DaysInMonth) {
    constexpr std::array<std::uint8_t, 12> kCommon{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (std::uint8_t month = 1; month <= 12; ++month) {
        const std::uint8_t common = kCommon[month - 1U];
        EXPECT_EQ(days_in_month(2023, month), common) << static_cast<int>(month);
        EXPECT_EQ(days_in_month(2024, month), month == 2 ? 29 : common) << static_cast<int>(month);
    }
    EXPECT_EQ(days_in_month(1900, 2), 28);
    EXPECT_EQ(days_in_month(2000, 2), 29);
    EXPECT_EQ(days_in_month(2100, 2), 28);
    EXPECT_EQ(days_in_month(2024, 0), 0);
    EXPECT_EQ(days_in_month(2024, 13), 0);
    EXPECT_EQ(days_in_month(2024, 255), 0);
}

struct ValidCase {
    CivilDate date;
    CivilTime time;
    bool valid;
};

constexpr auto kValidCases = std::to_array<ValidCase>({
    {{1970, 1, 1}, {0, 0, 0}, true},
    {{2199, 12, 31}, {23, 59, 59}, true},
    {{2024, 2, 29}, {12, 0, 0}, true},
    {{2000, 2, 29}, {0, 0, 0}, true},
    {{1969, 12, 31}, {23, 59, 59}, false}, // before the supported window
    {{2200, 1, 1}, {0, 0, 0}, false},      // after the supported window
    {{2023, 2, 29}, {0, 0, 0}, false},
    {{2100, 2, 29}, {0, 0, 0}, false},
    {{2024, 4, 31}, {0, 0, 0}, false},
    {{2024, 0, 1}, {0, 0, 0}, false},
    {{2024, 13, 1}, {0, 0, 0}, false},
    {{2024, 1, 0}, {0, 0, 0}, false},
    {{2024, 1, 32}, {0, 0, 0}, false},
    {{2024, 6, 15}, {24, 0, 0}, false},
    {{2024, 6, 15}, {0, 60, 0}, false},
    {{2024, 6, 15}, {0, 0, 60}, false},
    {{2024, 6, 15}, {23, 59, 59}, true},
});

TEST(CivilCalendar, IsValidChecksEveryField) {
    for (const ValidCase& c : kValidCases) {
        EXPECT_EQ(is_valid(c.date, c.time), c.valid)
            << show(c.date) << " " << static_cast<int>(c.time.hour) << ":"
            << static_cast<int>(c.time.minute) << ":" << static_cast<int>(c.time.second);
    }
}

TEST(CivilCalendar, DayOfAndFloorToMinuteFloorTowardNegativeInfinity) {
    EXPECT_EQ(day_of(0), 0);
    EXPECT_EQ(day_of(86'399), 0);
    EXPECT_EQ(day_of(86'400), 1);
    EXPECT_EQ(day_of(-1), -1);
    EXPECT_EQ(day_of(-86'400), -1);
    EXPECT_EQ(day_of(-86'401), -2);
    EXPECT_EQ(day_of(std::numeric_limits<std::int64_t>::max()),
              std::numeric_limits<DayNumber>::max());
    EXPECT_EQ(day_of(std::numeric_limits<std::int64_t>::min()),
              std::numeric_limits<DayNumber>::min());

    EXPECT_EQ(floor_to_minute(0), 0);
    EXPECT_EQ(floor_to_minute(59), 0);
    EXPECT_EQ(floor_to_minute(60), 60);
    EXPECT_EQ(floor_to_minute(119), 60);
    EXPECT_EQ(floor_to_minute(-1), -60);
    EXPECT_EQ(floor_to_minute(-60), -60);
    EXPECT_EQ(floor_to_minute(-61), -120);
    EXPECT_EQ(floor_to_minute(1'711'846'859), 1'711'846'800);
    // The extremes saturate instead of wrapping.
    EXPECT_EQ(floor_to_minute(std::numeric_limits<std::int64_t>::min()),
              std::numeric_limits<std::int64_t>::min());
    const std::int64_t top = floor_to_minute(std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(top % 60, 0);
    EXPECT_GT(top, std::numeric_limits<std::int64_t>::max() - 60);
}

// --- names ------------------------------------------------------------------------------------

TEST(CivilFormat, WeekdayAndMonthNames) {
    constexpr auto kWeekdays = std::to_array<std::string_view>(
        {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"});
    constexpr auto kMonths = std::to_array<std::string_view>({"January",
                                                              "February",
                                                              "March",
                                                              "April",
                                                              "May",
                                                              "June",
                                                              "July",
                                                              "August",
                                                              "September",
                                                              "October",
                                                              "November",
                                                              "December"});
    for (std::size_t i = 0; i < kWeekdays.size(); ++i) {
        const auto wd = static_cast<Weekday>(i);
        EXPECT_EQ(weekday_name(wd, false), kWeekdays[i]);
        EXPECT_EQ(weekday_name(wd, true), kWeekdays[i].substr(0, 3));
    }
    for (std::size_t i = 0; i < kMonths.size(); ++i) {
        const auto month = static_cast<std::uint8_t>(i + 1);
        EXPECT_EQ(month_name(month, false), kMonths[i]);
        EXPECT_EQ(month_name(month, true), kMonths[i].substr(0, 3));
    }
    EXPECT_TRUE(weekday_name(weekday_from_raw(7), false).empty());
    EXPECT_TRUE(weekday_name(weekday_from_raw(255), true).empty());
    EXPECT_TRUE(month_name(0, false).empty());
    EXPECT_TRUE(month_name(13, true).empty());
}

// --- HH:MM ------------------------------------------------------------------------------------

struct HhmmCase {
    std::uint8_t hour;
    std::uint8_t minute;
    HourFormat format;
    std::string_view text;
    bool pm;
};

constexpr auto kHhmmCases = std::to_array<HhmmCase>({
    {0, 0, HourFormat::k24h, "00:00", false},
    {9, 5, HourFormat::k24h, "09:05", false},
    {12, 0, HourFormat::k24h, "12:00", true},
    {23, 59, HourFormat::k24h, "23:59", true},
    {0, 0, HourFormat::k12h, "12:00", false},
    {0, 59, HourFormat::k12h, "12:59", false},
    {1, 0, HourFormat::k12h, "1:00", false},
    {9, 5, HourFormat::k12h, "9:05", false},
    {10, 30, HourFormat::k12h, "10:30", false},
    {11, 59, HourFormat::k12h, "11:59", false},
    {12, 0, HourFormat::k12h, "12:00", true},
    {12, 30, HourFormat::k12h, "12:30", true},
    {13, 0, HourFormat::k12h, "1:00", true},
    {21, 7, HourFormat::k12h, "9:07", true},
    {22, 45, HourFormat::k12h, "10:45", true},
    {23, 59, HourFormat::k12h, "11:59", true},
});

AssertionResult check_hhmm(const HhmmCase& c) {
    std::array<char, 8> buffer{};
    buffer.fill('#');
    bool pm = !c.pm; // start with the wrong answer so a missing write is visible
    const CivilTime t{c.hour, c.minute, 0};
    const std::size_t length = format_hhmm(buffer, t, c.format, &pm);
    if (length != c.text.size() || std::string_view(buffer.data(), length) != c.text) {
        return AssertionFailure() << "got '" << std::string_view(buffer.data(), length) << "' ("
                                  << length << " chars), expected '" << c.text << "'";
    }
    if (buffer[length] != '#') {
        return AssertionFailure() << c.text << ": wrote past the text (no NUL, no padding)";
    }
    if (pm != c.pm) {
        return AssertionFailure() << c.text << ": is_pm=" << pm << ", expected " << c.pm;
    }
    std::array<char, 8> again{};
    if (format_hhmm(again, t, c.format, nullptr) != length) {
        return AssertionFailure() << c.text << ": null is_pm changes the text";
    }
    return AssertionSuccess();
}

TEST(CivilFormat, HhmmIn24hAnd12h) {
    for (const HhmmCase& c : kHhmmCases) {
        EXPECT_TRUE(check_hhmm(c)) << c.text;
    }
}

TEST(CivilFormat, HhmmHonoursTheBufferSize) {
    bool pm = true;
    std::array<char, 5> exact24{};
    EXPECT_EQ(format_hhmm(exact24, CivilTime{9, 5, 0}, HourFormat::k24h, nullptr), 5U);
    EXPECT_EQ(std::string_view(exact24.data(), 5), "09:05");

    std::array<char, 4> small24{};
    small24.fill('#');
    EXPECT_EQ(format_hhmm(small24, CivilTime{9, 5, 0}, HourFormat::k24h, &pm), 0U);
    EXPECT_EQ(std::string_view(small24.data(), 4), "####"); // nothing written
    EXPECT_TRUE(pm);                                        // untouched on failure

    std::array<char, 4> exact12{};
    EXPECT_EQ(format_hhmm(exact12, CivilTime{9, 5, 0}, HourFormat::k12h, &pm), 4U);
    EXPECT_EQ(std::string_view(exact12.data(), 4), "9:05");
    EXPECT_FALSE(pm);

    pm = true;
    std::array<char, 4> small12{};
    small12.fill('#');
    EXPECT_EQ(format_hhmm(small12, CivilTime{10, 5, 0}, HourFormat::k12h, &pm), 0U); // needs 5
    EXPECT_EQ(std::string_view(small12.data(), 4), "####");
    EXPECT_TRUE(pm);

    std::array<char, 3> tiny{};
    EXPECT_EQ(format_hhmm(tiny, CivilTime{9, 5, 0}, HourFormat::k12h, nullptr), 0U);
    EXPECT_EQ(format_hhmm(std::span<char>{}, CivilTime{9, 5, 0}, HourFormat::k24h, nullptr), 0U);
}

TEST(CivilFormat, HhmmRefusesValuesWithoutATextForm) {
    std::array<char, 8> buffer{};
    buffer.fill('#');
    bool pm = true;
    for (const CivilTime& bad :
         {CivilTime{24, 0, 0}, CivilTime{0, 60, 0}, CivilTime{255, 255, 0}}) {
        EXPECT_EQ(format_hhmm(buffer, bad, HourFormat::k24h, &pm), 0U);
        EXPECT_EQ(format_hhmm(buffer, bad, HourFormat::k12h, &pm), 0U);
    }
    EXPECT_EQ(std::string_view(buffer.data(), 8), "########");
    EXPECT_TRUE(pm);
}

// --- ISO 8601 ---------------------------------------------------------------------------------

struct IsoCase {
    std::int64_t t;
    std::string_view text;
};

constexpr auto kIsoCases = std::to_array<IsoCase>({
    {0, "1970-01-01T00:00:00Z"},
    {-1, "1969-12-31T23:59:59Z"},
    {86'400, "1970-01-02T00:00:00Z"},
    {951'827'696, "2000-02-29T12:34:56Z"},
    {1'711'846'800, "2024-03-31T01:00:00Z"},
    {4'107'542'400, "2100-03-01T00:00:00Z"},
    {-2'208'988'800, "1900-01-01T00:00:00Z"},
    {253'402'300'799, "9999-12-31T23:59:59Z"},
    {-62'167'219'200, "0000-01-01T00:00:00Z"},
});

TEST(CivilFormat, Iso8601KnownInstants) {
    for (const IsoCase& c : kIsoCases) {
        std::array<char, 24> buffer{};
        buffer.fill('#');
        const std::size_t length = format_iso8601_utc(buffer, c.t);
        EXPECT_EQ(length, 20U) << c.text;
        EXPECT_EQ(std::string_view(buffer.data(), length), c.text);
        EXPECT_EQ(buffer[20], '#') << "wrote past the 20 characters";
    }
}

TEST(CivilFormat, Iso8601HonoursTheBufferAndTheYearRange) {
    std::array<char, 20> exact{};
    EXPECT_EQ(format_iso8601_utc(exact, 0), 20U);
    EXPECT_EQ(std::string_view(exact.data(), 20), "1970-01-01T00:00:00Z");

    std::array<char, 19> small{};
    small.fill('#');
    EXPECT_EQ(format_iso8601_utc(small, 0), 0U);
    EXPECT_EQ(std::string_view(small.data(), 19), std::string(19, '#'));
    EXPECT_EQ(format_iso8601_utc(std::span<char>{}, 0), 0U);

    std::array<char, 24> big{};
    EXPECT_EQ(format_iso8601_utc(big, 253'402'300'800), 0U); // year 10000
    EXPECT_EQ(format_iso8601_utc(big, -62'167'219'201), 0U); // year -1
    EXPECT_EQ(format_iso8601_utc(big, std::numeric_limits<std::int64_t>::max()), 0U);
    EXPECT_EQ(format_iso8601_utc(big, std::numeric_limits<std::int64_t>::min()), 0U);
}

TEST(CivilFormat, Iso8601MatchesStrftimeAcrossTheCalendar) {
    const std::int64_t begin =
        std::int64_t{days_from_civil(CivilDate{1900, 1, 1})} * kSecondsPerDay;
    const std::int64_t end = std::int64_t{days_from_civil(CivilDate{2201, 1, 1})} * kSecondsPerDay;
    std::size_t checked = 0;
    for (std::int64_t t = begin; t < end;
         t += 100'003) { // ~27.8 h: walks through every hour of day
        const auto stamp = static_cast<std::time_t>(t);
        std::tm ref{};
        std::array<char, 32> expected{};
        ASSERT_NE(gmtime_r(&stamp, &ref), nullptr);
        ASSERT_EQ(strftime(expected.data(), expected.size(), "%Y-%m-%dT%H:%M:%SZ", &ref), 20U);
        std::array<char, 20> actual{};
        ASSERT_EQ(format_iso8601_utc(actual, t), 20U) << t;
        ASSERT_EQ(std::string_view(actual.data(), 20), std::string_view(expected.data(), 20)) << t;
        ++checked;
    }
    EXPECT_GT(checked, 90'000U);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::time
