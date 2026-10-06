// glibc oracle for the POSIX-TZ engine (ARCHITECTURE.md section 9, docs/TEST_PLAN.md "O"). Each
// zone string below is evaluated by the engine and by glibc (setenv("TZ") + tzset, localtime_r,
// mktime) and both must agree
//   - on hourly samples across 1970-2100 (every field, offset, DST flag and abbreviation),
//   - on the exact second of every transition (+-1 s), found by bisecting glibc itself,
//   - on gap and overlap resolution of local times, and on ordinary local times via mktime.
// glibc picks the rule year from the UTC year of the instant and so mishandles transitions that
// straddle new year; none of the zones below has one, except permanent DST, whose
// year-boundary behaviour is pinned against the definition in tz_test.cpp instead.
#include "qz/time/tz.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace qz::time {
namespace {

using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;

/// uint8 field as int (implicit promotion, so integer-sign-comparison lints stay quiet).
constexpr int as_int(std::uint8_t value) {
    return value;
}

constexpr std::int64_t kHour = 3'600;
constexpr std::int64_t kHourlySkew = 1'234;   ///< keeps minutes and seconds non-trivial
constexpr std::size_t kDeepCheckStride = 11;  ///< every 11th sample also checks the accessors
constexpr std::int64_t kWallStride = 129'451; ///< ~1.5 days, walks through every time of day
constexpr std::int64_t kNoTransition = std::numeric_limits<std::int64_t>::max();

/// End of the checked range: 2101-01-01T00:00:00Z (a multiple of an hour after the epoch).
std::int64_t range_end() {
    return std::int64_t{days_from_civil(CivilDate{2101, 1, 1})} * kSecondsPerDay;
}

struct OracleCase {
    const char* label; ///< gtest-safe name
    const char* posix;
};

const auto kCases = std::to_array<OracleCase>({
    {"Utc", "UTC0"},
    {"Kathmandu", "<+0545>-5:45"},
    {"Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"NewYork", "EST5EDT,M3.2.0,M11.1.0"},
    {"LosAngeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"Phoenix", "MST7"},
    {"Honolulu", "HST10"},
    {"StJohns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Brisbane", "AEST-10"},
    {"LordHowe", "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Chatham", "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45"},
    {"Nuuk", "<-02>2<-01>,M3.5.0/-1,M10.5.0/0"},
    {"Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Gaza", "EET-2EEST,M3.4.4/50,M10.4.4/50"},
    {"Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"Fiji", "<+12>-12<+13>,M11.1.0,M1.2.2/123"},
    {"Troll", "<+00>0<+02>-2,M3.5.0/1,M10.5.0/3"},
    {"Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"Marquesas", "<-0930>9:30"},
    {"Eucla", "<+0845>-8:45"},
    {"Moscow", "<+03>-3"},
    {"SaoPaulo", "<-03>3"},
    {"Kiritimati", "<+14>-14"},
    {"BakerIsland", "<-12>12"},
    {"JulianDays", "XXX3YYY,J60/2,J300/3"},
    {"ZeroBasedDays", "XXX3YYY,59/2,300/3"},
    {"SouthernJulianDays", "<-03>3<-02>,J274,J59"},
    {"LastWeekOfFebruary", "XXX0YYY,M2.5.0,M11.1.0"},
    {"RuleTimesPlusMinus167h", "XXX5YYY,M3.2.0/-167,M11.1.0/167"},
    {"OffsetsWithSeconds", "<+005328>-0:53:28<+015328>-1:53:28,M3.5.0,M10.5.0"},
    {"SameOffsetDst", "AAA0BBB0,M3.5.0,M10.5.0"},
    {"TwoHourDst", "XXX-1YYY-3,M3.5.0,M10.5.0"},
    {"PermanentDst", "GMT0BST,0/0,J365/25"},
});

// --- glibc access ------------------------------------------------------------------------------

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

bool glibc_local(std::int64_t t, std::tm& out) {
    const auto stamp = static_cast<std::time_t>(t);
    return localtime_r(&stamp, &out) != nullptr;
}

/// UTC-style broken-down time of `wall_seconds` (used to spell a wall clock reading).
std::tm tm_from_wall_seconds(std::int64_t wall_seconds) {
    const auto stamp = static_cast<std::time_t>(wall_seconds);
    std::tm fields{};
    if (gmtime_r(&stamp, &fields) == nullptr) {
        ADD_FAILURE() << "gmtime_r failed for " << wall_seconds;
    }
    return fields;
}

CivilDate date_of(const std::tm& t) {
    return CivilDate{t.tm_year + 1900,
                     static_cast<std::uint8_t>(t.tm_mon + 1),
                     static_cast<std::uint8_t>(t.tm_mday)};
}

CivilTime time_of(const std::tm& t) {
    return CivilTime{static_cast<std::uint8_t>(t.tm_hour),
                     static_cast<std::uint8_t>(t.tm_min),
                     static_cast<std::uint8_t>(t.tm_sec)};
}

bool same_wall(const std::tm& a, const std::tm& b) {
    return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon && a.tm_mday == b.tm_mday &&
           a.tm_hour == b.tm_hour && a.tm_min == b.tm_min && a.tm_sec == b.tm_sec;
}

/// What glibc says about an instant that matters for transitions: DST flag and offset.
struct GlibcState {
    bool dst = false;
    std::int64_t offset_s = 0;
    bool operator==(const GlibcState&) const = default;
};

GlibcState glibc_state(std::int64_t t) {
    std::tm ref{};
    if (!glibc_local(t, ref)) {
        ADD_FAILURE() << "localtime_r failed at t=" << t;
        return {};
    }
    return GlibcState{ref.tm_isdst > 0, ref.tm_gmtoff};
}

/// glibc's instant for a wall time with an explicit DST flag (-1 lets glibc guess).
std::int64_t glibc_mktime(std::tm fields, int isdst) {
    fields.tm_isdst = isdst;
    return static_cast<std::int64_t>(mktime(&fields));
}

/// Every instant glibc maps to this wall time, ascending: none = gap, one = ordinary, two =
/// overlap. Both DST flags are tried and a candidate counts only if it reads back as the wall time.
std::vector<std::int64_t> glibc_instants_of(const std::tm& wall) {
    std::vector<std::int64_t> instants;
    for (const int isdst : {0, 1}) {
        const std::int64_t candidate = glibc_mktime(wall, isdst);
        std::tm back{};
        if (candidate != -1 && glibc_local(candidate, back) && same_wall(back, wall) &&
            std::ranges::find(instants, candidate) == instants.end()) {
            instants.push_back(candidate);
        }
    }
    std::ranges::sort(instants);
    return instants;
}

// --- instant -> local ------------------------------------------------------------------------

std::string show_engine(const LocalDateTime& l, std::string_view abbreviation) {
    return std::to_string(l.date.year) + "-" + std::to_string(l.date.month) + "-" +
           std::to_string(l.date.day) + " " + std::to_string(l.time.hour) + ":" +
           std::to_string(l.time.minute) + ":" + std::to_string(l.time.second) +
           " wday=" + std::to_string(static_cast<int>(l.weekday)) +
           " off=" + std::to_string(l.utc_offset_s) + " dst=" + std::to_string(l.is_dst ? 1 : 0) +
           " " + std::string(abbreviation);
}

std::string show_glibc(const std::tm& g) {
    return std::to_string(g.tm_year + 1900) + "-" + std::to_string(g.tm_mon + 1) + "-" +
           std::to_string(g.tm_mday) + " " + std::to_string(g.tm_hour) + ":" +
           std::to_string(g.tm_min) + ":" + std::to_string(g.tm_sec) +
           " wday=" + std::to_string(g.tm_wday) + " off=" + std::to_string(g.tm_gmtoff) +
           " dst=" + std::to_string(g.tm_isdst > 0 ? 1 : 0) + " " + std::string(g.tm_zone);
}

/// The engine and glibc must read instant t identically. `deep` also exercises the scalar
/// accessors.
AssertionResult compare_instant(const TimeZone& zone, std::int64_t t, bool deep) {
    std::tm ref{};
    if (!glibc_local(t, ref)) {
        return AssertionFailure() << "localtime_r failed at t=" << t;
    }
    const LocalDateTime mine = zone.to_local(t);
    const bool ref_dst = ref.tm_isdst > 0;
    const std::string_view mine_abbreviation = zone.abbreviation(mine.is_dst);
    bool same = mine.date.year == ref.tm_year + 1900 && as_int(mine.date.month) == ref.tm_mon + 1 &&
                as_int(mine.date.day) == ref.tm_mday && as_int(mine.time.hour) == ref.tm_hour &&
                as_int(mine.time.minute) == ref.tm_min && as_int(mine.time.second) == ref.tm_sec &&
                static_cast<int>(mine.weekday) == ref.tm_wday &&
                mine.utc_offset_s == ref.tm_gmtoff && mine.is_dst == ref_dst &&
                mine_abbreviation == ref.tm_zone;
    if (deep) {
        same = same && zone.utc_offset_at(t) == ref.tm_gmtoff && zone.is_dst_at(t) == ref_dst;
    }
    if (same) {
        return AssertionSuccess();
    }
    return AssertionFailure() << "t=" << t << "\n  engine: " << show_engine(mine, mine_abbreviation)
                              << "\n  glibc:  " << show_glibc(ref);
}

// --- transitions -----------------------------------------------------------------------------

/// Smallest instant in (low, high] where glibc is in the state it has at `high`; the state at
/// `low` must differ from the one at `high` and change exactly once in between.
std::int64_t first_instant_of_new_state(std::int64_t low, std::int64_t high) {
    const GlibcState before = glibc_state(low);
    while (high - low > 1) {
        const std::int64_t middle = low + ((high - low) / 2);
        if (glibc_state(middle) == before) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return high;
}

/// Every instant in (0, end] where glibc's DST flag or offset changes, found by hourly scanning
/// and bisection (so independent of the engine's own transition arithmetic).
std::vector<std::int64_t> glibc_flips(std::int64_t end) {
    std::vector<std::int64_t> flips;
    GlibcState previous = glibc_state(0);
    for (std::int64_t t = kHour; t <= end; t += kHour) {
        const GlibcState current = glibc_state(t);
        if (current != previous) {
            flips.push_back(first_instant_of_new_state(t - kHour, t));
            previous = current;
        }
    }
    return flips;
}

/// The engine's transitions in (0, end] by chaining next_transition().
std::vector<std::int64_t> engine_transitions(const TimeZone& zone, std::int64_t end) {
    std::vector<std::int64_t> chain;
    std::int64_t cursor = 0;
    for (;;) {
        const std::int64_t next = zone.next_transition(cursor).value_or(kNoTransition);
        if (next > end || next <= cursor) {
            break;
        }
        chain.push_back(next);
        cursor = next;
    }
    return chain;
}

// --- local -> UTC ------------------------------------------------------------------------------

/// Resolves a wall time under all three policies and compares with the expected instants.
AssertionResult check_resolution(const TimeZone& zone,
                                 std::int64_t wall_seconds,
                                 std::int64_t earlier,
                                 std::int64_t later,
                                 bool reject_fails) {
    const std::tm wall = tm_from_wall_seconds(wall_seconds);
    const CivilDate date = date_of(wall);
    const CivilTime time = time_of(wall);
    const Result<UnixSeconds> got_earlier = zone.to_utc(date, time, GapPolicy::kEarlier);
    const Result<UnixSeconds> got_later = zone.to_utc(date, time, GapPolicy::kLater);
    const Result<UnixSeconds> strict = zone.to_utc(date, time, GapPolicy::kReject);
    if (!got_earlier || !got_later || *got_earlier != earlier || *got_later != later) {
        return AssertionFailure() << "wall " << wall_seconds << ": kEarlier/kLater gave "
                                  << (got_earlier ? *got_earlier : -1) << "/"
                                  << (got_later ? *got_later : -1) << ", expected " << earlier
                                  << "/" << later;
    }
    if (strict.has_value() == reject_fails) {
        return AssertionFailure() << "wall " << wall_seconds << ": kReject "
                                  << (strict.has_value() ? "accepted" : "rejected");
    }
    if (strict && *strict != earlier) {
        return AssertionFailure() << "wall " << wall_seconds << ": kReject gave " << *strict
                                  << ", expected " << earlier;
    }
    return AssertionSuccess();
}

/// Wall times skipped by a forward jump resolve forward by the jump length (both policies).
AssertionResult
check_gap(const TimeZone& zone, std::int64_t flip, const GlibcState& before, std::int64_t length) {
    for (const std::int64_t k : {std::int64_t{0}, std::int64_t{1}, length / 2, length - 1}) {
        const std::int64_t wall_seconds = flip + before.offset_s + k;
        AssertionResult result = check_resolution(zone, wall_seconds, flip + k, flip + k, true);
        if (!result) {
            return result;
        }
        std::tm actual{};
        if (!glibc_local(flip + k, actual) ||
            !same_wall(actual, tm_from_wall_seconds(wall_seconds + length))) {
            return AssertionFailure()
                   << "glibc does not read " << (flip + k) << " as the wall time " << length
                   << " s after " << wall_seconds;
        }
        if (!glibc_instants_of(tm_from_wall_seconds(wall_seconds)).empty()) {
            return AssertionFailure() << "glibc maps the skipped wall time " << wall_seconds;
        }
    }
    // The wall times just outside the gap exist exactly once.
    AssertionResult result =
        check_resolution(zone, flip + before.offset_s - 1, flip - 1, flip - 1, false);
    if (!result) {
        return result;
    }
    return check_resolution(zone, flip + before.offset_s + length, flip, flip, false);
}

/// Wall times repeated by a backward jump: kEarlier = first pass, kLater = second pass, and
/// glibc agrees through mktime with an explicit DST flag.
AssertionResult check_overlap(const TimeZone& zone,
                              std::int64_t flip,
                              const GlibcState& before,
                              const GlibcState& after,
                              std::int64_t length) {
    for (const std::int64_t k : {std::int64_t{0}, std::int64_t{1}, length / 2, length - 1}) {
        const std::int64_t wall_seconds = flip + after.offset_s + k;
        const std::int64_t first = flip - length + k;
        const std::int64_t second = flip + k;
        AssertionResult result = check_resolution(zone, wall_seconds, first, second, true);
        if (!result) {
            return result;
        }
        const std::tm wall = tm_from_wall_seconds(wall_seconds);
        const std::vector<std::int64_t> truth = glibc_instants_of(wall);
        if (truth != std::vector<std::int64_t>{first, second}) {
            return AssertionFailure()
                   << "glibc maps wall " << wall_seconds << " to " << truth.size()
                   << " instants, expected {" << first << ", " << second << "}";
        }
        if (before.dst != after.dst && (glibc_mktime(wall, before.dst ? 1 : 0) != first ||
                                        glibc_mktime(wall, after.dst ? 1 : 0) != second)) {
            return AssertionFailure()
                   << "mktime with an explicit DST flag disagrees at wall " << wall_seconds;
        }
    }
    // The wall times just outside the overlap exist exactly once.
    AssertionResult result = check_resolution(
        zone, flip + after.offset_s - 1, flip - length - 1, flip - length - 1, false);
    if (!result) {
        return result;
    }
    return check_resolution(zone, flip + before.offset_s, flip + length, flip + length, false);
}

AssertionResult check_flip_resolution(const TimeZone& zone, std::int64_t flip) {
    const GlibcState before = glibc_state(flip - 1);
    const GlibcState after = glibc_state(flip);
    const std::int64_t jump = after.offset_s - before.offset_s;
    if (jump > 0) {
        return check_gap(zone, flip, before, jump);
    }
    if (jump < 0) {
        return check_overlap(zone, flip, before, after, -jump);
    }
    return AssertionSuccess(); // the DST flag changed but the offset did not
}

/// Ordinary wall times: the engine resolves them like mktime(); ambiguous ones like glibc's two
/// candidates (earlier/later) and are refused by kReject.
AssertionResult compare_wall(const TimeZone& zone, std::int64_t wall_seconds) {
    const std::tm wall = tm_from_wall_seconds(wall_seconds);
    const std::vector<std::int64_t> truth = glibc_instants_of(wall);
    const CivilDate date = date_of(wall);
    const CivilTime time = time_of(wall);
    const Result<UnixSeconds> earlier = zone.to_utc(date, time, GapPolicy::kEarlier);
    const Result<UnixSeconds> later = zone.to_utc(date, time, GapPolicy::kLater);
    const Result<UnixSeconds> strict = zone.to_utc(date, time, GapPolicy::kReject);
    if (!earlier || !later) {
        return AssertionFailure() << "wall " << wall_seconds << ": a valid time did not resolve";
    }
    if (truth.size() == 1) {
        if (!strict || *strict != truth.front() || *earlier != truth.front() ||
            *later != truth.front() || glibc_mktime(wall, -1) != truth.front()) {
            return AssertionFailure()
                   << "wall " << wall_seconds << ": ordinary time resolved to " << *earlier << "/"
                   << *later << ", glibc says " << truth.front();
        }
        return AssertionSuccess();
    }
    if (strict.has_value() ||
        (truth.size() == 2 && (*earlier != truth.front() || *later != truth.back())) ||
        (truth.empty() && *earlier != *later)) {
        return AssertionFailure() << "wall " << wall_seconds << ": " << truth.size()
                                  << " glibc instants, engine " << *earlier << "/" << *later
                                  << (strict.has_value() ? " (kReject accepted it)" : "");
    }
    return AssertionSuccess();
}

// --- the parameterised oracle ------------------------------------------------------------------

class TzOracle : public ::testing::TestWithParam<OracleCase> {};

TEST_P(TzOracle, HourlySamplesAcross1970To2100MatchGlibc) {
    const OracleCase& zone_case = GetParam();
    const ScopedTz env(zone_case.posix);
    const Result<TimeZone> parsed = TimeZone::parse(zone_case.posix);
    ASSERT_TRUE(parsed.has_value()) << zone_case.posix;
    const TimeZone zone = *parsed;
    const std::int64_t end = range_end();
    std::size_t index = 0;
    for (std::int64_t t = kHourlySkew; t < end; t += kHour) {
        const AssertionResult result = compare_instant(zone, t, (index++ % kDeepCheckStride) == 0);
        if (!result) {
            ADD_FAILURE() << zone_case.posix << ": " << result.message();
            return;
        }
    }
}

TEST_P(TzOracle, EveryTransitionMatchesGlibcToTheSecond) {
    const OracleCase& zone_case = GetParam();
    const ScopedTz env(zone_case.posix);
    const Result<TimeZone> parsed = TimeZone::parse(zone_case.posix);
    ASSERT_TRUE(parsed.has_value()) << zone_case.posix;
    const TimeZone zone = *parsed;
    const std::int64_t end = range_end();

    const std::vector<std::int64_t> flips = glibc_flips(end);
    ASSERT_EQ(engine_transitions(zone, end), flips) << zone_case.posix;

    for (const std::int64_t flip : flips) {
        for (const std::int64_t delta : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}}) {
            const AssertionResult result = compare_instant(zone, flip + delta, true);
            if (!result) {
                ADD_FAILURE() << zone_case.posix << " at transition " << flip << ": "
                              << result.message();
                return;
            }
        }
    }
    // next_transition() is strictly-after from every kind of starting point.
    if (flips.empty()) {
        EXPECT_FALSE(zone.next_transition(0).has_value()) << zone_case.posix;
        return;
    }
    EXPECT_EQ(zone.next_transition(0).value_or(kNoTransition), flips.front());
    for (std::size_t i = 0; i + 1 < flips.size(); ++i) {
        const std::int64_t middle = flips[i] + ((flips[i + 1] - flips[i]) / 2);
        EXPECT_EQ(zone.next_transition(flips[i]).value_or(kNoTransition), flips[i + 1]);
        EXPECT_EQ(zone.next_transition(middle).value_or(kNoTransition), flips[i + 1]);
        EXPECT_EQ(zone.next_transition(flips[i + 1] - 1).value_or(kNoTransition), flips[i + 1]);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
}

TEST_P(TzOracle, LocalToUtcMatchesMktimeAndTheGapAndOverlapPolicies) {
    const OracleCase& zone_case = GetParam();
    const ScopedTz env(zone_case.posix);
    const Result<TimeZone> parsed = TimeZone::parse(zone_case.posix);
    ASSERT_TRUE(parsed.has_value()) << zone_case.posix;
    const TimeZone zone = *parsed;
    const std::int64_t end = range_end();

    for (const std::int64_t flip : glibc_flips(end)) {
        const AssertionResult result = check_flip_resolution(zone, flip);
        if (!result) {
            ADD_FAILURE() << zone_case.posix << " at transition " << flip << ": "
                          << result.message();
            return;
        }
    }
    for (std::int64_t wall = 2 * kSecondsPerDay; wall < end; wall += kWallStride) {
        const AssertionResult result = compare_wall(zone, wall);
        if (!result) {
            ADD_FAILURE() << zone_case.posix << ": " << result.message();
            return;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Zones,
                         TzOracle,
                         ::testing::ValuesIn(kCases),
                         [](const ::testing::TestParamInfo<OracleCase>& param_info) {
                             return std::string(param_info.param.label);
                         });

TEST(TzOracleTable, CoversAtLeastThirtyDistinctZoneStrings) {
    EXPECT_GE(kCases.size(), 30U);
    for (std::size_t i = 0; i < kCases.size(); ++i) {
        for (std::size_t j = i + 1; j < kCases.size(); ++j) {
            EXPECT_NE(std::string_view(kCases[i].label), std::string_view(kCases[j].label));
            EXPECT_NE(std::string_view(kCases[i].posix), std::string_view(kCases[j].posix));
        }
    }
}

} // namespace
} // namespace qz::time
