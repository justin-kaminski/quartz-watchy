// Suite "time": time zone and civil-date spot checks (the exhaustive oracle suite lives in the
// qz_time unit tests; this is the on-device sanity check of the built-in table).
#include "detail.hpp"
#include "qz/time/civil.hpp"
#include "qz/time/tz.hpp"
#include "tests.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace qz::selftest::tests {

namespace {

time::UnixSeconds
utc_of(std::int32_t y, std::uint8_t mo, std::uint8_t d, std::int32_t hour) noexcept {
    return (time::days_from_civil({y, mo, d}) * time::kSecondsPerDay) +
           (static_cast<std::int64_t>(hour) * 3600);
}

struct ZoneCheck {
    std::string_view name;
    std::int32_t summer_offset_s; ///< 2026-07-01 12:00 UTC
    std::int32_t winter_offset_s; ///< 2026-01-15 12:00 UTC
};

constexpr std::array<ZoneCheck, 4> kZoneChecks{{
    {"UTC", 0, 0},
    {"America/Chicago", -5 * 3600, -6 * 3600},
    {"Europe/Berlin", 2 * 3600, 3600},
    {"Asia/Kolkata", 19800, 19800},
}};

Result<time::TimeZone> zone_named(std::string_view name) noexcept {
    const time::TzEntry* e = time::find_zone(name);
    if (e == nullptr) {
        return Errc::kNotFound;
    }
    return time::TimeZone::parse(e->posix);
}

} // namespace

Outcome tz_zones(Context& /*ctx*/, Detail& d) noexcept {
    const time::UnixSeconds summer = utc_of(2026, 7, 1, 12);
    const time::UnixSeconds winter = utc_of(2026, 1, 15, 12);
    for (const ZoneCheck& z : kZoneChecks) {
        const Result<time::TimeZone> tz = zone_named(z.name);
        if (!tz) {
            return fail_error(d, z.name, tz.error());
        }
        if (tz->utc_offset_at(summer) != z.summer_offset_s ||
            tz->utc_offset_at(winter) != z.winter_offset_s) {
            return fail(d, Text().put(z.name).put(" offset wrong").view());
        }
        const time::LocalDateTime local = tz->to_local(summer);
        const std::int64_t expect_hour = ((12 * 3600) + z.summer_offset_s) / 3600;
        if (local.date.day != 1 || local.time.hour != (((expect_hour % 24) + 24) % 24)) {
            return fail(d, Text().put(z.name).put(" local time wrong").view());
        }
    }
    return pass(d, "UTC Chicago Berlin Kolkata offsets");
}

Outcome tz_transitions(Context& /*ctx*/, Detail& d) noexcept {
    const Result<time::TimeZone> tz = zone_named("America/Chicago");
    if (!tz) {
        return fail_error(d, "zone", tz.error());
    }
    // Spring forward 2026-03-08 02:00 CST -> 03:00 CDT: 02:30 does not exist.
    const time::CivilDate gap_date{2026, 3, 8};
    const time::CivilTime gap_time{2, 30, 0};
    if (tz->to_utc(gap_date, gap_time, time::GapPolicy::kReject)) {
        return fail(d, "gap time accepted by kReject");
    }
    const Result<time::UnixSeconds> shifted =
        tz->to_utc(gap_date, gap_time, time::GapPolicy::kEarlier);
    if (!shifted) {
        return fail_error(d, "gap kEarlier", shifted.error());
    }
    const time::LocalDateTime after = tz->to_local(*shifted);
    if (after.time.hour != 3 || after.time.minute != 30 || !after.is_dst) {
        return fail(d, "gap not shifted to 03:30 CDT");
    }
    // Fall back 2026-11-01 02:00 CDT -> 01:00 CST: 01:30 happens twice, one hour apart.
    const time::CivilDate fall_date{2026, 11, 1};
    const time::CivilTime fall_time{1, 30, 0};
    const Result<time::UnixSeconds> first =
        tz->to_utc(fall_date, fall_time, time::GapPolicy::kEarlier);
    const Result<time::UnixSeconds> second =
        tz->to_utc(fall_date, fall_time, time::GapPolicy::kLater);
    if (!first || !second || *second - *first != 3600) {
        return fail(d, "overlap occurrences not 1 h apart");
    }
    const std::optional<time::UnixSeconds> next = tz->next_transition(utc_of(2026, 7, 1, 0));
    if (!next || *next != utc_of(2026, 11, 1, 7)) {
        return fail(d, "next transition after July is not the November change");
    }
    return pass(d, "gap and overlap resolved");
}

Outcome civil_math(Context& /*ctx*/, Detail& d) noexcept {
    if (time::days_from_civil({1970, 1, 1}) != 0) {
        return fail(d, "epoch day is not 0");
    }
    if (time::weekday_from_days(time::days_from_civil({2026, 10, 6})) != time::Weekday::kTuesday) {
        return fail(d, "2026-10-06 is not a Tuesday");
    }
    if (!time::is_leap_year(2000) || !time::is_leap_year(2024) || time::is_leap_year(2100) ||
        time::is_leap_year(2026)) {
        return fail(d, "leap year rule");
    }
    if (time::days_in_month(2024, 2) != 29 || time::days_in_month(2026, 2) != 28 ||
        time::days_in_month(2026, 12) != 31) {
        return fail(d, "days in month");
    }
    for (const std::int32_t year : {1970, 1999, 2000, 2024, 2100, 2199}) {
        for (const int month : {1, 2, 3, 12}) {
            const time::CivilDate date{year, static_cast<std::uint8_t>(month), 1};
            const time::CivilDate back = time::civil_from_days(time::days_from_civil(date));
            if (back.year != date.year || back.month != date.month || back.day != date.day) {
                return fail(d, "civil <-> day number round trip");
            }
        }
    }
    return pass(d, "civil date arithmetic");
}

Outcome tz_table_parse(Context& /*ctx*/, Detail& d) noexcept {
    std::int64_t count = 0;
    for (const time::TzEntry& e : time::builtin_zones()) {
        const Result<time::TimeZone> tz = time::TimeZone::parse(e.posix);
        if (!tz) {
            return fail(d, Text().put("unparsable: ").put(e.name).view());
        }
        ++count;
    }
    if (count == 0) {
        return fail(d, "empty zone table");
    }
    return pass(d, Text().num(count).put(" zones parse").view());
}

} // namespace qz::selftest::tests
