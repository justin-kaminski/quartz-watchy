// SettingsStore: load/save/erase_all against FakeKvStore (flash-wear counters, fallbacks).
#include "qz/settings/settings.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace qz::settings {
namespace {

using test::code_of;
using testkit::FakeKvStore;

constexpr std::string_view kNs = "qz_set";

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Settings set_all_non_default() {
    Settings s = defaults();
    EXPECT_TRUE(set_from_string(s, Key::kHourFormat, "12h"));
    EXPECT_TRUE(set_from_string(s, Key::kTimeZone, "America/Chicago"));
    EXPECT_TRUE(set_from_string(s, Key::kTempUnit, "f"));
    EXPECT_TRUE(set_from_string(s, Key::kConnectivity, "time+weather"));
    EXPECT_TRUE(set_from_string(s, Key::kWeatherHighLow, "off"));
    EXPECT_TRUE(set_from_string(s, Key::kLatitude, "41.88113"));
    EXPECT_TRUE(set_from_string(s, Key::kLongitude, "-87.6298"));
    EXPECT_TRUE(set_from_string(s, Key::kSyncIntervalH, "12"));
    EXPECT_TRUE(set_from_string(s, Key::kWeatherIntervalMin, "180"));
    EXPECT_TRUE(set_from_string(s, Key::kStepGoal, "8000"));
    EXPECT_TRUE(set_from_string(s, Key::kVibration, "off"));
    EXPECT_TRUE(set_from_string(s, Key::kFace, "3"));
    EXPECT_TRUE(set_from_string(s, Key::kTapWake, "on"));
    return s;
}

TEST(SettingsStore, FreshInstallLoadsDefaultsWithoutAnyWrite) {
    FakeKvStore kv;
    SettingsStore store(kv);
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(*loaded == defaults());
    EXPECT_EQ(kv.write_count(), 0U);
    EXPECT_EQ(kv.commit_count(), 0U);
}

TEST(SettingsStore, SaveLoadRoundTripsEveryKey) {
    FakeKvStore kv;
    SettingsStore store(kv);
    const Settings all = set_all_non_default();
    ASSERT_TRUE(validate(all, nullptr));
    ASSERT_TRUE(store.save(all, defaults()));
    const auto loaded = SettingsStore(kv).load();
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(*loaded == all);
    // ver + 13 schema keys + tzposix
    EXPECT_EQ(kv.entry_count(kNs), 15U);
    EXPECT_EQ(kv.commit_count(), 1U);
}

TEST(SettingsStore, SaveWritesOnlyChangedKeys) {
    FakeKvStore kv;
    SettingsStore store(kv);
    const Settings base = defaults();

    Settings s1 = base;
    ASSERT_TRUE(set_from_string(s1, Key::kTempUnit, "f"));
    ASSERT_TRUE(store.save(s1, base));
    EXPECT_EQ(kv.write_count(), 2U) << "units + ver stamp on first save";
    EXPECT_EQ(kv.commit_count(), 1U);

    Settings s2 = s1;
    ASSERT_TRUE(set_from_string(s2, Key::kStepGoal, "7500"));
    ASSERT_TRUE(store.save(s2, s1));
    EXPECT_EQ(kv.write_count(), 3U) << "exactly the changed key";
    EXPECT_EQ(kv.commit_count(), 2U);

    // Identical: no write and no commit at all.
    ASSERT_TRUE(store.save(s2, s2));
    EXPECT_EQ(kv.write_count(), 3U);
    EXPECT_EQ(kv.commit_count(), 2U);

    // Zone change writes name and POSIX string (2 keys); the rest is untouched.
    Settings s3 = s2;
    ASSERT_TRUE(set_from_string(s3, Key::kTimeZone, "Europe/Berlin"));
    ASSERT_TRUE(store.save(s3, s2));
    EXPECT_EQ(kv.write_count(), 5U);

    // A change that only touches the POSIX fallback writes just that key.
    Settings s4 = s3;
    ASSERT_TRUE(s4.tz_posix.assign("CET-1CEST,M3.5.0,M10.5.0/2"));
    ASSERT_TRUE(store.save(s4, s3));
    EXPECT_EQ(kv.write_count(), 6U);

    // load() prefers the built-in table's rules for a listed name, so it returns s3's POSIX.
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(*loaded == s3);
}

TEST(SettingsStore, SevenIdleDaysCostNoFlashWrites) {
    FakeKvStore kv;
    SettingsStore store(kv);
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    const Settings cached = *loaded;
    for (int day = 0; day < 7; ++day) {
        for (int wake = 0; wake < 24 * 60; wake += 60) {
            ASSERT_TRUE(store.save(cached, cached));
        }
    }
    EXPECT_EQ(kv.write_count(), 0U);
    EXPECT_EQ(kv.commit_count(), 0U);
}

TEST(SettingsStore, ClearingLocationErasesItsKeys) {
    FakeKvStore kv;
    SettingsStore store(kv);
    Settings located = defaults();
    ASSERT_TRUE(set_from_string(located, Key::kLatitude, "10"));
    ASSERT_TRUE(set_from_string(located, Key::kLongitude, "20"));
    ASSERT_TRUE(store.save(located, defaults()));
    const std::size_t with_location = kv.entry_count(kNs);

    Settings cleared = located;
    ASSERT_TRUE(set_from_string(cleared, Key::kLatitude, "unset"));
    ASSERT_TRUE(store.save(cleared, located));
    EXPECT_EQ(kv.entry_count(kNs), with_location - 2);
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->location_set);

