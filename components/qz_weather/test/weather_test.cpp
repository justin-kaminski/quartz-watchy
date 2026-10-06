// qz_weather tests: Open-Meteo URL, parser fixtures (synthetic JSON), WMO map, freshness, units.
#include "qz/weather/provider.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace qz::weather {
namespace {

using model::Location;
using model::TempUnit;
using model::WeatherCondition;
using model::WeatherFreshness;
using model::WeatherReport;

constexpr time::UnixSeconds kNow = 1'800'000'000;
constexpr time::UnixSeconds kMin = 60;
constexpr time::UnixSeconds kHour = 3600;

std::string url_for(const Location& loc) {
    std::array<char, kMaxUrlBytes> buf{};
    const OpenMeteoProvider p;
    const auto r = p.build_url(loc, buf);
    EXPECT_TRUE(r.has_value());
    if (!r) {
        return {};
    }
    EXPECT_EQ(buf[*r], '\0');
    return {buf.data(), *r};
}

constexpr std::string_view kUrlTail =
    "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
    "&timezone=auto&forecast_days=1&temperature_unit=celsius";

// ---------------------------------------------------------------- URL
TEST(OpenMeteoUrl, ExactForSampleCoordinates) {
    EXPECT_EQ(url_for({4'175'080, -8'814'700}),
              std::string("https://api.open-meteo.com/v1/forecast?latitude=41.75080"
                          "&longitude=-88.14700") +
                  std::string(kUrlTail));
}

TEST(OpenMeteoUrl, NegativeLatitudeAndSmallFractions) {
    EXPECT_EQ(url_for({-3'386'000, 15'120'005}),
              std::string("https://api.open-meteo.com/v1/forecast?latitude=-33.86000"
                          "&longitude=151.20005") +
                  std::string(kUrlTail));
}

TEST(OpenMeteoUrl, SubDegreeValuesKeepLeadingZeros) {
    const std::string u = url_for({-1, 7});
    EXPECT_NE(u.find("latitude=-0.00001&longitude=0.00007&"), std::string::npos);
    const std::string z = url_for({0, 0});
    EXPECT_NE(z.find("latitude=0.00000&longitude=0.00000&"), std::string::npos);
}

TEST(OpenMeteoUrl, Extremes) {
    EXPECT_NE(url_for({9'000'000, 18'000'000}).find("latitude=90.00000&longitude=180.00000&"),
              std::string::npos);
    EXPECT_NE(url_for({-9'000'000, -18'000'000}).find("latitude=-90.00000&longitude=-180.00000&"),
              std::string::npos);
}

TEST(OpenMeteoUrl, FitsWithinMaxUrlBytes) {
    EXPECT_LT(url_for({-9'000'000, -18'000'000}).size(), kMaxUrlBytes);
}

TEST(OpenMeteoUrl, BufferBoundaries) {
    const OpenMeteoProvider p;
    const Location loc{4'175'080, -8'814'700};
    const std::size_t len = url_for(loc).size();
    std::array<char, kMaxUrlBytes> big{};
    // Exactly len + NUL fits; one byte less does not (no partial write promised, no overrun).
    EXPECT_EQ(*p.build_url(loc, std::span<char>(big).first(len + 1)), len);
    const auto too_small = p.build_url(loc, std::span<char>(big).first(len));
    ASSERT_FALSE(too_small.has_value());
    EXPECT_EQ(too_small.error().code, Errc::kNoSpace);
    const auto empty = p.build_url(loc, std::span<char>{});
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code, Errc::kNoSpace);
}

TEST(OpenMeteoUrl, OutOfRangeCoordinatesRejected) {
    const OpenMeteoProvider p;
    std::array<char, kMaxUrlBytes> buf{};
    for (const Location loc : {Location{9'000'001, 0},
                               Location{-9'000'001, 0},
                               Location{0, 18'000'001},
                               Location{0, -18'000'001},
                               Location{INT32_MIN, 0},
                               Location{0, INT32_MAX}}) {
        const auto r = p.build_url(loc, buf);
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, Errc::kBadArgs);
    }
}

