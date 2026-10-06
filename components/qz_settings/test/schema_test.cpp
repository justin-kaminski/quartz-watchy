// Schema, parsing, formatting and validation (qz/settings/settings.hpp).
#include "qz/settings/settings.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace qz::settings {
namespace {

using test::code_of;
using test::fmt;

constexpr std::size_t kNvsKeyMax = 15;

/// Canonical (format_value) spellings that must be accepted for each key.
std::vector<std::string_view> valid_samples(Key key) {
    switch (key) {
        case Key::kHourFormat:
            return {"24h", "12h"};
        case Key::kTimeZone:
            return {"UTC", "America/Chicago", "Europe/Berlin", "Asia/Kolkata"};
        case Key::kTempUnit:
            return {"c", "f"};
        case Key::kConnectivity:
            return {"off", "time", "time+weather"};
        case Key::kWeatherHighLow:
        case Key::kVibration:
        case Key::kTapWake:
            return {"on", "off"};
        case Key::kLatitude:
            return {"0.00000", "41.88113", "-41.88113", "90.00000", "-90.00000"};
        case Key::kLongitude:
            return {"0.00000", "-87.62980", "180.00000", "-180.00000", "0.00001", "-0.00001"};
        case Key::kSyncIntervalH:
            return {"6", "12", "24", "48", "168"};
        case Key::kWeatherIntervalMin:
            return {"30", "60", "120", "180", "360"};
        case Key::kStepGoal:
            return {"0", "500", "10000", "50000"};
        case Key::kFace:
            return {"0", "1", "255"};
        case Key::kCount:
            break;
    }
    return {};
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Schema, NamesAreUniqueShortAndOrdered) {
    std::set<std::string_view> seen;
    const auto keys = schema();
    ASSERT_EQ(keys.size(), static_cast<std::size_t>(Key::kCount));
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const KeyInfo& ki = keys[i];
        EXPECT_FALSE(ki.name.empty());
        EXPECT_LE(ki.name.size(), kNvsKeyMax) << ki.name;
        EXPECT_TRUE(seen.insert(ki.name).second) << "duplicate " << ki.name;
        EXPECT_EQ(static_cast<std::size_t>(ki.key), i);
        EXPECT_FALSE(ki.help.empty()) << ki.name;
        EXPECT_EQ(&info(ki.key), &ki);
        EXPECT_EQ(find_key(ki.name), &ki);
        EXPECT_LE(ki.min, ki.max);
    }
    // Reserved NVS key names must not collide with schema names.
    EXPECT_FALSE(seen.contains("ver"));
    EXPECT_FALSE(seen.contains("tzposix"));
    EXPECT_EQ(find_key("nope"), nullptr);
    EXPECT_EQ(find_key(""), nullptr);
    EXPECT_EQ(find_key("TFMT"), nullptr); // key names are exact; values are case-insensitive
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Schema, EnumAndChoiceListsAreWellFormed) {
    for (const KeyInfo& ki : schema()) {
        if (ki.type == ValueType::kEnum || ki.type == ValueType::kBool) {
            EXPECT_EQ(static_cast<std::int64_t>(ki.choices.size()), ki.max + 1) << ki.name;
            EXPECT_EQ(ki.min, 0);
        }
        const std::set<std::string_view> unique(ki.choices.begin(), ki.choices.end());
        EXPECT_EQ(unique.size(), ki.choices.size()) << ki.name;
    }
}