    // Coordinates at exactly 0,0 are a real location, distinct from "unset".
    Settings origin = defaults();
    ASSERT_TRUE(set_from_string(origin, Key::kLatitude, "0"));
    ASSERT_TRUE(set_from_string(origin, Key::kLongitude, "0"));
    ASSERT_TRUE(store.save(origin, cleared));
    const auto again = store.load();
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->location_set);
    EXPECT_TRUE(again->location == model::Location{});
}

TEST(SettingsStore, InvalidSettingsAreRejectedBeforeAnyWrite) {
    FakeKvStore kv;
    SettingsStore store(kv);
    Settings bad = defaults();
    bad.step_goal = 123;
    EXPECT_EQ(code_of(store.save(bad, defaults())), Errc::kBadArgs);
    EXPECT_EQ(kv.write_count(), 0U);
    EXPECT_EQ(kv.commit_count(), 0U);
}

TEST(SettingsStore, WriteFailurePropagatesWithoutCommit) {
    FakeKvStore kv;
    SettingsStore store(kv);
    kv.fail_writes_after(2);
    EXPECT_EQ(code_of(store.save(set_all_non_default(), defaults())), Errc::kIo);
    EXPECT_EQ(kv.commit_count(), 0U);
    kv.fail_writes_after(-1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SettingsStore, InvalidStoredKeysFallBackToTheirDefaults) {
    FakeKvStore kv;
    SettingsStore store(kv);
    ASSERT_TRUE(store.save(set_all_non_default(), defaults()));
    ASSERT_TRUE(kv.set_u32(kNs, "goal", 777));       // not a multiple of 500
    ASSERT_TRUE(kv.set_u32(kNs, "sync_h", 5));       // not an allowed interval
    ASSERT_TRUE(kv.set_u32(kNs, "conn", 9));         // enum out of range
    ASSERT_TRUE(kv.set_str(kNs, "units", "f"));      // wrong type
    ASSERT_TRUE(kv.set_i32(kNs, "lat", 91'000'000)); // out of range => location dropped
    ASSERT_TRUE(kv.set_u32(kNs, "face", 1000));      // above uint8_t

    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    const Settings expected_defaults = defaults();
    EXPECT_EQ(loaded->step_goal, expected_defaults.step_goal);
    EXPECT_EQ(loaded->sync_interval_h, expected_defaults.sync_interval_h);
    EXPECT_EQ(loaded->connectivity, expected_defaults.connectivity);
    EXPECT_EQ(loaded->temp_unit, expected_defaults.temp_unit);
    EXPECT_FALSE(loaded->location_set);
    EXPECT_EQ(loaded->face_id, expected_defaults.face_id);
    // Untouched keys keep their stored values.
    EXPECT_EQ(loaded->hour_format, model::HourFormat::k12h);
    EXPECT_EQ(loaded->weather_interval_min, 180);
    EXPECT_TRUE(loaded->tap_wake);
    EXPECT_TRUE(validate(*loaded, nullptr));
}

TEST(SettingsStore, HalfAStoredLocationIsIgnored) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "ver", kSchemaVersion));
    ASSERT_TRUE(kv.set_i32(kNs, "lat", 1'000'000));
    const auto loaded = SettingsStore(kv).load();
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->location_set);
}

TEST(SettingsStore, ZoneNameKnownToTheTableTakesTheTablePosix) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "ver", kSchemaVersion));
    ASSERT_TRUE(kv.set_str(kNs, "tz", "Europe/Berlin"));
    ASSERT_TRUE(kv.set_str(kNs, "tzposix", "UTC0")); // stale
    const auto loaded = SettingsStore(kv).load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->tz_name.view(), "Europe/Berlin");
    EXPECT_EQ(loaded->tz_posix.view(), "CET-1CEST,M3.5.0,M10.5.0/3");
}