TEST(OpenMeteoUrl, Name) {
    EXPECT_EQ(OpenMeteoProvider{}.name(), "open-meteo");
}

// ---------------------------------------------------------------- parse
Result<WeatherReport> parse(std::string_view body) {
    return OpenMeteoProvider{}.parse(body, kNow);
}

constexpr std::string_view kNormal = R"({
  "latitude": 41.75, "longitude": -88.14, "elevation": 215.0, "timezone": "America/Chicago",
  "utc_offset_seconds": -18000,
  "current_units": {"temperature_2m": "°C", "weather_code": "wmo code", "is_day": ""},
  "current": {"time": "2026-10-06T12:00", "interval": 900,
              "temperature_2m": 18.46, "weather_code": 61, "is_day": 1},
  "daily_units": {"temperature_2m_max": "°C", "temperature_2m_min": "°C"},
  "daily": {"time": ["2026-10-06"], "temperature_2m_max": [21.25], "temperature_2m_min": [-3.04]}
})";

TEST(OpenMeteoParse, NormalReport) {
    const auto r = parse(kNormal);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->temp_dc, 185);
    EXPECT_EQ(r->high_dc, 213); // 212.5 -> half away from zero (21.25*10 exact in binary)
    EXPECT_EQ(r->low_dc, -30);
    EXPECT_EQ(r->condition, WeatherCondition::kRain);
    EXPECT_EQ(r->has_high_low, 1);
    EXPECT_EQ(r->valid, 1);
    EXPECT_EQ(r->faked, 0);
    EXPECT_EQ(r->fetched_utc, kNow);
}

TEST(OpenMeteoParse, IntegerAndExponentNumbers) {
    const auto r = parse(R"({"current":{"temperature_2m":13,"weather_code":0},
        "daily":{"temperature_2m_max":[1.5e1],"temperature_2m_min":[-2E0]}})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->temp_dc, 130);
    EXPECT_EQ(r->high_dc, 150);
    EXPECT_EQ(r->low_dc, -20);
    EXPECT_EQ(r->condition, WeatherCondition::kClear);
}

TEST(OpenMeteoParse, NegativeRoundingHalfAwayFromZero) {
    const auto r = parse(R"({"current":{"temperature_2m":-0.25,"weather_code":3},
        "daily":{"temperature_2m_max":[-0.35],"temperature_2m_min":[-12.5]}})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->temp_dc, -3); // -2.5 -> -3
    EXPECT_EQ(r->high_dc, -4); // -3.5 -> -4
    EXPECT_EQ(r->low_dc, -125);
}