TEST(Defaults, MatchFirstBootDecisionQ03) {
    const Settings d = defaults();
    EXPECT_EQ(d.connectivity, model::ConnectivityMode::kOff);
    EXPECT_EQ(d.hour_format, model::HourFormat::k24h);
    EXPECT_EQ(d.temp_unit, model::TempUnit::kCelsius);
    EXPECT_EQ(d.face_id, 0);
    EXPECT_TRUE(d.vibration);
    EXPECT_EQ(d.step_goal, 0U);
    EXPECT_EQ(d.tz_name.view(), "UTC");
    EXPECT_FALSE(d.location_set);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Defaults, EveryKeyDefaultIsValidAndRoundTrips) {
    const Settings d = defaults();
    EXPECT_TRUE(validate(d, nullptr));
    EXPECT_TRUE(validate(d, test::face_below_four));
    for (const KeyInfo& ki : schema()) {
        const std::string text = fmt(d, ki.key);
        ASSERT_FALSE(text.empty()) << ki.name;
        Settings other = d;
        // Move every key away from its default first so the round trip is not a no-op.
        if (ki.key == Key::kHourFormat) {
            ASSERT_TRUE(set_from_string(other, ki.key, "12h"));
        }
        if (ki.key == Key::kStepGoal) {
            ASSERT_TRUE(set_from_string(other, ki.key, "500"));
        }
        ASSERT_TRUE(set_from_string(other, ki.key, text)) << ki.name << "=" << text;
        EXPECT_EQ(fmt(other, ki.key), text) << ki.name;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Parse, EverySampleRoundTripsThroughFormat) {
    for (const KeyInfo& ki : schema()) {
        const auto samples = valid_samples(ki.key);
        ASSERT_FALSE(samples.empty()) << ki.name;
        for (const std::string_view sample : samples) {
            Settings s = defaults();
            ASSERT_TRUE(set_from_string(s, ki.key, sample)) << ki.name << "=" << sample;
            EXPECT_EQ(fmt(s, ki.key), sample) << ki.name;
            EXPECT_TRUE(validate(s, nullptr)) << ki.name << "=" << sample;
            Settings again = defaults();
            ASSERT_TRUE(set_from_string(again, ki.key, fmt(s, ki.key)));
            EXPECT_TRUE(again == s) << ki.name << "=" << sample;
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Parse, ErrorLeavesSettingsUntouched) {
    Settings s = defaults();
    ASSERT_TRUE(set_from_string(s, Key::kStepGoal, "5000"));
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "10.5"));
    const Settings before = s;
    constexpr std::array<std::pair<Key, std::string_view>, 12> kBad{{
        {Key::kStepGoal, "5001"},
        {Key::kHourFormat, "13h"},
        {Key::kTimeZone, "Mars/Olympus"},
        {Key::kLatitude, "91"},
        {Key::kLongitude, "x"},
        {Key::kSyncIntervalH, "7"},
        {Key::kVibration, "maybe"},
        {Key::kFace, "256"},
        {Key::kWeatherIntervalMin, ""},
        {Key::kConnectivity, "wifi"},
        {Key::kTempUnit, "k"},
        {Key::kCount, "1"},
    }};
    for (const auto& [key, value] : kBad) {
        EXPECT_EQ(code_of(set_from_string(s, key, value)), Errc::kBadArgs) << value;
        EXPECT_TRUE(s == before) << value;
    }
}

TEST(Parse, EnumTokensAreCaseInsensitiveAndExact) {
    Settings s = defaults();
    ASSERT_TRUE(set_from_string(s, Key::kHourFormat, "12H"));
    EXPECT_EQ(s.hour_format, model::HourFormat::k12h);
    ASSERT_TRUE(set_from_string(s, Key::kConnectivity, "Time+Weather"));
    EXPECT_EQ(s.connectivity, model::ConnectivityMode::kTimeWeather);
    ASSERT_TRUE(set_from_string(s, Key::kTempUnit, "F"));
    EXPECT_EQ(s.temp_unit, model::TempUnit::kFahrenheit);
    for (const std::string_view bad : {"", " 12h", "12h ", "time+", "weather", "12"}) {
        EXPECT_FALSE(set_from_string(s, Key::kHourFormat, bad)) << bad;
        EXPECT_FALSE(set_from_string(s, Key::kConnectivity, bad)) << bad;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Parse, BoolSpellings) {
    for (const std::string_view yes : {"on", "ON", "1", "true", "Yes"}) {
        Settings s = defaults();
        s.vibration = false;
        ASSERT_TRUE(set_from_string(s, Key::kVibration, yes)) << yes;
        EXPECT_TRUE(s.vibration);
    }
    for (const std::string_view no : {"off", "Off", "0", "false", "NO"}) {
        Settings s = defaults();
        ASSERT_TRUE(set_from_string(s, Key::kTapWake, "on"));
        ASSERT_TRUE(set_from_string(s, Key::kTapWake, no)) << no;
        EXPECT_FALSE(s.tap_wake);
    }
    Settings s = defaults();
    for (const std::string_view bad : {"", "2", "-1", "onn", "t"}) {
        EXPECT_FALSE(set_from_string(s, Key::kWeatherHighLow, bad)) << bad;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Parse, UIntBoundsAndChoices) {
    Settings s = defaults();
    // goal: 0..50000 in steps of 500
    for (const std::string_view ok_value : {"0", "500", "00500", "50000"}) {
        EXPECT_TRUE(set_from_string(s, Key::kStepGoal, ok_value)) << ok_value;
    }
    for (const std::string_view bad : {"50500",
                                       "250",
                                       "1",
                                       "-500",
                                       "+500",
                                       " 500",
                                       "500 ",
                                       "5e2",
                                       "0x1F4",
                                       "4294967296",
                                       "99999999999",
                                       ""}) {
        EXPECT_FALSE(set_from_string(s, Key::kStepGoal, bad)) << bad;
    }
    // sync_h / wx_min accept exactly their listed values, nothing between or outside
    for (std::int64_t v = 0; v <= 400; ++v) {
        const std::string text = std::to_string(v);
        const bool sync_ok = v == 6 || v == 12 || v == 24 || v == 48 || v == 168;
        const bool wx_ok = v == 30 || v == 60 || v == 120 || v == 180 || v == 360;
        EXPECT_EQ(static_cast<bool>(set_from_string(s, Key::kSyncIntervalH, text)), sync_ok) << v;
        EXPECT_EQ(static_cast<bool>(set_from_string(s, Key::kWeatherIntervalMin, text)), wx_ok)
            << v;
    }
    // face: 0..255 (registry membership is validate()'s face_ok job)
    EXPECT_TRUE(set_from_string(s, Key::kFace, "255"));
    EXPECT_EQ(s.face_id, 255);
    EXPECT_FALSE(set_from_string(s, Key::kFace, "256"));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Parse, DegreesFormatAndRange) {
    Settings s = defaults();
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "41.88113"));
    EXPECT_EQ(s.location.lat_e5, 4'188'113);
    EXPECT_TRUE(s.location_set);
    ASSERT_TRUE(set_from_string(s, Key::kLongitude, "-87.6298"));
    EXPECT_EQ(s.location.lon_e5, -8'762'980);
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "+5"));
    EXPECT_EQ(s.location.lat_e5, 500'000);
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "0.5"));
    EXPECT_EQ(s.location.lat_e5, 50'000);
    // more than 5 fraction digits: rounded half away from zero on the 6th digit
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "41.881134"));
    EXPECT_EQ(s.location.lat_e5, 4'188'113);
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "41.881135"));
    EXPECT_EQ(s.location.lat_e5, 4'188'114);
    ASSERT_TRUE(set_from_string(s, Key::kLongitude, "-87.629798"));
    EXPECT_EQ(s.location.lon_e5, -8'762'980);
    ASSERT_TRUE(set_from_string(s, Key::kLongitude, "-0.000004"));
    EXPECT_EQ(s.location.lon_e5, 0);
    // exact limits
    EXPECT_TRUE(set_from_string(s, Key::kLatitude, "90"));
    EXPECT_TRUE(set_from_string(s, Key::kLatitude, "-90.000000"));
    EXPECT_FALSE(set_from_string(s, Key::kLatitude, "90.00001"));
    EXPECT_FALSE(set_from_string(s, Key::kLatitude, "-90.00001"));
    EXPECT_FALSE(set_from_string(s, Key::kLatitude, "90.000005")); // rounds to 90.00001
    EXPECT_TRUE(set_from_string(s, Key::kLongitude, "180"));
    EXPECT_TRUE(set_from_string(s, Key::kLongitude, "-180"));
    EXPECT_FALSE(set_from_string(s, Key::kLongitude, "180.00001"));
    EXPECT_FALSE(set_from_string(s, Key::kLongitude, "-181"));
    for (const std::string_view bad :
         {"", "-", ".5", "5.", "1,5", "1000", "1.2.3", "1e2", "NaN", " 1", "--1", "0x10"}) {
        EXPECT_FALSE(set_from_string(s, Key::kLongitude, bad)) << bad;
    }
}

