// POSIX TZ engine (qz/time/tz.hpp): grammar, DST evaluation and local<->UTC resolution, pinned to
// facts that do not depend on glibc (tz_oracle_test.cpp compares the same engine against glibc).
#include "qz/time/tz.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

std::atomic<std::size_t>& allocation_counter() {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

} // namespace

// Called by the ASan runtime after every allocation; a strong definition here overrides the
// runtime's weak empty one (the same device qz_testkit's fake_allocation_test uses).
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" void __sanitizer_malloc_hook(const volatile void* /*ptr*/, std::size_t /*size*/) {
    allocation_counter().fetch_add(1, std::memory_order_relaxed);
}

namespace qz::time {
namespace {

using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;

// Most of a TEST body's cognitive complexity is the expansion of the gtest assertion macros.
// NOLINTBEGIN(readability-function-cognitive-complexity)

constexpr std::int64_t kNoTransition = std::numeric_limits<std::int64_t>::max();

/// UTC instant of a civil date and time (independent of the zone engine under test).
std::int64_t utc(std::int32_t year,
                 std::uint8_t month,
                 std::uint8_t day,
                 std::int64_t hour = 0,
                 std::int64_t minute = 0,
                 std::int64_t second = 0) {
    return (std::int64_t{days_from_civil(CivilDate{year, month, day})} * kSecondsPerDay) +
           (hour * 3'600) + (minute * 60) + second;
}

TimeZone parse_ok(std::string_view posix) {
    const Result<TimeZone> parsed = TimeZone::parse(posix);
    if (!parsed) {
        ADD_FAILURE() << "parse rejected '" << posix << "'";
        return TimeZone{};
    }
    return *parsed;
}

std::int64_t next_after(const TimeZone& zone, std::int64_t t) {
    return zone.next_transition(t).value_or(kNoTransition);
}

/// An expected wall-clock reading.
struct Wall {
    std::int32_t year;
    std::uint8_t month;
    std::uint8_t day;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t second;
    std::int32_t offset_s;
    bool dst;
};

AssertionResult reads(const TimeZone& zone, std::int64_t t, const Wall& want) {
    const LocalDateTime got = zone.to_local(t);
    const bool same = got.date.year == want.year && got.date.month == want.month &&
                      got.date.day == want.day && got.time.hour == want.hour &&
                      got.time.minute == want.minute && got.time.second == want.second &&
                      got.utc_offset_s == want.offset_s && got.is_dst == want.dst;
    if (same && zone.utc_offset_at(t) == want.offset_s && zone.is_dst_at(t) == want.dst) {
        return AssertionSuccess();
    }
    return AssertionFailure() << "t=" << t << " reads " << got.date.year << "-"
                              << static_cast<int>(got.date.month) << "-"
                              << static_cast<int>(got.date.day) << " "
                              << static_cast<int>(got.time.hour) << ":"
                              << static_cast<int>(got.time.minute) << ":"
                              << static_cast<int>(got.time.second) << " offset " << got.utc_offset_s
                              << " dst " << got.is_dst << ", expected " << want.year << "-"
                              << static_cast<int>(want.month) << "-" << static_cast<int>(want.day)
                              << " " << static_cast<int>(want.hour) << ":"
                              << static_cast<int>(want.minute) << ":"
                              << static_cast<int>(want.second) << " offset " << want.offset_s
                              << " dst " << want.dst;
}

// --- grammar: accepted forms -----------------------------------------------------------------

struct AcceptCase {
    std::string_view posix;
    bool has_dst;
    std::int32_t january_offset_s; ///< at 2024-01-15T12:00Z
    std::int32_t july_offset_s;    ///< at 2024-07-15T12:00Z
    std::string_view std_abbr;
    std::string_view dst_abbr; ///< equals std_abbr for fixed-offset zones
};

const auto kAccepted = std::to_array<AcceptCase>({
    // fixed offsets: plain, signed, quoted, minutes, seconds, extremes
    {"UTC0", false, 0, 0, "UTC", "UTC"},
    {"GMT0", false, 0, 0, "GMT", "GMT"},
    {"EST5", false, -18'000, -18'000, "EST", "EST"},
    {"EST+5", false, -18'000, -18'000, "EST", "EST"},
    {"CET-1", false, 3'600, 3'600, "CET", "CET"},
    {"<+03>-3", false, 10'800, 10'800, "+03", "+03"},
    {"<-03>3", false, -10'800, -10'800, "-03", "-03"},
    {"<+0545>-5:45", false, 20'700, 20'700, "+0545", "+0545"},
    {"<-0930>9:30", false, -34'200, -34'200, "-0930", "-0930"},
    {"<+005328>-0:53:28", false, 3'208, 3'208, "+005328", "+005328"},
    {"<UTC+1>-1", false, 3'600, 3'600, "UTC+1", "UTC+1"},
    {"<AB1>5", false, -18'000, -18'000, "AB1", "AB1"},
    {"ABC-14", false, 50'400, 50'400, "ABC", "ABC"},
    {"ABC12", false, -43'200, -43'200, "ABC", "ABC"},
    {"ABC-24", false, 86'400, 86'400, "ABC", "ABC"},
    {"ABCDEFGHIJ0", false, 0, 0, "ABCDEFGHIJ", "ABCDEFGHIJ"}, // 10 letters: the storage limit
    // DST with Mm.w.d rules, default and explicit DST offsets
    {"est5edt,M3.2.0,M11.1.0", true, -18'000, -14'400, "est", "edt"},
    {"EST5EDT,M3.2.0,M11.1.0", true, -18'000, -14'400, "EST", "EDT"},
    {"EST5EDT4,M3.2.0,M11.1.0", true, -18'000, -14'400, "EST", "EDT"},
    {"CET-1CEST,M3.5.0,M10.5.0/3", true, 3'600, 7'200, "CET", "CEST"},
    {"XXX3YYY1,M3.2.0,M11.1.0", true, -10'800, -3'600, "XXX", "YYY"},
    {"XXX-1YYY-3,M3.5.0,M10.5.0", true, 3'600, 10'800, "XXX", "YYY"}, // two-hour DST
    {"AAA5:30BBB4:30,M3.2.0,M11.1.0", true, -19'800, -16'200, "AAA", "BBB"},
    {"XXX0YYY0,M3.5.0,M10.5.0", true, 0, 0, "XXX", "YYY"}, // DST that only renames
    // southern hemisphere, half-hour DST, negative DST (Dublin)
    {"AEST-10AEDT,M10.1.0,M4.1.0/3", true, 39'600, 36'000, "AEST", "AEDT"},
    {"<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", true, 39'600, 37'800, "+1030", "+11"},
    {"IST-1GMT0,M10.5.0,M3.5.0/1", true, 0, 3'600, "IST", "GMT"},
    {"<+00>0<+02>-2,M3.5.0/1,M10.5.0/3", true, 0, 7'200, "+00", "+02"},
    // rule times: > 24 h, negative, with minutes and seconds, explicit signs, +-167 h
    {"EET-2EEST,M3.4.4/50,M10.4.4/50", true, 7'200, 10'800, "EET", "EEST"},
    {"<-02>2<-01>,M3.5.0/-1,M10.5.0/0", true, -7'200, -3'600, "-02", "-01"},
    {"XXX5YYY,M3.2.0/+3,M11.1.0/-3", true, -18'000, -14'400, "XXX", "YYY"},
    {"XXX5YYY,M3.2.0/2:30:15,M11.1.0/1:45", true, -18'000, -14'400, "XXX", "YYY"},
    {"XXX5YYY,M3.2.0/167:59:59,M11.1.0/-167:59:59", true, -18'000, -14'400, "XXX", "YYY"},
    // Jn, n and permanent DST (0/0,J365/25)
    {"XXX5YYY,J60/2,J300/3", true, -18'000, -14'400, "XXX", "YYY"},
    {"XXX5YYY,59,300", true, -18'000, -14'400, "XXX", "YYY"},
    {"XXX5YYY,0/0,J365/25", true, -14'400, -14'400, "XXX", "YYY"},
    {"XXX5YYY,J1/0,J365/25", true, -14'400, -14'400, "XXX", "YYY"},
});

AssertionResult check_accepted(const AcceptCase& c) {
    const Result<TimeZone> parsed = TimeZone::parse(c.posix);
    if (!parsed) {
        return AssertionFailure() << "rejected '" << c.posix << "'";
    }
    const TimeZone& zone = *parsed;
    const std::int64_t january = utc(2024, 1, 15, 12);
    const std::int64_t july = utc(2024, 7, 15, 12);
    if (zone.has_dst() != c.has_dst) {
        return AssertionFailure() << c.posix << ": has_dst=" << zone.has_dst();
    }
    if (zone.abbreviation(false) != c.std_abbr || zone.abbreviation(true) != c.dst_abbr) {
        return AssertionFailure() << c.posix << ": abbreviations '" << zone.abbreviation(false)
                                  << "'/'" << zone.abbreviation(true) << "'";
    }
    if (zone.utc_offset_at(january) != c.january_offset_s ||
        zone.utc_offset_at(july) != c.july_offset_s) {
        return AssertionFailure() << c.posix << ": offsets " << zone.utc_offset_at(january) << "/"
                                  << zone.utc_offset_at(july) << ", expected " << c.january_offset_s
                                  << "/" << c.july_offset_s;
    }
    const LocalDateTime local = zone.to_local(july);
    if (local.utc_offset_s != c.july_offset_s || local.is_dst != zone.is_dst_at(july)) {
        return AssertionFailure() << c.posix << ": to_local disagrees with the accessors";
    }
    return AssertionSuccess();
}

TEST(TzParse, AcceptsEveryRfc9636Form) {
    EXPECT_GE(kAccepted.size(), 30U);
    for (const AcceptCase& c : kAccepted) {
        EXPECT_TRUE(check_accepted(c));
    }
}

// --- grammar: rejected strings ----------------------------------------------------------------

using namespace std::string_view_literals;

constexpr auto kRejected = std::to_array<std::string_view>({
    // structure
    ""sv,
    "UTC"sv,
    "EST"sv,
    "5"sv,
    "EST5EDT"sv,                        // DST name without the mandatory rule pair
    "EST5EDT,"sv,                       // comma, no rule
    "EST5EDT,M3.2.0"sv,                 // one rule only
    "EST5EDT,M3.2.0,M11.1.0,"sv,        // trailing comma
    "EST5EDT,M3.2.0,M11.1.0,M12.1.0"sv, // third rule
    "EST5EDT,M3.2.0,,M11.1.0"sv,        // empty rule
    "EST5EDT,,"sv,                      // empty rules
    "EST5 EDT,M3.2.0,M11.1.0"sv,        // space
    " EST5"sv,
    "EST5 "sv,
    "EST5EDT;M3.2.0;M11.1.0"sv, // wrong separator
    "EST5:EDT"sv,
    "EST5EDT,M3.2.0,M11.1.0x"sv, // trailing junk
    "EST5EDT,M3.2.0,M11.1.0 "sv,
    "EST5EDT,M3.2.0,M11.1.0\n"sv,
    "EST5EDT,M3.2.0,M11.1.0\0"sv, // trailing NUL character
    "EST5\0EDT"sv,                // embedded NUL character
    // designations
    "ES5"sv, // two letters
    "<>5"sv,
    "<ES>5"sv,
    "<EST5"sv, // unterminated quote
    "EST>5"sv,
    "<EST>5>"sv,
    "<E ST>5"sv,        // space inside the quotes
    "<EST!>5"sv,        // punctuation inside the quotes
    "ESTABCDEFGHIJ5"sv, // longer than the storage
    "<ESTABCDEFGHIJ>5"sv,
    "ABCDEFGHIJK0"sv, // eleven letters
    "1EST5"sv,
    "E1T5"sv,
    "<EST>"sv, // no offset
    "EST5<EDT"sv,
    // offsets
    "EST25"sv, // hour > 24
    "EST5:60"sv,
    "EST5:30:60"sv,
    "EST5:5"sv, // one-digit minute
    "EST5:"sv,
    "EST5:30:"sv,
    "EST5::30"sv,
    "EST+"sv,
    "EST-"sv,
    "EST5:30:45:10"sv,
    "EST005"sv, // three-digit hour
    "EST5.5"sv,
    "EST--5"sv,
    "EST+-5"sv,
    "EST5+"sv,
    "EST5EDT25,M3.2.0,M11.1.0"sv,
    "EST5EDT+,M3.2.0,M11.1.0"sv,
    // rules
    "EST5EDT,M0.1.0,M11.1.0"sv, // month 0
    "EST5EDT,M13.1.0,M11.1.0"sv,
    "EST5EDT,M3.0.0,M11.1.0"sv, // week 0
    "EST5EDT,M3.6.0,M11.1.0"sv,
    "EST5EDT,M3.2.7,M11.1.0"sv, // weekday 7
    "EST5EDT,M3.2,M11.1.0"sv,
    "EST5EDT,M3,M11"sv,
    "EST5EDT,M3.2.0.1,M11.1.0"sv,
    "EST5EDT,J0,J365"sv, // Jn counts from 1
    "EST5EDT,J366,J10"sv,
    "EST5EDT,J1000,J10"sv,
    "EST5EDT,366,300"sv,
    "EST5EDT,J,J10"sv,
    "EST5EDT,X3,M11.1.0"sv,
    "EST5EDT,m3.2.0,M11.1.0"sv, // lowercase rule letter
    // rule times
    "EST5EDT,M3.2.0/,M11.1.0"sv,
    "EST5EDT,M3.2.0/168,M11.1.0"sv, // hour > 167
    "EST5EDT,M3.2.0/-168,M11.1.0"sv,
    "EST5EDT,M3.2.0/2:60,M11.1.0"sv,
    "EST5EDT,M3.2.0/2:5,M11.1.0"sv,
    "EST5EDT,M3.2.0/+,M11.1.0"sv,
    "EST5EDT,M3.2.0/1000,M11.1.0"sv,
    "EST5EDT,M3.2.0/2,M11.1.0/2/3"sv,
});

AssertionResult check_rejected(std::string_view posix) {
    const Result<TimeZone> parsed = TimeZone::parse(posix);
    if (parsed) {
        return AssertionFailure() << "accepted '" << posix << "'";
    }
    if (parsed.error() != Error{Errc::kBadArgs}) {
        return AssertionFailure() << "'" << posix << "' failed with the wrong error";
    }
    return AssertionSuccess();
}

TEST(TzParse, RejectsMalformedStrings) {
    EXPECT_GE(kRejected.size(), 40U);
    for (const std::string_view posix : kRejected) {
        EXPECT_TRUE(check_rejected(posix));
    }
}

TEST(TzParse, DefaultConstructedZoneIsUtc) {
    const TimeZone zone;
    EXPECT_FALSE(zone.has_dst());
    EXPECT_EQ(zone.abbreviation(false), "UTC");
    EXPECT_EQ(zone.abbreviation(true), "UTC");
    EXPECT_EQ(zone.utc_offset_at(0), 0);
    EXPECT_FALSE(zone.is_dst_at(0));
    EXPECT_FALSE(zone.next_transition(0).has_value());
    EXPECT_TRUE(reads(zone, 0, {1970, 1, 1, 0, 0, 0, 0, false}));
    EXPECT_EQ(zone.to_local(0).weekday, Weekday::kThursday);
}

TEST(TzParse, ZoneIsASmallTriviallyCopyableValue) {
    static_assert(std::is_trivially_copyable_v<TimeZone>);
    static_assert(noexcept(TimeZone::parse("UTC0")));
    static_assert(noexcept(std::declval<const TimeZone&>().to_local(0)));
    static_assert(noexcept(std::declval<const TimeZone&>().next_transition(0)));
    static_assert(noexcept(
        std::declval<const TimeZone&>().to_utc(CivilDate{}, CivilTime{}, GapPolicy::kReject)));
    EXPECT_LE(sizeof(TimeZone), 128U);
}

// --- evaluation: real zones, instants pinned by hand --------------------------------------------

TEST(TzEngine, BerlinSpringForwardAndFallBack) {
    const TimeZone zone = parse_ok("CET-1CEST,M3.5.0,M10.5.0/3");
    const std::int64_t spring = utc(2024, 3, 31, 1);
    const std::int64_t autumn = utc(2024, 10, 27, 1);
    EXPECT_EQ(spring, 1'711'846'800);
    EXPECT_EQ(autumn, 1'729'990'800);
    EXPECT_TRUE(reads(zone, spring - 1, {2024, 3, 31, 1, 59, 59, 3'600, false}));
    EXPECT_TRUE(reads(zone, spring, {2024, 3, 31, 3, 0, 0, 7'200, true}));
    EXPECT_TRUE(reads(zone, autumn - 1, {2024, 10, 27, 2, 59, 59, 7'200, true}));
    EXPECT_TRUE(reads(zone, autumn, {2024, 10, 27, 2, 0, 0, 3'600, false}));
    EXPECT_EQ(zone.to_local(spring).weekday, Weekday::kSunday);
    EXPECT_EQ(zone.abbreviation(zone.is_dst_at(spring)), "CEST");
    EXPECT_EQ(zone.abbreviation(zone.is_dst_at(autumn)), "CET");
    EXPECT_TRUE(zone.has_dst());
}

TEST(TzEngine, NewYorkSpringForwardAndFallBack) {
    const TimeZone zone = parse_ok("EST5EDT,M3.2.0,M11.1.0");
    const std::int64_t spring = utc(2024, 3, 10, 7);
    const std::int64_t fall = utc(2024, 11, 3, 6);
    EXPECT_EQ(spring, 1'710'054'000);
    EXPECT_EQ(fall, 1'730'613'600);
    EXPECT_TRUE(reads(zone, spring - 1, {2024, 3, 10, 1, 59, 59, -18'000, false}));
    EXPECT_TRUE(reads(zone, spring, {2024, 3, 10, 3, 0, 0, -14'400, true}));
    EXPECT_TRUE(reads(zone, fall - 1, {2024, 11, 3, 1, 59, 59, -14'400, true}));
    EXPECT_TRUE(reads(zone, fall, {2024, 11, 3, 1, 0, 0, -18'000, false}));
}

TEST(TzEngine, SydneyDstSpansNewYear) {
    const TimeZone zone = parse_ok("AEST-10AEDT,M10.1.0,M4.1.0/3");
    const std::int64_t start = utc(2024, 10, 5, 16);
    const std::int64_t end = utc(2025, 4, 5, 16);
    EXPECT_EQ(start, 1'728'144'000);
    EXPECT_EQ(end, 1'743'868'800);
    EXPECT_TRUE(zone.is_dst_at(utc(2024, 1, 15, 12))); // January is summer
    EXPECT_FALSE(zone.is_dst_at(utc(2024, 7, 15, 12)));
    EXPECT_TRUE(zone.is_dst_at(utc(2024, 12, 31, 23, 59, 59)));
    EXPECT_TRUE(zone.is_dst_at(utc(2025, 1, 1)));
    EXPECT_TRUE(reads(zone, start - 1, {2024, 10, 6, 1, 59, 59, 36'000, false}));
    EXPECT_TRUE(reads(zone, start, {2024, 10, 6, 3, 0, 0, 39'600, true}));
    EXPECT_TRUE(reads(zone, end - 1, {2025, 4, 6, 2, 59, 59, 39'600, true}));
    EXPECT_TRUE(reads(zone, end, {2025, 4, 6, 2, 0, 0, 36'000, false}));
    // The southern year starts with the DST end, not the DST start.
    EXPECT_EQ(next_after(zone, utc(2024, 1, 1)), utc(2024, 4, 6, 16));
    EXPECT_EQ(next_after(zone, utc(2024, 4, 6, 16)), start);
    EXPECT_EQ(next_after(zone, start), end);
}

TEST(TzEngine, LordHoweShiftsByThirtyMinutes) {
    const TimeZone zone = parse_ok("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0");
    const std::int64_t start = utc(2024, 10, 5, 15, 30);
    const std::int64_t end = utc(2025, 4, 5, 15);
    EXPECT_EQ(start, 1'728'142'200);
    EXPECT_EQ(end, 1'743'865'200);
    EXPECT_TRUE(reads(zone, start - 1, {2024, 10, 6, 1, 59, 59, 37'800, false})); // gap 02:00-02:30
    EXPECT_TRUE(reads(zone, start, {2024, 10, 6, 2, 30, 0, 39'600, true}));
    EXPECT_TRUE(reads(zone, end - 1, {2025, 4, 6, 1, 59, 59, 39'600, true})); // overlap 01:30-02:00
    EXPECT_TRUE(reads(zone, end, {2025, 4, 6, 1, 30, 0, 37'800, false}));
    EXPECT_EQ(zone.abbreviation(false), "+1030");
    EXPECT_EQ(zone.abbreviation(true), "+11");
}

TEST(TzEngine, DublinHasNegativeDst) {
    const TimeZone zone = parse_ok("IST-1GMT0,M10.5.0,M3.5.0/1");
    const std::int64_t winter = utc(2024, 10, 27, 1); // "DST" (GMT, offset 0) starts
    const std::int64_t summer = utc(2025, 3, 30, 1);  // "DST" ends, IST (+1) resumes
    EXPECT_EQ(winter, 1'729'990'800);
    EXPECT_EQ(summer, 1'743'296'400);
    EXPECT_TRUE(reads(zone, winter - 1, {2024, 10, 27, 1, 59, 59, 3'600, false}));
    EXPECT_TRUE(reads(zone, winter, {2024, 10, 27, 1, 0, 0, 0, true}));
    EXPECT_TRUE(reads(zone, summer - 1, {2025, 3, 30, 0, 59, 59, 0, true}));
    EXPECT_TRUE(reads(zone, summer, {2025, 3, 30, 2, 0, 0, 3'600, false}));
    EXPECT_EQ(zone.abbreviation(zone.is_dst_at(winter)), "GMT");
    EXPECT_EQ(zone.abbreviation(zone.is_dst_at(summer)), "IST");
    EXPECT_EQ(next_after(zone, utc(2024, 6, 1)), winter);
    EXPECT_EQ(next_after(zone, winter), summer);
}

TEST(TzEngine, ChathamUsesQuarterHourOffsetsAndRuleMinutes) {
    const TimeZone zone = parse_ok("<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45");
    const std::int64_t start = utc(2024, 9, 28, 14);
    const std::int64_t end = utc(2025, 4, 5, 14);
    EXPECT_TRUE(reads(zone, start - 1, {2024, 9, 29, 2, 44, 59, 45'900, false}));
    EXPECT_TRUE(reads(zone, start, {2024, 9, 29, 3, 45, 0, 49'500, true}));
    EXPECT_TRUE(reads(zone, end - 1, {2025, 4, 6, 3, 44, 59, 49'500, true}));
    EXPECT_TRUE(reads(zone, end, {2025, 4, 6, 2, 45, 0, 45'900, false}));
    EXPECT_EQ(next_after(zone, utc(2024, 1, 1)), utc(2024, 4, 6, 14)); // 2024 DST end first
}

TEST(TzEngine, KathmanduHasAQuarterHourOffsetAndNoDst) {
    const TimeZone zone = parse_ok("<+0545>-5:45");
    EXPECT_TRUE(reads(zone, 0, {1970, 1, 1, 5, 45, 0, 20'700, false}));
    EXPECT_TRUE(reads(zone, utc(2024, 6, 1), {2024, 6, 1, 5, 45, 0, 20'700, false}));
    EXPECT_EQ(zone.abbreviation(false), "+0545");
    EXPECT_FALSE(zone.has_dst());
    EXPECT_FALSE(zone.next_transition(utc(2024, 1, 1)).has_value());
}

TEST(TzEngine, RuleTimesBeyondTheDayAndNegative) {
    // Gaza: second rule time is 50 h after Thursday midnight (RFC 9636 extension).
    const TimeZone gaza = parse_ok("EET-2EEST,M3.4.4/50,M10.4.4/50");
    const std::int64_t gaza_start = utc(2024, 3, 30, 0); // Sat 02:00 EET
    const std::int64_t gaza_end = utc(2024, 10, 25, 23); // Sat 02:00 EEST
    EXPECT_EQ(next_after(gaza, utc(2024, 1, 1)), gaza_start);
    EXPECT_EQ(next_after(gaza, gaza_start), gaza_end);
    EXPECT_TRUE(reads(gaza, gaza_start - 1, {2024, 3, 30, 1, 59, 59, 7'200, false}));
    EXPECT_TRUE(reads(gaza, gaza_start, {2024, 3, 30, 3, 0, 0, 10'800, true}));
    EXPECT_TRUE(reads(gaza, gaza_end - 1, {2024, 10, 26, 1, 59, 59, 10'800, true}));
    EXPECT_TRUE(reads(gaza, gaza_end, {2024, 10, 26, 1, 0, 0, 7'200, false}));

    // Nuuk: negative times move the transitions into the previous day.
    const TimeZone nuuk = parse_ok("<-02>2<-01>,M3.5.0/-1,M10.5.0/0");
    const std::int64_t nuuk_start = utc(2024, 3, 31, 1);
    const std::int64_t nuuk_end = utc(2024, 10, 27, 1);
    EXPECT_EQ(next_after(nuuk, utc(2024, 1, 1)), nuuk_start);
    EXPECT_EQ(next_after(nuuk, nuuk_start), nuuk_end);
    EXPECT_TRUE(reads(nuuk, nuuk_start - 1, {2024, 3, 30, 22, 59, 59, -7'200, false}));
    EXPECT_TRUE(reads(nuuk, nuuk_start, {2024, 3, 31, 0, 0, 0, -3'600, true}));
    EXPECT_TRUE(reads(nuuk, nuuk_end - 1, {2024, 10, 26, 23, 59, 59, -3'600, true}));
    EXPECT_TRUE(reads(nuuk, nuuk_end, {2024, 10, 26, 23, 0, 0, -7'200, false}));

    // Both ends of the RFC range: 167:59:59 after / before midnight.
    const TimeZone wide = parse_ok("XXX0YYY,M3.2.0/167:59:59,M11.1.0/-167:59:59");
    EXPECT_EQ(next_after(wide, utc(2024, 1, 1)), utc(2024, 3, 16, 23, 59, 59));
    EXPECT_EQ(next_after(wide, utc(2024, 4, 1)), utc(2024, 10, 26, 23, 0, 1));
    const TimeZone negative = parse_ok("XXX0YYY,M3.2.0/-24,M11.1.0/48");
    EXPECT_EQ(next_after(negative, utc(2024, 1, 1)), utc(2024, 3, 9, 0));
    EXPECT_EQ(next_after(negative, utc(2024, 3, 10)), utc(2024, 11, 4, 23));
}

// --- evaluation: the three rule kinds -----------------------------------------------------------

TEST(TzRules, JulianDayNumbersNeverCountFebruary29) {
    const TimeZone zone = parse_ok("XXX0YYY,J60,J300"); // std +0, DST +1 by default
    EXPECT_EQ(next_after(zone, utc(2023, 1, 1)), utc(2023, 3, 1, 2));
    EXPECT_EQ(next_after(zone, utc(2024, 1, 1)), utc(2024, 3, 1, 2)); // 2024 is a leap year
    EXPECT_EQ(next_after(zone, utc(2023, 3, 2)), utc(2023, 10, 27, 1));
    EXPECT_EQ(next_after(zone, utc(2024, 3, 2)), utc(2024, 10, 27, 1));
    const TimeZone january = parse_ok("XXX0YYY,J1/0,J59/0"); // J59 = Feb 28 in every year
    EXPECT_EQ(next_after(january, utc(2024, 1, 2)), utc(2024, 2, 27, 23)); // 00:00 DST = 23:00Z
}

TEST(TzRules, ZeroBasedDayNumbersCountFebruary29) {
    const TimeZone zone = parse_ok("XXX0YYY,59,300");
    EXPECT_EQ(next_after(zone, utc(2023, 1, 1)), utc(2023, 3, 1, 2));   // day 59 = Mar 1
    EXPECT_EQ(next_after(zone, utc(2024, 1, 1)), utc(2024, 2, 29, 2));  // day 59 = Feb 29
    EXPECT_EQ(next_after(zone, utc(2023, 3, 2)), utc(2023, 10, 28, 1)); // day 300
    EXPECT_EQ(next_after(zone, utc(2024, 3, 2)), utc(2024, 10, 27, 1));
}

TEST(TzRules, WeekFiveMeansTheLastOccurrence) {
    const TimeZone february = parse_ok("XXX0YYY,M2.5.0,M11.1.0");
    EXPECT_EQ(next_after(february, utc(2023, 1, 1)), utc(2023, 2, 26, 2)); // Feb 2023: 5,12,19,26
    EXPECT_EQ(next_after(february, utc(2024, 1, 1)), utc(2024, 2, 25, 2)); // Feb 2024: 4,11,18,25
    const TimeZone april = parse_ok("XXX0YYY,M4.5.0,M11.1.0");
    EXPECT_EQ(next_after(april, utc(2023, 1, 1)), utc(2023, 4, 30, 2)); // five Sundays
    EXPECT_EQ(next_after(april, utc(2024, 1, 1)), utc(2024, 4, 28, 2)); // four Sundays
    EXPECT_EQ(next_after(april, utc(2025, 1, 1)), utc(2025, 4, 27, 2));
    const TimeZone fourth = parse_ok("XXX0YYY,M4.4.0,M11.1.0");
    EXPECT_EQ(next_after(fourth, utc(2023, 1, 1)), utc(2023, 4, 23, 2));
    EXPECT_EQ(next_after(fourth, utc(2024, 1, 1)), utc(2024, 4, 28, 2));
}

TEST(TzRules, WeekdaysAndMonthEdges) {
    // January 1st on a Sunday (2023) and Saturday (2022) for M1.1.0; December M12.5.6 (last Sat).
    const TimeZone zone = parse_ok("XXX0YYY,M1.1.0/0,M12.5.6/0");
    EXPECT_EQ(next_after(zone, utc(2022, 1, 1)), utc(2022, 1, 2));       // first Sunday of Jan 2022
    EXPECT_EQ(next_after(zone, utc(2022, 12, 31)), utc(2023, 1, 1));     // Jan 1 2023 is a Sunday
    EXPECT_EQ(next_after(zone, utc(2023, 1, 2)), utc(2023, 12, 29, 23)); // Sat Dec 30 00:00 DST
    EXPECT_EQ(next_after(zone, utc(2024, 6, 1)), utc(2024, 12, 27, 23)); // Sat Dec 28 00:00 DST
}

// --- permanent DST ----------------------------------------------------------------------------

TEST(TzPermanentDst, NeverChangesAcrossAnyYearBoundary) {
    constexpr auto kZones = std::to_array<std::string_view>({
        "EST5EDT,0/0,J365/25",
        "CET-1CEST,0/0,J365/25",
        "GMT0BST,0/0,J365/25",
        "<+1030>-10:30<+11>-11,0/0,J365/25",
        "<-0330>3:30<-0230>,J1/0,J365/25",
        "<+1245>-12:45<+1345>,J1/0,J365/25",
    });
    for (const std::string_view posix : kZones) {
        const TimeZone zone = parse_ok(posix);
        ASSERT_TRUE(zone.has_dst()) << posix;
        const std::int32_t dst_offset = zone.utc_offset_at(utc(2024, 7, 1));
        EXPECT_NE(dst_offset, 0) << posix; // sanity: a DST offset was applied
        std::size_t index = 0;
        // Hourly across a leap and a non-leap year boundary, plus the same near 2100.
        for (const std::int64_t begin : {utc(2019, 12, 30), utc(2099, 12, 30)}) {
            for (std::int64_t t = begin; t < begin + (6 * kSecondsPerDay); t += 1'800, ++index) {
                if (!zone.is_dst_at(t) || zone.utc_offset_at(t) != dst_offset) {
                    ADD_FAILURE() << posix << ": not DST at t=" << t;
                    return;
                }
                if (index % 53 == 0 && zone.next_transition(t).has_value()) {
                    ADD_FAILURE() << posix << ": has a transition after t=" << t;
                    return;
                }
            }
        }
        EXPECT_EQ(zone.abbreviation(zone.is_dst_at(0)), zone.abbreviation(true));
    }
}

TEST(TzPermanentDst, ANearlyPermanentRuleStillSwitchesForAnHour) {
    // DST ends one hour before it would restart: 0/0,J365/24 leaves one hour of standard time.
    const TimeZone zone = parse_ok("EST5EDT,0/0,J365/24");
    const std::int64_t gap_begin = utc(2024, 1, 1, 4); // Dec 31 24:00 EDT = Jan 1 00:00 EDT
    EXPECT_TRUE(zone.is_dst_at(gap_begin - 1));
    EXPECT_FALSE(zone.is_dst_at(gap_begin));
    EXPECT_FALSE(zone.is_dst_at(gap_begin + 3'599));
    EXPECT_TRUE(zone.is_dst_at(gap_begin + 3'600));
    EXPECT_EQ(next_after(zone, gap_begin - 1), gap_begin);
    EXPECT_EQ(next_after(zone, gap_begin), gap_begin + 3'600);
}

// --- next_transition --------------------------------------------------------------------------

TEST(TzNextTransition, IsStrictlyAfterAndWalksTheYears) {
    const TimeZone zone = parse_ok("CET-1CEST,M3.5.0,M10.5.0/3");
    const auto chain = std::to_array<std::int64_t>(
        {utc(2024, 3, 31, 1), utc(2024, 10, 27, 1), utc(2025, 3, 30, 1), utc(2025, 10, 26, 1)});
    std::int64_t cursor = utc(2024, 1, 1);
    for (const std::int64_t expected : chain) {
        const std::int64_t next = next_after(zone, cursor);
        EXPECT_EQ(next, expected);
        cursor = next;
    }
    EXPECT_EQ(next_after(zone, chain[0] - 1), chain[0]);
    EXPECT_EQ(next_after(zone, chain[0]), chain[1]); // exactly at a transition: the following one
    EXPECT_EQ(next_after(zone, chain[0] + 1), chain[1]);
    EXPECT_EQ(next_after(zone, utc(1970, 1, 1)), utc(1970, 3, 29, 1));
    EXPECT_EQ(next_after(zone, utc(2100, 1, 1)), utc(2100, 3, 28, 1));
    // Late December: the next one is in the following year.
    EXPECT_EQ(next_after(zone, utc(2024, 12, 31, 23, 59, 59)), chain[2]);
}

TEST(TzNextTransition, NoneForFixedOffsetsAndTheDefaultZone) {
    for (const std::string_view posix : {"UTC0"sv, "EST5"sv, "<+0545>-5:45"sv}) {
        const TimeZone zone = parse_ok(posix);
        EXPECT_FALSE(zone.next_transition(0).has_value()) << posix;
        EXPECT_FALSE(zone.next_transition(utc(2024, 6, 1)).has_value()) << posix;
    }
    EXPECT_FALSE(TimeZone{}.next_transition(utc(2024, 6, 1)).has_value());
}

// --- to_utc: gap and overlap policies --------------------------------------------------------

struct Resolution {
    std::int64_t earlier;
    std::int64_t later;
    bool rejected;
};

/// Resolves a wall time under all three policies.
Resolution resolve(const TimeZone& zone, const CivilDate& date, const CivilTime& time) {
    const Result<UnixSeconds> earlier = zone.to_utc(date, time, GapPolicy::kEarlier);
    const Result<UnixSeconds> later = zone.to_utc(date, time, GapPolicy::kLater);
    const Result<UnixSeconds> strict = zone.to_utc(date, time, GapPolicy::kReject);
    if (strict && (!earlier || !later || *earlier != *strict || *later != *strict)) {
        ADD_FAILURE() << "an unambiguous time must resolve identically under every policy";
    }
    if (!strict && strict.error() != Error{Errc::kBadArgs}) {
        ADD_FAILURE() << "kReject must fail with kBadArgs";
    }
    return Resolution{
        earlier.value_or(kNoTransition), later.value_or(kNoTransition), !strict.has_value()};
}

TEST(TzToUtc, BerlinGapShiftsForwardUnderBothPolicies) {
    const TimeZone zone = parse_ok("CET-1CEST,M3.5.0,M10.5.0/3");
    const CivilDate day{2024, 3, 31}; // 02:00-03:00 does not exist
    const Resolution inside = resolve(zone, day, CivilTime{2, 30, 0});
    EXPECT_TRUE(inside.rejected);
    EXPECT_EQ(inside.earlier, utc(2024, 3, 31, 1, 30)); // read as CET, displays 03:30 CEST
    EXPECT_EQ(inside.later, utc(2024, 3, 31, 1, 30));
    const Resolution first = resolve(zone, day, CivilTime{2, 0, 0});
    EXPECT_TRUE(first.rejected);
    EXPECT_EQ(first.earlier, utc(2024, 3, 31, 1));
    const Resolution last = resolve(zone, day, CivilTime{2, 59, 59});
    EXPECT_TRUE(last.rejected);
    EXPECT_EQ(last.earlier, utc(2024, 3, 31, 1, 59, 59));
    const Resolution before = resolve(zone, day, CivilTime{1, 59, 59});
    EXPECT_FALSE(before.rejected);
    EXPECT_EQ(before.earlier, utc(2024, 3, 31, 0, 59, 59));
    const Resolution after = resolve(zone, day, CivilTime{3, 0, 0});
    EXPECT_FALSE(after.rejected);
    EXPECT_EQ(after.earlier, utc(2024, 3, 31, 1));
}

TEST(TzToUtc, BerlinOverlapHonoursEarlierAndLater) {
    const TimeZone zone = parse_ok("CET-1CEST,M3.5.0,M10.5.0/3");
    const CivilDate day{2024, 10, 27}; // 02:00-03:00 happens twice
    const Resolution inside = resolve(zone, day, CivilTime{2, 30, 0});
    EXPECT_TRUE(inside.rejected);
    EXPECT_EQ(inside.earlier, utc(2024, 10, 27, 0, 30)); // CEST
    EXPECT_EQ(inside.later, utc(2024, 10, 27, 1, 30));   // CET
    const Resolution first = resolve(zone, day, CivilTime{2, 0, 0});
    EXPECT_EQ(first.earlier, utc(2024, 10, 27, 0));
    EXPECT_EQ(first.later, utc(2024, 10, 27, 1));
    const Resolution last = resolve(zone, day, CivilTime{2, 59, 59});
    EXPECT_EQ(last.earlier, utc(2024, 10, 27, 0, 59, 59));
    EXPECT_EQ(last.later, utc(2024, 10, 27, 1, 59, 59));
    const Resolution before = resolve(zone, day, CivilTime{1, 59, 59});
    EXPECT_FALSE(before.rejected);
    EXPECT_EQ(before.earlier, utc(2024, 10, 26, 23, 59, 59));
    const Resolution after = resolve(zone, day, CivilTime{3, 0, 0});
    EXPECT_FALSE(after.rejected);
    EXPECT_EQ(after.earlier, utc(2024, 10, 27, 2));
}

TEST(TzToUtc, LordHoweHalfHourGapAndOverlap) {
    const TimeZone zone = parse_ok("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0");
    const Resolution gap = resolve(zone, CivilDate{2024, 10, 6}, CivilTime{2, 15, 0});
    EXPECT_TRUE(gap.rejected);
    EXPECT_EQ(gap.earlier, utc(2024, 10, 5, 15, 45)); // 02:45 +11
    EXPECT_EQ(gap.later, utc(2024, 10, 5, 15, 45));
    EXPECT_FALSE(resolve(zone, CivilDate{2024, 10, 6}, CivilTime{2, 30, 0}).rejected);
    const Resolution overlap = resolve(zone, CivilDate{2025, 4, 6}, CivilTime{1, 45, 0});
    EXPECT_TRUE(overlap.rejected);
    EXPECT_EQ(overlap.earlier, utc(2025, 4, 5, 14, 45)); // +11
    EXPECT_EQ(overlap.later, utc(2025, 4, 5, 15, 15));   // +10:30
    EXPECT_FALSE(resolve(zone, CivilDate{2025, 4, 6}, CivilTime{1, 29, 59}).rejected);
    EXPECT_FALSE(resolve(zone, CivilDate{2025, 4, 6}, CivilTime{2, 0, 0}).rejected);
}

TEST(TzToUtc, DublinNegativeDstGapAndOverlap) {
    const TimeZone zone = parse_ok("IST-1GMT0,M10.5.0,M3.5.0/1");
    // March: 01:00 GMT jumps to 02:00 IST, so 01:30 never happens.
    const Resolution gap = resolve(zone, CivilDate{2025, 3, 30}, CivilTime{1, 30, 0});
    EXPECT_TRUE(gap.rejected);
    EXPECT_EQ(gap.earlier, utc(2025, 3, 30, 1, 30));
    EXPECT_EQ(gap.later, utc(2025, 3, 30, 1, 30));
    // October: 02:00 IST falls back to 01:00 GMT, so 01:30 happens twice (IST first).
    const Resolution overlap = resolve(zone, CivilDate{2024, 10, 27}, CivilTime{1, 30, 0});
    EXPECT_TRUE(overlap.rejected);
    EXPECT_EQ(overlap.earlier, utc(2024, 10, 27, 0, 30));
    EXPECT_EQ(overlap.later, utc(2024, 10, 27, 1, 30));
}

TEST(TzToUtc, ResolvedInstantsRoundTripThroughToLocal) {
    const TimeZone zone = parse_ok("EST5EDT,M3.2.0,M11.1.0");
    // Overlap 2024-11-03 01:30 happens twice; each resolution reads back as 01:30.
    const Resolution overlap = resolve(zone, CivilDate{2024, 11, 3}, CivilTime{1, 30, 0});
    EXPECT_TRUE(reads(zone, overlap.earlier, {2024, 11, 3, 1, 30, 0, -14'400, true}));
    EXPECT_TRUE(reads(zone, overlap.later, {2024, 11, 3, 1, 30, 0, -18'000, false}));
    // Gap 2024-03-10 02:30 reads back one gap-length later.
    const Resolution gap = resolve(zone, CivilDate{2024, 3, 10}, CivilTime{2, 30, 0});
    EXPECT_TRUE(reads(zone, gap.earlier, {2024, 3, 10, 3, 30, 0, -14'400, true}));
}

TEST(TzToUtc, FixedOffsetZonesAreAlwaysUnambiguous) {
    const TimeZone zone = parse_ok("<+0545>-5:45");
    const Resolution noon = resolve(zone, CivilDate{2024, 6, 1}, CivilTime{12, 0, 0});
    EXPECT_FALSE(noon.rejected);
    EXPECT_EQ(noon.earlier, utc(2024, 6, 1, 6, 15));
    const Resolution epoch = resolve(zone, CivilDate{1970, 1, 1}, CivilTime{0, 0, 0});
    EXPECT_FALSE(epoch.rejected);
    EXPECT_EQ(epoch.earlier, -20'700); // before the epoch in UTC
    const Resolution utc_zone = resolve(TimeZone{}, CivilDate{2199, 12, 31}, CivilTime{23, 59, 59});
    EXPECT_FALSE(utc_zone.rejected);
    EXPECT_EQ(utc_zone.earlier, utc(2199, 12, 31, 23, 59, 59));
}

TEST(TzToUtc, PermanentDstIsUnambiguousEvenAtNewYear) {
    const TimeZone zone = parse_ok("EST5EDT,0/0,J365/25");
    for (const CivilDate& day :
         {CivilDate{2024, 1, 1}, CivilDate{2024, 12, 31}, CivilDate{2025, 1, 1}}) {
        const Resolution r = resolve(zone, day, CivilTime{0, 30, 0});
        EXPECT_FALSE(r.rejected);
        EXPECT_EQ(r.earlier,
                  (std::int64_t{days_from_civil(day)} * kSecondsPerDay) + 1'800 + 14'400);
    }
}

TEST(TzToUtc, RejectsInvalidCivilFields) {
    const TimeZone zone = parse_ok("CET-1CEST,M3.5.0,M10.5.0/3");
    struct Bad {
        CivilDate date;
        CivilTime time;
    };
    for (const Bad& bad : {Bad{{2024, 2, 30}, {0, 0, 0}},
                           Bad{{2024, 13, 1}, {0, 0, 0}},
                           Bad{{2024, 1, 1}, {24, 0, 0}},
                           Bad{{2024, 1, 1}, {0, 60, 0}},
                           Bad{{2024, 1, 1}, {0, 0, 60}},
                           Bad{{1969, 12, 31}, {23, 0, 0}},
                           Bad{{2200, 1, 1}, {0, 0, 0}}}) {
        for (const GapPolicy policy :
             {GapPolicy::kEarlier, GapPolicy::kLater, GapPolicy::kReject}) {
            const Result<UnixSeconds> result = zone.to_utc(bad.date, bad.time, policy);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), Error{Errc::kBadArgs});
        }
    }
}

// --- domain and heap -----------------------------------------------------------------------------

TEST(TzDomain, ExtremeInstantsNeverOverflow) {
    constexpr auto kLimits =
        std::to_array<std::int64_t>({std::numeric_limits<std::int64_t>::min(),
                                     std::numeric_limits<std::int64_t>::min() + 1,
                                     -100'000'000'000'001,
                                     100'000'000'000'001,
                                     std::numeric_limits<std::int64_t>::max() - 1,
                                     std::numeric_limits<std::int64_t>::max()});
    for (const std::string_view posix : {"UTC0"sv,
                                         "CET-1CEST,M3.5.0,M10.5.0/3"sv,
                                         "AEST-10AEDT,M10.1.0,M4.1.0/3"sv,
                                         "EST5EDT,0/0,J365/25"sv}) {
        const TimeZone zone = parse_ok(posix);
        for (const std::int64_t t : kLimits) {
            const LocalDateTime local = zone.to_local(t);
            EXPECT_GE(local.date.month, 1) << posix;
            EXPECT_LE(local.date.month, 12) << posix;
            EXPECT_GE(local.date.day, 1) << posix;
            EXPECT_LE(local.date.day, 31) << posix;
            EXPECT_LT(local.time.hour, 24) << posix;
            EXPECT_LT(local.time.minute, 60) << posix;
            EXPECT_LT(local.time.second, 60) << posix;
            EXPECT_EQ(local.utc_offset_s, zone.utc_offset_at(t)) << posix;
            EXPECT_EQ(local.is_dst, zone.is_dst_at(t)) << posix;
            (void)zone.next_transition(t); // must not trap; the value itself is unspecified
        }
    }
}

bool allocation_counting_works() {
    const std::size_t before = allocation_counter().load(std::memory_order_relaxed);
    auto* probe = new std::uint8_t[64];              // NOLINT(cppcoreguidelines-owning-memory)
    *static_cast<volatile std::uint8_t*>(probe) = 1; // keep the allocation observable
    const std::size_t after = allocation_counter().load(std::memory_order_relaxed);
    delete[] probe; // NOLINT(cppcoreguidelines-owning-memory)
    return after > before;
}

TEST(TzNoHeap, EngineAndFormattersNeverAllocate) {
    if (!allocation_counting_works()) {
        GTEST_SKIP() << "allocation counting needs the AddressSanitizer allocator hook";
    }
    std::array<char, 32> text{};
    std::int64_t sink = 0;
    const std::size_t before = allocation_counter().load(std::memory_order_relaxed);
    for (const AcceptCase& c : kAccepted) {
        const Result<TimeZone> parsed = TimeZone::parse(c.posix);
        if (parsed) {
            const TimeZone& zone = *parsed;
            sink += zone.utc_offset_at(1'700'000'000);
            sink += zone.to_local(1'700'000'000).utc_offset_s;
            sink += zone.next_transition(1'700'000'000).value_or(0);
            sink += zone.to_utc(CivilDate{2024, 3, 31}, CivilTime{2, 30, 0}, GapPolicy::kEarlier)
                        .value_or(0);
            sink += static_cast<std::int64_t>(zone.abbreviation(true).size());
        }
    }
    for (const std::string_view bad : kRejected) {
        sink += TimeZone::parse(bad).has_value() ? 1 : 0;
    }
    sink += static_cast<std::int64_t>(
        format_hhmm(text, CivilTime{13, 5, 0}, HourFormat::k12h, nullptr));
    sink += static_cast<std::int64_t>(format_iso8601_utc(text, 1'700'000'000));
    sink += static_cast<std::int64_t>(weekday_name(Weekday::kMonday, false).size());
    const std::size_t after = allocation_counter().load(std::memory_order_relaxed);
    EXPECT_EQ(after - before, 0U);
    EXPECT_NE(sink, 0); // keeps the calls observable
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::time