TEST(OpenMeteoParse, TinyNegativeIsZeroNotMinusZero) {
    const auto r = parse(R"({"current":{"temperature_2m":-0.04,"weather_code":0}})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->temp_dc, 0);
}

TEST(OpenMeteoParse, MissingDailyKeepsCurrent) {
    const auto r = parse(R"({"current":{"temperature_2m":5.0,"weather_code":71}})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->temp_dc, 50);
    EXPECT_EQ(r->has_high_low, 0);
    EXPECT_EQ(r->high_dc, 0);
    EXPECT_EQ(r->low_dc, 0);
    EXPECT_EQ(r->condition, WeatherCondition::kSnow);
    EXPECT_EQ(r->valid, 1);
}

TEST(OpenMeteoParse, BadDailyDropsOnlyHighLow) {
    for (const std::string_view daily : {
             R"("daily": null)",
             R"("daily": 5)",
             R"("daily": [])",
             R"("daily": {})",
             R"("daily": {"temperature_2m_max":[20.0]})",
             R"("daily": {"temperature_2m_min":[1.0]})",
             R"("daily": {"temperature_2m_max":[],"temperature_2m_min":[1.0]})",
             R"("daily": {"temperature_2m_max":[null],"temperature_2m_min":[1.0]})",
             R"("daily": {"temperature_2m_max":["20"],"temperature_2m_min":[1.0]})",
             R"("daily": {"temperature_2m_max":20.0,"temperature_2m_min":[1.0]})",
             R"("daily": {"temperature_2m_max":[20.0],"temperature_2m_min":[1e9]})",
         }) {
        const std::string body =
            R"({"current":{"temperature_2m":7.0,"weather_code":2},)" + std::string(daily) + "}";
        const auto r = parse(body);
        ASSERT_TRUE(r.has_value()) << daily;
        EXPECT_EQ(r->has_high_low, 0) << daily;
        EXPECT_EQ(r->temp_dc, 70) << daily;
        EXPECT_EQ(r->condition, WeatherCondition::kPartlyCloudy) << daily;
    }
}

TEST(OpenMeteoParse, NullsInCurrentAreCorrupt) {
    for (const std::string_view cur : {
             R"({"temperature_2m":null,"weather_code":0})",
             R"({"temperature_2m":1.0,"weather_code":null})",
             R"({"temperature_2m":null,"weather_code":null})",
         }) {
        const std::string body = R"({"current":)" + std::string(cur) + "}";
        const auto r = parse(body);
        ASSERT_FALSE(r.has_value()) << cur;
        EXPECT_EQ(r.error().code, Errc::kCorrupt) << cur;
    }
}

TEST(OpenMeteoParse, WrongTypesAndMissingFieldsAreCorrupt) {
    for (const std::string_view body : {
             R"({})",
             R"([])",
             R"(42)",
             R"("text")",
             R"(null)",
             R"({"current":null})",
             R"({"current":[]})",
             R"({"current":"x"})",
             R"({"current":{}})",
             R"({"current":{"temperature_2m":1.0}})",
             R"({"current":{"weather_code":1}})",
             R"({"current":{"temperature_2m":"1.0","weather_code":1}})",
             R"({"current":{"temperature_2m":1.0,"weather_code":"1"}})",
             R"({"current":{"temperature_2m":true,"weather_code":1}})",
             R"({"current":{"temperature_2m":[1.0],"weather_code":1}})",
             R"({"current":{"temperature_2m":1.0,"weather_code":1.5}})",
             R"({"current":{"temperature_2m":1.0,"weather_code":-1}})",
             R"({"current":{"temperature_2m":1.0,"weather_code":100000}})",
             R"({"current":{"temperature_2m":150.0,"weather_code":1}})",
             R"({"current":{"temperature_2m":-100.5,"weather_code":1}})",
             R"({"current":{"temperature_2m":1e999,"weather_code":1}})",
             R"({"current":{"temperature_2m":-1e999,"weather_code":1}})",
             R"({"Current":{"temperature_2m":1.0,"weather_code":1}})",
             R"({"current":{"temperature_2m":1.0,"weather_code":1})", // unterminated
             R"({"current":{"temperature_2m":1.0,"weather_code":)",
             R"({"current":{"tempe)",
             R"({)",
             R"(not json at all)",
             "",
         }) {
        const auto r = parse(body);
        ASSERT_FALSE(r.has_value()) << body;
        EXPECT_EQ(r.error().code, Errc::kCorrupt) << body;
    }
}

TEST(OpenMeteoParse, EveryTruncationOfValidBodyIsCorrupt) {
    // The body ends with '}' of the root object; any strict prefix cannot be valid JSON.
    for (std::size_t n = 0; n < kNormal.size(); ++n) {
        const auto r = parse(kNormal.substr(0, n));
        ASSERT_FALSE(r.has_value()) << "prefix " << n;
        EXPECT_EQ(r.error().code, Errc::kCorrupt);
    }
    EXPECT_TRUE(parse(kNormal).has_value());
}

TEST(OpenMeteoParse, SizeLimit) {
    // Exactly kMaxBodyBytes with trailing whitespace padding is accepted; one more is not.
    std::string at_limit(kNormal);
    at_limit.resize(kMaxBodyBytes, ' ');
    ASSERT_EQ(at_limit.size(), kMaxBodyBytes);
    EXPECT_TRUE(parse(at_limit).has_value());

    std::string over(kNormal);
    over.resize(kMaxBodyBytes + 1, ' ');
    const auto r = parse(over);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Errc::kCorrupt);

    // Huge garbage is rejected on size alone.
    const std::string huge(std::size_t{64} * 1024, '{');
    EXPECT_EQ(parse(huge).error().code, Errc::kCorrupt);
}

