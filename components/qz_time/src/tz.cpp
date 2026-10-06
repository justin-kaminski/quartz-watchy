// POSIX TZ engine (ARCHITECTURE.md section 9): parser, DST evaluation, local <-> UTC mapping.
//
// Evaluation model. A rule pair gives, for every nominal year Y, a DST start s(Y) and a DST end
// e(Y) as UTC instants (the start rule is a wall time in standard time, the end rule one in DST).
// The latest start s(Y) at or before t opens a DST period that lasts until e(Y), or until e(Y+1)
// when e(Y) < s(Y) (DST spanning new year: southern hemisphere, Dublin's negative DST). The zone
// is in DST iff that period has not ended at t. A period of a year or more therefore never ends,
// which is how RFC 9636's "0/0,J365/25" expresses permanent DST (tzcode reads it the same way),
// and an empty period (e == s) is standard time. The rule year is not taken from the UTC year of
// t (glibc does that, and mishandles transitions that straddle new year): the years around t are
// all examined. Every instant is clamped to +-1e14 s, so int64 arithmetic cannot overflow.
#include "qz/time/tz.hpp"

#include "time_math.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace qz::time {
namespace {

using detail::floor_div;
using detail::floor_mod;
using detail::saturate_to_i32;

// --- grammar limits (POSIX XBD 8.3, RFC 9636 section 3.3.1) ----------------------------------
constexpr std::size_t kMinNameLength = 3;
constexpr std::size_t kOffsetHourDigits = 2; ///< std/dst offset hh: 1-2 digits, 0..24
constexpr std::int32_t kMaxOffsetHours = 24;
constexpr std::size_t kRuleHourDigits = 3; ///< rule time hh: 1-3 digits, 0..167
constexpr std::int32_t kMaxRuleHours = 167;
constexpr std::size_t kMinuteSecondDigits = 2; ///< mm and ss are exactly two digits
constexpr std::int32_t kMaxMinuteOrSecond = 59;
constexpr std::size_t kDayOfYearDigits = 3; ///< Jn / n: 1-3 digits
constexpr std::int32_t kMaxDayOfYear = 365;
constexpr std::size_t kMonthDigits = 2;
constexpr std::int32_t kMaxMonth = 12;
constexpr std::int32_t kMaxWeek = 5;
constexpr std::int32_t kMaxWeekday = 6;
constexpr std::int32_t kHourSeconds = 3'600;
constexpr std::int32_t kMinuteSeconds = 60;

// --- evaluation constants --------------------------------------------------------------------
constexpr std::int32_t kDaysPerWeek = 7;
constexpr std::int32_t kFirstMarchJulianDay = 60; ///< Jn never counts Feb 29: J60 is March 1
constexpr std::int64_t kMaxAbsInstantS = 100'000'000'000'000;
/// dst_state() examines the start edges of the nominal years [y - 2, y + 1] around the year y of
/// the instant: rule transitions stay within about +-8 days of their nominal year.
constexpr std::int32_t kStartNewestYear = 1;
constexpr std::int32_t kStartOldestYear = -2;
/// next_transition() considers the edges of nominal years [y - 1, y + 2].
constexpr std::int32_t kNextEdgeFirstYear = -1;
constexpr std::int32_t kNextEdgeLastYear = 2;
constexpr std::size_t kNextEdgeCandidates = 8; ///< two rules x four years

// =================================================================================================
// Parser
// =================================================================================================

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
constexpr bool is_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
/// Characters allowed between '<' and '>' (POSIX): alphanumerics, '+' and '-'.
constexpr bool is_quoted_name_char(char c) noexcept {
    return is_alpha(c) || is_digit(c) || c == '+' || c == '-';
}

/// Read position over the TZ text. Ends are tested with at_end(), never with a NUL sentinel, so an
/// embedded NUL is just an invalid character.
class Cursor {
public:
    explicit Cursor(std::string_view text) noexcept : text_(text) {}