TEST(Parse, UnsetClearsLocation) {
    Settings s = defaults();
    EXPECT_EQ(fmt(s, Key::kLatitude), "unset");
    EXPECT_EQ(fmt(s, Key::kLongitude), "unset");
    ASSERT_TRUE(set_from_string(s, Key::kLatitude, "12.5"));
    ASSERT_TRUE(set_from_string(s, Key::kLongitude, "20"));
    ASSERT_TRUE(s.location_set);
    ASSERT_TRUE(set_from_string(s, Key::kLongitude, "UNSET"));
    EXPECT_FALSE(s.location_set);
    EXPECT_TRUE(s.location == model::Location{});
    EXPECT_EQ(fmt(s, Key::kLatitude), "unset");
}

TEST(Parse, ZoneNameSetsNameAndPosixFallback) {
    Settings s = defaults();
    ASSERT_TRUE(set_from_string(s, Key::kTimeZone, "Europe/Berlin"));
    EXPECT_EQ(s.tz_name.view(), "Europe/Berlin");
    EXPECT_EQ(s.tz_posix.view(), "CET-1CEST,M3.5.0,M10.5.0/3");
    EXPECT_FALSE(set_from_string(s, Key::kTimeZone, "europe/berlin")); // IANA names are exact
    EXPECT_FALSE(set_from_string(s, Key::kTimeZone, ""));
    EXPECT_EQ(s.tz_name.view(), "Europe/Berlin");
}