TEST(OpenMeteoParse, DeepNestingDoesNotCrash) {
    const std::string deep(kMaxBodyBytes, '[');
    const auto r = parse(deep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Errc::kCorrupt);
}

TEST(OpenMeteoParse, UnknownWmoCodeStillValid) {
    const auto r = parse(R"({"current":{"temperature_2m":1.0,"weather_code":42}})");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->condition, WeatherCondition::kUnknown);
    EXPECT_EQ(r->valid, 1);
}

TEST(OpenMeteoParse, ExtraFieldsAndLeadingWhitespaceIgnored) {
    const auto r = parse(
        R"(  {"x":[1,2,{"y":null}],"current":{"a":1,"temperature_2m":0.0,"weather_code":95}} )");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->condition, WeatherCondition::kThunder);
}

TEST(OpenMeteoParse, ParsesThroughProviderInterface) {
    const OpenMeteoProvider impl;
    const Provider& p = impl;
    EXPECT_EQ(p.name(), "open-meteo");
    EXPECT_TRUE(p.parse(kNormal, kNow).has_value());
}

// ---------------------------------------------------------------- WMO map
TEST(WmoMap, EveryCode) {
    using C = WeatherCondition;
    auto expect_for = [](int code) -> C {
        switch (code) {
            case 0:
            case 1:
                return C::kClear;
            case 2:
                return C::kPartlyCloudy;
            case 3:
                return C::kCloudy;
            case 45:
            case 48:
                return C::kFog;
            case 51:
            case 53:
            case 55:
            case 56:
            case 57:
                return C::kDrizzle;
            case 61:
            case 63:
            case 65:
            case 66:
            case 67:
                return C::kRain;
            case 71:
            case 73:
            case 75:
            case 77:
            case 85:
            case 86:
                return C::kSnow;
            case 80:
            case 81:
            case 82:
                return C::kShowers;
            case 95:
            case 96:
            case 99:
                return C::kThunder;
            default:
                return C::kUnknown;
        }
    };
    for (int code = -5; code <= 120; ++code) {
        EXPECT_EQ(condition_from_wmo(code), expect_for(code)) << code;
    }
    EXPECT_EQ(condition_from_wmo(-1), C::kUnknown);
    EXPECT_EQ(condition_from_wmo(1000), C::kUnknown);
    EXPECT_EQ(condition_from_wmo(INT32_MAX), C::kUnknown);
    EXPECT_EQ(condition_from_wmo(INT32_MIN), C::kUnknown);
}

TEST(WmoMap, DocumentedCodesAreNeverUnknown) {
    constexpr std::array<int, 28> kDocumented = {0,  1,  2,  3,  45, 48, 51, 53, 55, 56,
                                                 57, 61, 63, 65, 66, 67, 71, 73, 75, 77,
                                                 80, 81, 82, 85, 86, 95, 96, 99};
    for (const int code : kDocumented) {
        EXPECT_NE(condition_from_wmo(code), WeatherCondition::kUnknown) << code;
    }
}

// ---------------------------------------------------------------- freshness
WeatherReport report_at(time::UnixSeconds fetched) {
    WeatherReport r;
    r.fetched_utc = fetched;
    r.valid = 1;
    return r;
}

