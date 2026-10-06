// Tests for the generated built-in zone table (WP-03, ARCHITECTURE.md section 9): table shape and
// curation invariants, every entry against the POSIX-TZ engine and against glibc (TZ set to the
// entry's POSIX string, so the check is hermetic and independent of the host's zoneinfo), and
// find_zone/tzdata_version.
#include "qz/time/tz.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <string_view>

namespace qz::time {
namespace {

constexpr std::int64_t kHour = 3'600;
constexpr std::int64_t kHourlySkew = 1'234; ///< keeps minutes and seconds non-trivial
constexpr std::size_t kMinEntries = 120;    ///< ARCHITECTURE section 9: ~120-160 entries
constexpr std::size_t kMaxEntries = 160;
constexpr std::size_t kMaxLabelChars = 24;    ///< fits the 200 px list rows (8 px font)
constexpr std::size_t kTargetEntryBytes = 28; ///< sizeof(TzEntry) on the 32-bit target
constexpr std::size_t kMaxTableBytes = std::size_t{10} * 1024; ///< same ceiling as tools/tzgen.py

std::int64_t start_of_year(std::int32_t year) {
    return std::int64_t{days_from_civil(CivilDate{year, 1, 1})} * kSecondsPerDay;
}

/// "UTC+5:30" style spelling of an offset, as used in labels.
std::string utc_text(std::int32_t offset_s) {
    std::string text = offset_s < 0 ? "UTC-" : "UTC+";
    const std::int32_t abs_min = (offset_s < 0 ? -offset_s : offset_s) / 60;
    text += std::to_string(abs_min / 60);
    if (abs_min % 60 != 0) {
        text += ':';
        text += (abs_min % 60 < 10 ? "0" : "");
        text += std::to_string(abs_min % 60);
    }
    return text;
}

/// Sets TZ for the lifetime of the object and restores the previous value afterwards.
class ScopedTz {
public:
    explicit ScopedTz(const char* posix) {
        const char* previous = std::getenv("TZ"); // NOLINT(concurrency-mt-unsafe)
        had_previous_ = previous != nullptr;
        if (had_previous_) {
            previous_ = previous;
        }
        set(posix);
    }
    ScopedTz(const ScopedTz&) = delete;
    ScopedTz& operator=(const ScopedTz&) = delete;
    ScopedTz(ScopedTz&&) = delete;
    ScopedTz& operator=(ScopedTz&&) = delete;
    ~ScopedTz() {
        if (had_previous_) {
            set(previous_.c_str());
        } else {
            unsetenv("TZ"); // NOLINT(concurrency-mt-unsafe,cert-err33-c)
            tzset();
        }
    }

private:
    static void set(const char* value) {
        if (setenv("TZ", value, 1) != 0) { // NOLINT(concurrency-mt-unsafe)
            ADD_FAILURE() << "setenv(TZ) failed";
        }
        tzset();
    }

