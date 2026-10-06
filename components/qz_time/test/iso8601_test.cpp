// parse_iso8601 (qz/time/timekeeper.hpp): accept and reject tables.
#include "qz/time/timekeeper.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace qz::time {
namespace {

static_assert(noexcept(parse_iso8601(std::string_view{}, TimeZone{})));

TimeZone zone(std::string_view posix) {
    const Result<TimeZone> parsed = TimeZone::parse(posix);
    EXPECT_TRUE(parsed.has_value()) << posix;
    return parsed ? *parsed : TimeZone{};
}

struct Accept {
    std::string_view text;
    UnixSeconds expected;
};

TEST(Iso8601, AcceptsExplicitOffsets) {
    const TimeZone utc;
    // Expected values were produced with GNU date -u -d (independent of the library).
    const std::array<Accept, 14> table{{
        {"1970-01-01T00:00Z", 0},
        {"1970-01-01T00:00:00Z", 0},
        {"1970-01-01T00:00:01Z", 1},
        {"2024-02-29T12:34:56Z", 1'709'210'096},
        {"2024-02-29T12:34Z", 1'709'210'040},
        {"2199-12-31T23:59:59Z", 7'258'118'399},
        {"2024-01-01T12:00:00+05:30", 1'704'090'600},
        {"2024-01-01T12:00+05:30", 1'704'090'600},
        {"2024-01-01T12:00:00-08:00", 1'704'139'200},
        {"2024-01-01T04:00:00-08:00", 1'704'110'400},
        {"2024-01-01T12:00-00:00", 1'704'110'400},
        {"2024-01-01T12:00+00:00", 1'704'110'400},
        {"1970-01-01T01:00+01:00", 0},
        {"2000-02-29T23:59:59-23:59", 951'868'799 + 86'340},
    }};
    for (const Accept& row : table) {
        const Result<UnixSeconds> result = parse_iso8601(row.text, utc);
        ASSERT_TRUE(result.has_value()) << row.text;
        EXPECT_EQ(*result, row.expected) << row.text;
    }
}

TEST(Iso8601, ExplicitOffsetIgnoresTheZone) {
    const TimeZone chicago = zone("CST6CDT,M3.2.0,M11.1.0");
    const Result<UnixSeconds> z = parse_iso8601("2024-07-04T17:00Z", chicago);
    const Result<UnixSeconds> off = parse_iso8601("2024-07-04T12:00-05:00", chicago);
    ASSERT_TRUE(z.has_value() && off.has_value());
    EXPECT_EQ(*z, 1'720'112'400);
    EXPECT_EQ(*off, *z);
}

TEST(Iso8601, WithoutOffsetTheValueIsLocalTimeInTheZone) {
    const TimeZone chicago = zone("CST6CDT,M3.2.0,M11.1.0");
    const struct {
        std::string_view text;
        UnixSeconds expected;
    } table[] = {
        {"2024-07-04T12:00", 1'720'112'400}, // CDT, UTC-5
        {"2024-07-04T12:00:00", 1'720'112'400},
        {"2024-01-01T12:00", 1'704'132'000}, // CST, UTC-6
        {"2024-03-10T02:30", 1'710'059'400}, // gap: shifted forward to 03:30 CDT = 08:30Z
        {"2024-11-03T01:30", 1'730'615'400}, // overlap: first occurrence (CDT) = 06:30Z
    };
    for (const auto& row : table) {
        const Result<UnixSeconds> result = parse_iso8601(row.text, chicago);
        ASSERT_TRUE(result.has_value()) << row.text;
        EXPECT_EQ(*result, row.expected) << row.text;
    }
    // The default zone is UTC.
    const Result<UnixSeconds> in_utc = parse_iso8601("2024-02-29T12:34:56", TimeZone{});
    ASSERT_TRUE(in_utc.has_value());
    EXPECT_EQ(*in_utc, 1'709'210'096);
}

TEST(Iso8601, RejectsMalformedAndOutOfRangeText) {
    const TimeZone utc;
    const std::string_view rejects[] = {
        "",
        "2024",
        "2024-01-01",
        "2024-01-01T",
        "2024-01-01T12",
        "2024-01-01T12:",
        "2024-01-01T12:0",
        "2024-01-01 12:00",  // space separator
        "2024-01-01t12:00",  // lowercase separator
        "2024-01-01T12:00z", // lowercase zulu
        "2024-01-01T12:00:",
        "2024-01-01T12:00:5",
        "2024-01-01T12:00:5Z",
        "2024-01-01T12:00:00.5Z", // fractions are not accepted
        "2024-01-01T12:00ZZ",
        "2024-01-01T12:00 Z",
        " 2024-01-01T12:00Z",
        "2024-01-01T12:00Z ",
        "2024-01-01T12:00Z\n",
        "2024-01-01T12:00+0100", // basic offset form
        "2024-01-01T12:00+01",
        "2024-01-01T12:00+1:00",
        "2024-01-01T12:00+01:0",
        "2024-01-01T12:00+01:000",
        "2024-01-01T12:00+24:00",
        "2024-01-01T12:00-24:00",
        "2024-01-01T12:00+01:60",
        "2024-01-01T12:00+aa:bb",
        "2024-01-01T12:00 01:00",
        "2024-01-01T12:00:00+",
        "20240101T1200Z", // basic date form
        "24-01-01T12:00Z",
        "02024-01-01T12:00Z",
        "2024/01/01T12:00Z",
        "2024-1-01T12:00Z",
        "2024-01-1T12:00Z",
        "2024-01-01T12.00Z",
        "2O24-01-01T12:00Z", // letter O
        "+024-01-01T12:00Z",
        "2024-01-01T1 :00Z",
        "2024-13-01T00:00Z",
        "2024-00-01T00:00Z",
        "2024-01-00T00:00Z",
        "2024-01-32T00:00Z",
        "2024-04-31T00:00Z",
        "2024-02-30T00:00Z",
        "2023-02-29T00:00Z",
        "2100-02-29T00:00Z", // 2100 is not a leap year
        "2024-01-01T24:00Z",
        "2024-01-01T12:60Z",
        "2024-01-01T12:00:60Z", // no leap seconds
        "1969-12-31T23:59:59Z",
        "1970-01-01T00:00+01:00", // before the epoch once the offset is applied
        "1970-01-01T00:00:00+00:01",
        "2200-01-01T00:00Z",
        "2199-12-31T23:59-01:00", // past the window once the offset is applied
        "\xEF\xBC\x92\xEF\xBC\x90\xEF\xBC\x92\xEF\xBC\x94-01-01T12:00Z",
    };
    for (const std::string_view text : rejects) {
        const Result<UnixSeconds> result = parse_iso8601(text, utc);
        ASSERT_FALSE(result.has_value()) << text;
        EXPECT_EQ(result.error().code, Errc::kBadArgs) << text;
    }
    // A string_view carrying an embedded NUL is rejected too (the literal above is cut at the NUL).
    const char with_nul[] = {'2',
                             '0',
                             '2',
                             '4',
                             '-',
                             '0',
                             '1',
                             '-',
                             '0',
                             '1',
                             'T',
                             '1',
                             '2',
                             ':',
                             '0',
                             '0',
                             'Z',
                             '\0',
                             'Z'};
    EXPECT_FALSE(parse_iso8601(std::string_view{with_nul, sizeof(with_nul)}, utc).has_value());
}

TEST(Iso8601, WindowEdgesAreInclusiveAtTheStartExclusiveAtTheEnd) {
    const TimeZone utc;
    ASSERT_TRUE(parse_iso8601("1970-01-01T00:00:00Z", utc).has_value());
    ASSERT_TRUE(parse_iso8601("2199-12-31T23:59:59Z", utc).has_value());
    EXPECT_FALSE(parse_iso8601("2200-01-01T00:00:00Z", utc).has_value());
    // Local times that map outside the window are rejected as well.
    const TimeZone minus_ten = zone("HST10");
    EXPECT_FALSE(parse_iso8601("2199-12-31T23:59:59", zone("<-0100>1")).has_value());
    EXPECT_FALSE(parse_iso8601("2199-12-31T23:59:59", minus_ten).has_value());
    EXPECT_FALSE(parse_iso8601("1970-01-01T00:00:00", zone("<+0100>-1")).has_value());
}

TEST(Iso8601, RoundTripsWithFormatIso8601Utc) {
    const TimeZone utc;
    for (const UnixSeconds t : {UnixSeconds{0},
                                UnixSeconds{86'399},
                                UnixSeconds{951'782'400},
                                UnixSeconds{1'709'210'096},
                                UnixSeconds{1'780'000'000},
                                UnixSeconds{4'102'444'799},
                                UnixSeconds{7'258'118'399}}) {
        std::array<char, 32> buffer{};
        const std::size_t length = format_iso8601_utc(buffer, t);
        ASSERT_GT(length, 0U);
        const Result<UnixSeconds> parsed = parse_iso8601({buffer.data(), length}, utc);
        ASSERT_TRUE(parsed.has_value()) << std::string_view(buffer.data(), length);
        EXPECT_EQ(*parsed, t);
    }
}

} // namespace
} // namespace qz::time