TEST(Freshness, BoundariesAtDefaultInterval) {
    constexpr std::uint16_t kInterval = 60;
    constexpr time::UnixSeconds kFresh = 2 * kHour;
    constexpr time::UnixSeconds kHide = 6 * kHour;
    const WeatherReport r = report_at(kNow);
    EXPECT_EQ(freshness(r, kNow, kInterval, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + kFresh - 1, kInterval, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + kFresh, kInterval, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + kFresh + 1, kInterval, true), WeatherFreshness::kStale);
    EXPECT_EQ(freshness(r, kNow + kHide - 1, kInterval, true), WeatherFreshness::kStale);
    EXPECT_EQ(freshness(r, kNow + kHide, kInterval, true), WeatherFreshness::kStale);
    EXPECT_EQ(freshness(r, kNow + kHide + 1, kInterval, true), WeatherFreshness::kHidden);
}

TEST(Freshness, ScalesWithInterval) {
    const WeatherReport r = report_at(kNow);
    EXPECT_EQ(freshness(r, kNow + kHour, 30, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + kHour + 1, 30, true), WeatherFreshness::kStale);
    EXPECT_EQ(freshness(r, kNow + (6 * kHour), 180, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + (6 * kHour) + 1, 180, true), WeatherFreshness::kHidden);
}

TEST(Freshness, HideLimitDominatesLongIntervals) {
    // interval 360 min: 2 x interval = 12 h, but section 12 hides beyond 6 h.
    const WeatherReport r = report_at(kNow);
    EXPECT_EQ(freshness(r, kNow + (6 * kHour), 360, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + (6 * kHour) + 1, 360, true), WeatherFreshness::kHidden);
}

TEST(Freshness, ZeroIntervalIsFreshOnlyAtAgeZero) {
    const WeatherReport r = report_at(kNow);
    EXPECT_EQ(freshness(r, kNow, 0, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow + 1, 0, true), WeatherFreshness::kStale);
}

TEST(Freshness, HiddenWhenTimeInvalidOrNeverFetched) {
    const WeatherReport r = report_at(kNow);
    EXPECT_EQ(freshness(r, kNow, 60, false), WeatherFreshness::kHidden);
    EXPECT_EQ(freshness(WeatherReport{}, kNow, 60, true), WeatherFreshness::kHidden);
    WeatherReport invalid = report_at(kNow);
    invalid.valid = 0;
    EXPECT_EQ(freshness(invalid, kNow, 60, true), WeatherFreshness::kHidden);
}

TEST(Freshness, ClockSkewTolerance) {
    const WeatherReport r = report_at(kNow);
    EXPECT_EQ(freshness(r, kNow - 1, 60, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow - 300, 60, true), WeatherFreshness::kFresh);
    EXPECT_EQ(freshness(r, kNow - 301, 60, true), WeatherFreshness::kHidden);
    EXPECT_EQ(freshness(r, kNow - (240 * kHour), 60, true), WeatherFreshness::kHidden);
}

TEST(Freshness, ExtremeTimesDoNotOverflow) {
    const WeatherReport r = report_at(INT64_MIN / 4);
    EXPECT_EQ(freshness(r, INT64_MAX / 4, 360, true), WeatherFreshness::kHidden);
}

// ---------------------------------------------------------------- display units
TEST(DisplayDegrees, CelsiusRoundsHalfAwayFromZero) {
    EXPECT_EQ(display_degrees(0, TempUnit::kCelsius), 0);
    EXPECT_EQ(display_degrees(4, TempUnit::kCelsius), 0);
    EXPECT_EQ(display_degrees(5, TempUnit::kCelsius), 1);
    EXPECT_EQ(display_degrees(14, TempUnit::kCelsius), 1);
    EXPECT_EQ(display_degrees(15, TempUnit::kCelsius), 2);
    EXPECT_EQ(display_degrees(-4, TempUnit::kCelsius), 0);
    EXPECT_EQ(display_degrees(-5, TempUnit::kCelsius), -1);
    EXPECT_EQ(display_degrees(-15, TempUnit::kCelsius), -2);
    EXPECT_EQ(display_degrees(-186, TempUnit::kCelsius), -19);
    EXPECT_EQ(display_degrees(1000, TempUnit::kCelsius), 100);
}