    [[nodiscard]] bool at_end() const noexcept { return pos_ >= text_.size(); }
    /// Current character, or NUL at the end of the text.
    [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : text_[pos_]; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::string_view slice(std::size_t begin, std::size_t end) const noexcept {
        return text_.substr(begin, end - begin);
    }
    void advance() noexcept {
        if (!at_end()) {
            ++pos_;
        }
    }
    /// Consumes `expected` if it is the current character.
    bool consume(char expected) noexcept {
        if (at_end() || text_[pos_] != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

/// Reads min_digits..max_digits decimal digits; fails when fewer are present.
[[nodiscard]] bool parse_number(Cursor& cur,
                                std::size_t min_digits,
                                std::size_t max_digits,
                                std::int32_t& value) noexcept {
    std::int32_t result = 0;
    std::size_t digits = 0;
    while (digits < max_digits && is_digit(cur.peek())) {
        result = (result * 10) + (cur.peek() - '0');
        cur.advance();
        ++digits;
    }
    if (digits < min_digits) {
        return false;
    }
    value = result;
    return true;
}

/// `hh[:mm[:ss]]` as seconds (never negative). mm and ss are exactly two digits.
[[nodiscard]] bool parse_clock(Cursor& cur,
                               std::size_t max_hour_digits,
                               std::int32_t max_hours,
                               std::int32_t& seconds) noexcept {
    std::int32_t hours = 0;
    std::int32_t minutes = 0;
    std::int32_t secs = 0;
    if (!parse_number(cur, 1, max_hour_digits, hours) || hours > max_hours) {
        return false;
    }
    if (cur.consume(':')) {
        if (!parse_number(cur, kMinuteSecondDigits, kMinuteSecondDigits, minutes) ||
            minutes > kMaxMinuteOrSecond) {
            return false;
        }
        if (cur.consume(':') &&
            (!parse_number(cur, kMinuteSecondDigits, kMinuteSecondDigits, secs) ||
             secs > kMaxMinuteOrSecond)) {
            return false;
        }
    }
    seconds = (hours * kHourSeconds) + (minutes * kMinuteSeconds) + secs;
    return true;
}

/// Optional sign: -1 for '-', +1 for '+' or none.
std::int32_t parse_sign(Cursor& cur) noexcept {
    if (cur.consume('-')) {
        return -1;
    }
    cur.consume('+');
    return 1;
}

/// std/dst designation: three or more letters, or `<...>` with alphanumerics, '+' and '-'. The
/// stored abbreviation never includes the angle brackets. Longer than the storage is rejected.
[[nodiscard]] bool parse_name(Cursor& cur, FixedString<10>& name) noexcept {
    const bool quoted = cur.consume('<');
    const std::size_t begin = cur.position();
    while (quoted ? is_quoted_name_char(cur.peek()) : is_alpha(cur.peek())) {
        cur.advance();
    }
    const std::size_t end = cur.position();
    if (quoted && !cur.consume('>')) {
        return false;
    }
    return (end - begin) >= kMinNameLength && name.assign(cur.slice(begin, end));
}

/// POSIX UTC offset `[+-]hh[:mm[:ss]]` (positive = WEST of Greenwich) -> seconds EAST of UTC.
[[nodiscard]] bool parse_utc_offset(Cursor& cur, std::int32_t& east_s) noexcept {
    const std::int32_t sign = parse_sign(cur);
    std::int32_t magnitude = 0;
    if (!parse_clock(cur, kOffsetHourDigits, kMaxOffsetHours, magnitude)) {
        return false;
    }
    east_s = -sign * magnitude;
    return true;
}

/// Rule time `[+-]hh[:mm[:ss]]`, hh 0..167 (RFC 9636 extension): negative and > 24 h allowed.
[[nodiscard]] bool parse_rule_time(Cursor& cur, std::int32_t& seconds) noexcept {
    const std::int32_t sign = parse_sign(cur);
    std::int32_t magnitude = 0;
    if (!parse_clock(cur, kRuleHourDigits, kMaxRuleHours, magnitude)) {
        return false;
    }
    seconds = sign * magnitude;
    return true;
}

/// The day number of `Jn` (1..365) or `n` (0..365).
[[nodiscard]] bool parse_day_of_year(Cursor& cur, std::int32_t min_day, TzRule& rule) noexcept {
    std::int32_t day = 0;
    if (!parse_number(cur, 1, kDayOfYearDigits, day) || day < min_day || day > kMaxDayOfYear) {
        return false;
    }
    rule.day = static_cast<std::uint16_t>(day);
    return true;
}

/// `m.w.d` after the 'M'.
[[nodiscard]] bool parse_month_week_day(Cursor& cur, TzRule& rule) noexcept {
    std::int32_t month = 0;
    std::int32_t week = 0;
    std::int32_t weekday = 0;
    const bool ok = parse_number(cur, 1, kMonthDigits, month) && month >= 1 && month <= kMaxMonth &&
                    cur.consume('.') && parse_number(cur, 1, 1, week) && week >= 1 &&
                    week <= kMaxWeek && cur.consume('.') && parse_number(cur, 1, 1, weekday) &&
                    weekday <= kMaxWeekday;
    if (!ok) {
        return false;
    }
    rule.month = static_cast<std::uint8_t>(month);
    rule.week = static_cast<std::uint8_t>(week);
    rule.weekday = static_cast<std::uint8_t>(weekday);
    return true;
}

/// One rule: `(Jn | n | Mm.w.d) [/time]`; the time defaults to 02:00:00.
[[nodiscard]] bool parse_rule(Cursor& cur, TzRule& rule) noexcept {
    TzRule parsed;
    if (cur.consume('J')) {
        parsed.kind = TzRule::Kind::kJulianNoLeap;
        if (!parse_day_of_year(cur, 1, parsed)) {
            return false;
        }
    } else if (cur.consume('M')) {
        parsed.kind = TzRule::Kind::kMonthWeekDay;
        if (!parse_month_week_day(cur, parsed)) {
            return false;
        }
    } else {
        parsed.kind = TzRule::Kind::kZeroBasedDay;
        if (!parse_day_of_year(cur, 0, parsed)) {
            return false;
        }
    }
    if (cur.consume('/') && !parse_rule_time(cur, parsed.time_s)) {
        return false;
    }
    rule = parsed;
    return true;
}

// =================================================================================================
// Evaluation
// =================================================================================================

/// Everything the evaluator needs from a TimeZone (its members are private).
struct RulePair {
    const TzRule& start;
    const TzRule& end;
    std::int32_t std_offset_s;
    std::int32_t dst_offset_s;
};

[[nodiscard]] constexpr std::int64_t clamp_instant(UnixSeconds t) noexcept {
    return std::clamp(t, -kMaxAbsInstantS, kMaxAbsInstantS);
}

/// Civil year of the instant (UTC calendar).
[[nodiscard]] std::int32_t year_of_instant(std::int64_t t) noexcept {
    return civil_from_days(saturate_to_i32(floor_div(t, kSecondsPerDay))).year;
}

/// Day number of the `Mm.w.d` rule in `year` (week 5 = last occurrence in the month).
[[nodiscard]] std::int64_t month_week_day(const TzRule& rule, std::int32_t year) noexcept {
    const DayNumber first = days_from_civil(CivilDate{year, rule.month, 1});
    const auto first_weekday = static_cast<std::int32_t>(weekday_from_days(first));
    const auto to_weekday =
        static_cast<std::int32_t>(floor_mod(rule.weekday - first_weekday, kDaysPerWeek));
    std::int32_t day = 1 + to_weekday + ((rule.week - 1) * kDaysPerWeek);
    const std::int32_t length = days_in_month(year, rule.month);
    while (day > length) {
        day -= kDaysPerWeek;
    }
    return std::int64_t{first} + (day - 1);
}

/// Day number selected by the rule in `year`.
[[nodiscard]] std::int64_t rule_day(const TzRule& rule, std::int32_t year) noexcept {
    const std::int64_t jan_first = days_from_civil(CivilDate{year, 1, 1});
    switch (rule.kind) {
        case TzRule::Kind::kJulianNoLeap: {
            const bool after_feb = is_leap_year(year) && rule.day >= kFirstMarchJulianDay;
            return jan_first + (rule.day - 1) + (after_feb ? 1 : 0);
        }
        case TzRule::Kind::kZeroBasedDay:
            return jan_first + rule.day;
        case TzRule::Kind::kMonthWeekDay:
            return month_week_day(rule, year);
    }
    return jan_first; // unreachable: Kind is a closed enum
}

/// UTC instant of the rule in `year`, given the offset the wall clock shows just before it.
[[nodiscard]] std::int64_t
edge_at(const TzRule& rule, std::int32_t offset_s, std::int32_t year) noexcept {
    return (rule_day(rule, year) * kSecondsPerDay) + rule.time_s - offset_s;
}

/// End of the DST period that begins at the start edge `start_at` of `year`: the end rule of the
/// same nominal year, or, when that falls before the start (DST spans new year), the end rule of
/// the following year.
[[nodiscard]] std::int64_t
period_end(const RulePair& rules, std::int32_t year, std::int64_t start_at) noexcept {
    const std::int64_t same_year = edge_at(rules.end, rules.dst_offset_s, year);
    if (same_year >= start_at) {
        return same_year;
    }
    return edge_at(rules.end, rules.dst_offset_s, year + 1);
}

/// True when the zone is in DST at instant t (already clamped): the period opened by the latest
/// start edge at or before t has not ended yet.
[[nodiscard]] bool dst_state(const RulePair& rules, std::int64_t t) noexcept {
    const std::int32_t year = year_of_instant(t);
    for (std::int32_t y = year + kStartNewestYear; y >= year + kStartOldestYear; --y) {
        const std::int64_t start_at = edge_at(rules.start, rules.std_offset_s, y);
        if (start_at <= t) {
            return t < period_end(rules, y, start_at);
        }
    }
    return false; // unreachable: the oldest year examined starts over a year before t
}

} // namespace

// =================================================================================================
// TimeZone
// =================================================================================================

Result<TimeZone> TimeZone::parse(std::string_view posix) noexcept {
    Cursor cur{posix};
    TimeZone zone;
    if (!parse_name(cur, zone.std_abbr_) || !parse_utc_offset(cur, zone.std_offset_s_)) {
        return Errc::kBadArgs;
    }
    zone.dst_offset_s_ = zone.std_offset_s_;
    if (cur.at_end()) {
        return zone; // fixed offset, no DST
    }
    // `dst [offset] , start , end`: the rule pair is mandatory (a default would be a guess).
    if (!parse_name(cur, zone.dst_abbr_)) {
        return Errc::kBadArgs;
    }
    zone.dst_offset_s_ = zone.std_offset_s_ + kHourSeconds;
    if (!cur.at_end() && cur.peek() != ',' && !parse_utc_offset(cur, zone.dst_offset_s_)) {
        return Errc::kBadArgs;
    }
    if (!cur.consume(',') || !parse_rule(cur, zone.dst_start_) || !cur.consume(',') ||
        !parse_rule(cur, zone.dst_end_) || !cur.at_end()) {
        return Errc::kBadArgs;
    }
    zone.has_dst_ = true;
    return zone;
}

bool TimeZone::is_dst_at(UnixSeconds t) const noexcept {
    if (!has_dst_) {
        return false;
    }
    const RulePair rules{dst_start_, dst_end_, std_offset_s_, dst_offset_s_};
    return dst_state(rules, clamp_instant(t));
}

std::int32_t TimeZone::utc_offset_at(UnixSeconds t) const noexcept {
    return is_dst_at(t) ? dst_offset_s_ : std_offset_s_;
}

LocalDateTime TimeZone::to_local(UnixSeconds t) const noexcept {
    const std::int64_t instant = clamp_instant(t);
    const bool dst = is_dst_at(instant);
    const std::int32_t offset = dst ? dst_offset_s_ : std_offset_s_;
    const std::int64_t wall = instant + offset;
    const DayNumber day = saturate_to_i32(floor_div(wall, kSecondsPerDay));
    const auto second_of_day = static_cast<std::int32_t>(floor_mod(wall, kSecondsPerDay));
    LocalDateTime local;
    local.date = civil_from_days(day);
    local.time =
        CivilTime{static_cast<std::uint8_t>(second_of_day / kHourSeconds),
                  static_cast<std::uint8_t>((second_of_day % kHourSeconds) / kMinuteSeconds),
                  static_cast<std::uint8_t>(second_of_day % kMinuteSeconds)};
    local.weekday = weekday_from_days(day);
    local.utc_offset_s = offset;
    local.is_dst = dst;
    return local;
}

Result<UnixSeconds>
TimeZone::to_utc(const CivilDate& date, const CivilTime& time, GapPolicy policy) const noexcept {
    if (!is_valid(date, time)) {
        return Errc::kBadArgs;
    }
    const std::int64_t wall = (std::int64_t{days_from_civil(date)} * kSecondsPerDay) +
                              (std::int64_t{time.hour} * detail::kSecondsPerHour) +
                              (std::int64_t{time.minute} * kSecondsPerMinute) + time.second;
    // The wall time maps to UTC through one of two offsets; a candidate is real only when the
    // zone really is in that offset at that instant. Two real candidates = overlap, none = gap.
    const std::int64_t via_std = wall - std_offset_s_;
    const std::int64_t via_dst = wall - dst_offset_s_;
    const bool std_real = utc_offset_at(via_std) == std_offset_s_;
    const bool dst_real = has_dst_ && utc_offset_at(via_dst) == dst_offset_s_;
    if (std_real != dst_real) {
        return std_real ? via_std : via_dst;
    }
    if (via_std == via_dst) {
        return via_std; // both offsets equal: one instant, nothing to resolve
    }
    if (policy == GapPolicy::kReject) {
        return Errc::kBadArgs;
    }
    if (!std_real) {
        // Gap: the wall time is read with the offset in force before the jump, i.e. shifted
        // forward by the gap length.
        return std::max(via_std, via_dst);
    }
    return (policy == GapPolicy::kEarlier) ? std::min(via_std, via_dst)
                                           : std::max(via_std, via_dst);
}

std::optional<UnixSeconds> TimeZone::next_transition(UnixSeconds t) const noexcept {
    if (!has_dst_) {
        return std::nullopt;
    }
    const std::int64_t instant = clamp_instant(t);
    const RulePair rules{dst_start_, dst_end_, std_offset_s_, dst_offset_s_};
    const std::int32_t year = year_of_instant(instant);
    std::array<std::int64_t, kNextEdgeCandidates> candidates{};
    std::size_t count = 0;
    for (std::int32_t y = year + kNextEdgeFirstYear; y <= year + kNextEdgeLastYear; ++y) {
        candidates[count++] = edge_at(rules.start, rules.std_offset_s, y);
        candidates[count++] = edge_at(rules.end, rules.dst_offset_s, y);
    }
    std::ranges::sort(candidates);
    // The state only changes at edges, so the first edge after t whose state differs from the
    // current one is the next real transition (ties and no-op edges are skipped).
    const bool current = dst_state(rules, instant);
    for (const std::int64_t at : candidates) {
        if (at > instant && dst_state(rules, at) != current) {
            return at;
        }
    }
    return std::nullopt;
}

std::string_view TimeZone::abbreviation(bool dst) const noexcept {
    return (dst && has_dst_) ? dst_abbr_.view() : std_abbr_.view();
}

} // namespace qz::time