    bool had_previous_ = false;
    std::string previous_;
};

TEST(TzDb, NonEmptyAndWithinTargetSize) {
    const auto zones = builtin_zones();
    EXPECT_GE(zones.size(), kMinEntries);
    EXPECT_LE(zones.size(), kMaxEntries);

    // Flash estimate for the 32-bit target: the records plus every distinct string once (the
    // compiler merges identical literals).
    std::set<std::string_view> pool;
    for (const TzEntry& zone : zones) {
        pool.insert(zone.name);
        pool.insert(zone.label);
        pool.insert(zone.posix);
    }
    std::size_t bytes = zones.size() * kTargetEntryBytes;
    for (const std::string_view text : pool) {
        bytes += text.size() + 1;
    }
    EXPECT_LE(bytes, kMaxTableBytes);
}

TEST(TzDb, NamesAndLabelsAreUnique) {
    std::set<std::string_view> names;
    std::set<std::string_view> labels;
    for (const TzEntry& zone : builtin_zones()) {
        EXPECT_TRUE(names.insert(zone.name).second) << "duplicate name " << zone.name;
        EXPECT_TRUE(labels.insert(zone.label).second) << "duplicate label " << zone.label;
    }
}

TEST(TzDb, FieldsAreNonEmptyAndLabelsFitTheList) {
    for (const TzEntry& zone : builtin_zones()) {
        EXPECT_FALSE(zone.name.empty());
        EXPECT_FALSE(zone.posix.empty());
        EXPECT_FALSE(zone.label.empty());
        EXPECT_LE(zone.label.size(), kMaxLabelChars) << zone.label;
    }
}

bool offset_then_label_less(const TzEntry& a, const TzEntry& b) {
    return a.std_offset_s != b.std_offset_s ? a.std_offset_s < b.std_offset_s : a.label < b.label;
}

TEST(TzDb, SortedByStandardOffsetThenLabel) {
    EXPECT_TRUE(std::ranges::is_sorted(builtin_zones(), offset_then_label_less));
}

/// Etc/GMT+N is N hours *west* (POSIX sign convention); there is no Etc/GMT+13 or Etc/GMT-15.
std::string fixed_zone_name(int n) {
    return std::string("Etc/GMT") + (n > 0 ? "+" : "-") + std::to_string(n < 0 ? -n : n);
}

void expect_fixed_offset_zone(int n) {
    const std::string name = fixed_zone_name(n);
    const TzEntry* zone = find_zone(name);
    ASSERT_NE(zone, nullptr) << name;
    EXPECT_EQ(zone->std_offset_s, -n * kHour) << name;
    EXPECT_EQ(zone->label, utc_text(-n * kHour)) << name;
    const Result<TimeZone> parsed = TimeZone::parse(zone->posix);
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_FALSE(parsed->has_dst()) << name;
    EXPECT_EQ(parsed->utc_offset_at(start_of_year(2026)), -n * kHour) << name;
}

TEST(TzDb, ContainsUtcAndAllFixedOffsets) {
    const TzEntry* utc = find_zone("UTC");
    ASSERT_NE(utc, nullptr);
    EXPECT_EQ(utc->posix, "UTC0");
    EXPECT_EQ(utc->std_offset_s, 0);
    for (int n = -14; n <= 12; ++n) {
        if (n != 0) {
            expect_fixed_offset_zone(n);
        }
    }
}

TEST(TzDb, EveryEntryParsesAndLabelCarriesItsStandardOffset) {
    for (const TzEntry& zone : builtin_zones()) {
        const Result<TimeZone> parsed = TimeZone::parse(zone.posix);
        ASSERT_TRUE(parsed.has_value()) << zone.name << " -> " << zone.posix;
        if (zone.name != "UTC") {
            EXPECT_TRUE(zone.label.ends_with(utc_text(zone.std_offset_s)) ||
                        zone.label.ends_with(utc_text(zone.std_offset_s) + ")"))
                << zone.label << " vs std_offset_s " << zone.std_offset_s;
        }
    }
}

TEST(TzDb, StandardOffsetIsTheLowestOffsetOfTheEntry) {
    // std_offset_s is documented as the offset without DST (for Europe/Dublin, whose tzdata
    // "standard" time is the summer one, the winter GMT offset): the minimum over a year.
    for (const TzEntry& zone : builtin_zones()) {
        const Result<TimeZone> parsed = TimeZone::parse(zone.posix);
        ASSERT_TRUE(parsed.has_value()) << zone.name;
        std::int32_t lowest = parsed->utc_offset_at(start_of_year(2027));
        for (std::int64_t t = start_of_year(2027); t < start_of_year(2028); t += 6 * kHour) {
            lowest = std::min(lowest, parsed->utc_offset_at(t));
        }
        EXPECT_EQ(lowest, zone.std_offset_s) << zone.name;
    }
}

constexpr int as_int(std::uint8_t value) {
    return value;
}

/// Empty when the engine agrees with glibc's `ref` for instant t, else a description.
std::string glibc_difference(const TimeZone& zone, std::int64_t t, const std::tm& ref) {
    const LocalDateTime mine = zone.to_local(t);
    const bool same =
        mine.date.year == ref.tm_year + 1900 && as_int(mine.date.month) == ref.tm_mon + 1 &&
        as_int(mine.date.day) == ref.tm_mday && as_int(mine.time.hour) == ref.tm_hour &&
        as_int(mine.time.minute) == ref.tm_min && as_int(mine.time.second) == ref.tm_sec &&
        mine.utc_offset_s == ref.tm_gmtoff && mine.is_dst == (ref.tm_isdst > 0) &&
        zone.abbreviation(mine.is_dst) == ref.tm_zone && zone.utc_offset_at(t) == ref.tm_gmtoff &&
        zone.is_dst_at(t) == (ref.tm_isdst > 0);
    if (same) {
        return {};
    }
    return "t=" + std::to_string(t) + ": engine offset " + std::to_string(mine.utc_offset_s) +
           " dst " + std::to_string(static_cast<int>(mine.is_dst)) + " " +
           std::string(zone.abbreviation(mine.is_dst)) + ", glibc offset " +
           std::to_string(ref.tm_gmtoff) + " dst " + std::to_string(ref.tm_isdst) + " " +
           ref.tm_zone;
}

/// Hourly comparison of one entry against glibc; returns the first difference or "".
std::string first_glibc_difference(const TzEntry& entry, const TimeZone& zone) {
    const std::string posix(entry.posix); // NUL-terminated copy for setenv
    const ScopedTz tz(posix.c_str());
    const std::int64_t end = start_of_year(2031);
    for (std::int64_t t = start_of_year(2026) + kHourlySkew; t < end; t += kHour) {
        const auto stamp = static_cast<std::time_t>(t);
        std::tm ref{};
        if (localtime_r(&stamp, &ref) == nullptr) {
            return "localtime_r failed at t=" + std::to_string(t);
        }
        std::string diff = glibc_difference(zone, t, ref);
        if (!diff.empty()) {
            return diff;
        }
    }
    return {};
}

TEST(TzDb, EveryEntryMatchesGlibcHourlyFrom2026To2030) {
    for (const TzEntry& entry : builtin_zones()) {
        const Result<TimeZone> parsed = TimeZone::parse(entry.posix);
        ASSERT_TRUE(parsed.has_value()) << entry.name;
        const std::string diff = first_glibc_difference(entry, *parsed);
        EXPECT_TRUE(diff.empty()) << entry.name << " (" << entry.posix << ") " << diff;
    }
}

TEST(TzDb, FindZoneRoundTripsEveryEntry) {
    for (const TzEntry& entry : builtin_zones()) {
        const TzEntry* found = find_zone(entry.name);
        ASSERT_NE(found, nullptr) << entry.name;
        EXPECT_EQ(found, &entry);
        // A copy of the name (different storage) must find the same entry.
        const std::string copy(entry.name);
        EXPECT_EQ(find_zone(copy), &entry);
    }
}

TEST(TzDb, FindZoneRejectsUnknownAndInexactNames) {
    EXPECT_EQ(find_zone(""), nullptr);
    EXPECT_EQ(find_zone("utc"), nullptr); // case-sensitive
    EXPECT_EQ(find_zone("Europe/Berli"), nullptr);
    EXPECT_EQ(find_zone("Europe/Berlin "), nullptr);
    EXPECT_EQ(find_zone("Berlin"), nullptr);   // IANA name, not the label
    EXPECT_EQ(find_zone("Etc/GMT0"), nullptr); // intentionally absent: "UTC" is the entry
    EXPECT_EQ(find_zone("Mars/Olympus_Mons"), nullptr);
}

TEST(TzDb, WellKnownZonesHaveExpectedRules) {
    const TzEntry* berlin = find_zone("Europe/Berlin");
    ASSERT_NE(berlin, nullptr);
    EXPECT_EQ(berlin->posix, "CET-1CEST,M3.5.0,M10.5.0/3");
    EXPECT_EQ(berlin->std_offset_s, kHour);
    EXPECT_EQ(berlin->label, "Berlin (UTC+1)");
    const TzEntry* chicago = find_zone("America/Chicago");
    ASSERT_NE(chicago, nullptr);
    EXPECT_EQ(chicago->posix, "CST6CDT,M3.2.0,M11.1.0");
    const TzEntry* kolkata = find_zone("Asia/Kolkata");
    ASSERT_NE(kolkata, nullptr);
    EXPECT_EQ(kolkata->std_offset_s, 19'800);
    EXPECT_EQ(kolkata->label, "Kolkata (UTC+5:30)");
    const TzEntry* dublin = find_zone("Europe/Dublin");
    ASSERT_NE(dublin, nullptr);
    EXPECT_EQ(dublin->std_offset_s, 0); // winter GMT, not the tzdata "standard" IST
}

TEST(TzDb, TzdataVersionLooksLikeAnIanaRelease) {
    const std::string_view version = tzdata_version();
    ASSERT_FALSE(version.empty());
    // "2026e": four-digit year and one release letter.
    ASSERT_EQ(version.size(), 5U);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(version[i] >= '0' && version[i] <= '9') << version;
    }
    EXPECT_TRUE(version[4] >= 'a' && version[4] <= 'z') << version;
}

} // namespace
} // namespace qz::time