TEST(Format, BufferHandling) {
    Settings s = defaults();
    ASSERT_TRUE(set_from_string(s, Key::kTimeZone, "America/Chicago"));
    std::array<char, 15> exact{};
    EXPECT_EQ(format_value(s, Key::kTimeZone, exact), 15U);
    EXPECT_EQ(std::string_view(exact.data(), 15), "America/Chicago");

    std::array<char, 14> small{};
    small.fill('#');
    EXPECT_EQ(format_value(s, Key::kTimeZone, small), 0U);
    EXPECT_EQ(small[0], '#'); // nothing written when it does not fit

    std::array<char, 8> unused{};
    EXPECT_EQ(format_value(s, Key::kCount, unused), 0U);
    EXPECT_EQ(format_value(s, Key::kStepGoal, std::span<char>{}), 0U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Validate, RejectsOutOfRangeFields) {
    Settings s = defaults();
    s.step_goal = 750;
    EXPECT_EQ(code_of(validate(s, nullptr)), Errc::kBadArgs);
    s = defaults();
    s.step_goal = 50'500;
    EXPECT_FALSE(validate(s, nullptr));
    s = defaults();
    s.sync_interval_h = 7;
    EXPECT_FALSE(validate(s, nullptr));
    s = defaults();
    s.weather_interval_min = 45;
    EXPECT_FALSE(validate(s, nullptr));
    s = defaults();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately invalid
    s.hour_format = static_cast<model::HourFormat>(2);
    EXPECT_FALSE(validate(s, nullptr));
    s = defaults();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately invalid
    s.connectivity = static_cast<model::ConnectivityMode>(3);
    EXPECT_FALSE(validate(s, nullptr));
    s = defaults();
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately invalid
    s.temp_unit = static_cast<model::TempUnit>(2);
    EXPECT_FALSE(validate(s, nullptr));
}

TEST(Validate, LocationOnlyCheckedWhenSet) {
    Settings s = defaults();
    s.location = {9'000'001, 0};
    EXPECT_TRUE(validate(s, nullptr)); // unset: ignored
    s.location_set = true;
    EXPECT_FALSE(validate(s, nullptr));
    s.location = {-9'000'000, -18'000'000};
    EXPECT_TRUE(validate(s, nullptr));
    s.location = {0, 18'000'001};
    EXPECT_FALSE(validate(s, nullptr));
}

TEST(Validate, ZoneMustExistOrHaveParsablePosixFallback) {
    Settings s = defaults();
    s.tz_name.clear();
    EXPECT_FALSE(validate(s, nullptr));

    ASSERT_TRUE(s.tz_name.assign("Atlantis/Lost")); // removed from the list in a later tzdata
    ASSERT_TRUE(s.tz_posix.assign("CST6CDT,M3.2.0,M11.1.0"));
    EXPECT_TRUE(validate(s, nullptr));
    ASSERT_TRUE(s.tz_posix.assign("not a zone!"));
    EXPECT_EQ(code_of(validate(s, nullptr)), Errc::kBadArgs);

    ASSERT_TRUE(s.tz_name.assign("Europe/Berlin")); // listed: the table covers a bad fallback
    EXPECT_TRUE(validate(s, nullptr));
}

TEST(Validate, FaceMustBeRegistered) {
    Settings s = defaults();
    s.face_id = 3;
    EXPECT_TRUE(validate(s, test::face_below_four));
    s.face_id = 4;
    EXPECT_EQ(code_of(validate(s, test::face_below_four)), Errc::kBadArgs);
    EXPECT_TRUE(validate(s, nullptr)); // no registry supplied: range check only
    EXPECT_FALSE(validate(defaults(), test::face_none));
}

} // namespace
} // namespace qz::settings