TEST(SettingsStore, RemovedZoneNameKeepsWorkingThroughStoredPosix) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "ver", kSchemaVersion));
    ASSERT_TRUE(kv.set_str(kNs, "tz", "Atlantis/Lost"));
    ASSERT_TRUE(kv.set_str(kNs, "tzposix", "CST6CDT,M3.2.0,M11.1.0"));
    const auto loaded = SettingsStore(kv).load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->tz_name.view(), "Atlantis/Lost");
    EXPECT_EQ(loaded->tz_posix.view(), "CST6CDT,M3.2.0,M11.1.0");
    EXPECT_TRUE(validate(*loaded, nullptr));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SettingsStore, UnusableZoneFallsBackToUtc) {
    for (const std::string_view posix : {"", "garbage zone", "CST6CDT,M13.9.9,M1.1.1"}) {
        FakeKvStore kv;
        ASSERT_TRUE(kv.set_u32(kNs, "ver", kSchemaVersion));
        ASSERT_TRUE(kv.set_str(kNs, "tz", "Atlantis/Lost"));
        ASSERT_TRUE(kv.set_str(kNs, "tzposix", posix));
        const auto loaded = SettingsStore(kv).load();
        ASSERT_TRUE(loaded);
        EXPECT_EQ(loaded->tz_name.view(), "UTC") << posix;
    }
    FakeKvStore no_posix;
    ASSERT_TRUE(no_posix.set_u32(kNs, "ver", kSchemaVersion));
    ASSERT_TRUE(no_posix.set_str(kNs, "tz", "Atlantis/Lost"));
    const auto loaded = SettingsStore(no_posix).load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->tz_name.view(), "UTC");
    // A name longer than the stored field is unusable, not a crash.
    FakeKvStore too_long;
    ASSERT_TRUE(too_long.set_u32(kNs, "ver", kSchemaVersion));
    ASSERT_TRUE(too_long.set_str(kNs, "tz", std::string(60, 'x')));
    const auto loaded_long = SettingsStore(too_long).load();
    ASSERT_TRUE(loaded_long);
    EXPECT_EQ(loaded_long->tz_name.view(), "UTC");
}

TEST(SettingsStore, V0NamespaceWithoutVerIsMigratedAndStamped) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "tfmt", 1));
    ASSERT_TRUE(kv.set_u32(kNs, "goal", 6000));
    const auto loaded = SettingsStore(kv).load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->hour_format, model::HourFormat::k12h);
    EXPECT_EQ(loaded->step_goal, 6000U);
    const auto ver = kv.get_u32(kNs, "ver");
    ASSERT_TRUE(ver);
    EXPECT_EQ(*ver, kSchemaVersion);
    EXPECT_EQ(kv.commit_count(), 1U);
    // Second load is a pure read.
    const std::uint32_t writes = kv.write_count();
    ASSERT_TRUE(SettingsStore(kv).load());
    EXPECT_EQ(kv.write_count(), writes);
}

TEST(SettingsStore, NewerSchemaIsReadBestEffortAndNeverDowngraded) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "ver", kSchemaVersion + 1));
    ASSERT_TRUE(kv.set_u32(kNs, "goal", 9000));
    ASSERT_TRUE(kv.set_u32(kNs, "future_key", 1));
    SettingsStore store(kv);
    const std::uint32_t writes_before = kv.write_count();
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->step_goal, 9000U);
    EXPECT_EQ(kv.write_count(), writes_before) << "load must not write";

    Settings changed = *loaded;
    ASSERT_TRUE(set_from_string(changed, Key::kVibration, "off"));
    ASSERT_TRUE(store.save(changed, *loaded));
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), kSchemaVersion + 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(SettingsStore, EraseAllClearsEveryQuartzNamespaceOnly) {
    FakeKvStore kv;
    SettingsStore store(kv);
    ASSERT_TRUE(store.save(set_all_non_default(), defaults()));
    ASSERT_TRUE(kv.set_str("qz_cred", "ssid", "home"));
    ASSERT_TRUE(kv.set_u32("qz_steps", "ver", 1));
    ASSERT_TRUE(kv.set_i64("qz_time", "drift_t", 5));
    ASSERT_TRUE(kv.set_u32("qz_diag", "crashes", 3));
    ASSERT_TRUE(kv.set_u32("phy", "cal", 1)); // foreign namespace (e.g. Wi-Fi PHY data)
    const std::uint32_t commits = kv.commit_count();

    ASSERT_TRUE(store.erase_all());
    for (const std::string_view ns : {"qz_set", "qz_cred", "qz_steps", "qz_time", "qz_diag"}) {
        EXPECT_EQ(kv.entry_count(ns), 0U) << ns;
    }
    EXPECT_EQ(kv.entry_count("phy"), 1U);
    EXPECT_EQ(kv.commit_count(), commits + 1);
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(*loaded == defaults());
    ASSERT_TRUE(store.erase_all()); // idempotent
}

} // namespace
} // namespace qz::settings