TEST(DisplayDegrees, FahrenheitKnownPoints) {
    EXPECT_EQ(display_degrees(0, TempUnit::kFahrenheit), 32);
    EXPECT_EQ(display_degrees(1000, TempUnit::kFahrenheit), 212);
    EXPECT_EQ(display_degrees(-400, TempUnit::kFahrenheit), -40);
    EXPECT_EQ(display_degrees(370, TempUnit::kFahrenheit), 99); // 98.6
    EXPECT_EQ(display_degrees(200, TempUnit::kFahrenheit), 68);
    EXPECT_EQ(display_degrees(-100, TempUnit::kFahrenheit), 14);
    EXPECT_EQ(display_degrees(-178, TempUnit::kFahrenheit), 0); // 0.04 F
}

TEST(DisplayDegrees, FahrenheitRoundingBoundaries) {
    // F = dc*0.18 + 32. dc = 25 -> 36.5 exactly -> 37 ; dc = 24 -> 36.32 -> 36.
    EXPECT_EQ(display_degrees(25, TempUnit::kFahrenheit), 37);
    EXPECT_EQ(display_degrees(24, TempUnit::kFahrenheit), 36);
    // Negative halves round away from zero: F = -0.5 needs dc*0.18 = -32.5 (no integer dc);
    // exact -x.5 cases at dc = -25 - 200k: dc=-225 -> -8.5 F -> -9 ; dc=-224 -> -8.32 -> -8.
    EXPECT_EQ(display_degrees(-225, TempUnit::kFahrenheit), -9);
    EXPECT_EQ(display_degrees(-224, TempUnit::kFahrenheit), -8);
    // Positive/negative mirror around the .5 points.
    EXPECT_EQ(display_degrees(-175, TempUnit::kFahrenheit), 1); // 0.5 -> 1
    EXPECT_EQ(display_degrees(-176, TempUnit::kFahrenheit), 0); // 0.32 -> 0
    EXPECT_EQ(display_degrees(-180, TempUnit::kFahrenheit), 0); // -0.4 -> 0 (never "-0")
}

TEST(DisplayDegrees, FahrenheitNegativeZeroBand) {
    // Results in (-0.5, 0) F round to integer 0; the type cannot carry -0.
    for (int dc = -180; dc <= -178; ++dc) {
        EXPECT_EQ(display_degrees(static_cast<std::int16_t>(dc), TempUnit::kFahrenheit), 0) << dc;
    }
    EXPECT_EQ(display_degrees(-181, TempUnit::kFahrenheit), -1); // -0.58 F
}

void expect_f_monotonic_over_int16() {
    std::int16_t prev = display_degrees(-32768, TempUnit::kFahrenheit);
    for (int dc = -32767; dc <= 32767; ++dc) {
        const std::int16_t cur =
            display_degrees(static_cast<std::int16_t>(dc), TempUnit::kFahrenheit);
        ASSERT_GE(cur, prev) << dc;
        ASSERT_LE(cur - prev, 1) << dc;
        prev = cur;
    }
}

TEST(DisplayDegrees, MonotonicAndExactAcrossInt16Range) {
    expect_f_monotonic_over_int16();
    EXPECT_EQ(display_degrees(32767, TempUnit::kFahrenheit), 5930);
    EXPECT_EQ(display_degrees(-32768, TempUnit::kFahrenheit), -5866);
    EXPECT_EQ(display_degrees(-32768, TempUnit::kCelsius), -3277);
    EXPECT_EQ(display_degrees(32767, TempUnit::kCelsius), 3277);
}

} // namespace
} // namespace qz::weather
