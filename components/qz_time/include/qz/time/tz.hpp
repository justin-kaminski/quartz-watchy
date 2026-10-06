// POSIX TZ engine (ARCHITECTURE.md section 9). Oracle-tested against glibc on the host.
// TimeZone is a small trivially copyable value; all operations are pure and reentrant.
#pragma once

#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"
#include "qz/time/civil.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::time {

/// Resolution of local times that fall into a DST gap or overlap.
enum class GapPolicy : std::uint8_t {
    kEarlier, ///< overlap: first occurrence; gap: shift forward by the gap length
    kLater,   ///< overlap: second occurrence; gap: shift forward by the gap length
    kReject,  ///< gap or overlap -> Errc::kBadArgs
};

/// One POSIX transition rule: Jn (1..365, no Feb 29), n (0..365), or Mm.w.d.
struct TzRule {
    enum class Kind : std::uint8_t { kJulianNoLeap, kZeroBasedDay, kMonthWeekDay };
    Kind kind = Kind::kMonthWeekDay;
    std::uint16_t day = 0;      ///< Jn / n
    std::uint8_t month = 0;     ///< Mm: 1..12
    std::uint8_t week = 0;      ///< Mm.w.d: 1..5 (5 = last)
    std::uint8_t weekday = 0;   ///< Mm.w.d: 0..6 (0 = Sunday)
    std::int32_t time_s = 7200; ///< local wall time of the transition, -167h..+167h (RFC 9636)
};

class TimeZone {
public:
    /// UTC ("UTC0").
    TimeZone() noexcept = default;
    /// Parses a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3", "<+0545>-5:45",
    /// "EST5EDT,M3.2.0,M11.1.0"). Errc::kBadArgs on any syntax/range error. No heap.
    static Result<TimeZone> parse(std::string_view posix) noexcept;

    /// Offset in seconds east of UTC at instant t (DST included).
    [[nodiscard]] std::int32_t utc_offset_at(UnixSeconds t) const noexcept;
    [[nodiscard]] bool is_dst_at(UnixSeconds t) const noexcept;
    [[nodiscard]] LocalDateTime to_local(UnixSeconds t) const noexcept;
    /// Local wall time -> UTC with explicit gap/overlap handling.
    [[nodiscard]] Result<UnixSeconds>
    to_utc(const CivilDate& date, const CivilTime& time, GapPolicy policy) const noexcept;
    /// First transition strictly after t, if the zone has DST.
    [[nodiscard]] std::optional<UnixSeconds> next_transition(UnixSeconds t) const noexcept;
    [[nodiscard]] std::string_view abbreviation(bool dst) const noexcept;
    [[nodiscard]] bool has_dst() const noexcept { return has_dst_; }

private:
    FixedString<10> std_abbr_{"UTC"};
    FixedString<10> dst_abbr_;
    std::int32_t std_offset_s_ = 0; ///< seconds east (POSIX sign inverted at parse time)
    std::int32_t dst_offset_s_ = 0;
    TzRule dst_start_{};
    TzRule dst_end_{};
    bool has_dst_ = false;
};

/// Built-in zone list entry (generated table, ARCHITECTURE.md section 9).
struct TzEntry {
    std::string_view name;     ///< IANA name, e.g. "Europe/Berlin" (stored in settings)
    std::string_view label;    ///< UI label, e.g. "Berlin (UTC+1)"
    std::string_view posix;    ///< TZif footer string
    std::int32_t std_offset_s; ///< seconds east, for sorting/grouping
};

/// Generated, sorted by std offset then label. Includes "UTC".
[[nodiscard]] std::span<const TzEntry> builtin_zones() noexcept;
/// nullptr when not found.
[[nodiscard]] const TzEntry* find_zone(std::string_view iana_name) noexcept;
/// IANA release used by the generator, e.g. "2026b".
[[nodiscard]] std::string_view tzdata_version() noexcept;

} // namespace qz::time
