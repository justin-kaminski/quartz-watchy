// parse_iso8601: strict "YYYY-MM-DDTHH:MM[:SS]" + optional "Z" / "+HH:MM" / "-HH:MM".
// Only ASCII digits and the exact punctuation are accepted: no whitespace, lowercase t/z, basic
// (unseparated) forms, fractional seconds or leap seconds. The result must lie in
// [1970-01-01T00:00:00Z, 2200-01-01T00:00:00Z), the calendar window of is_valid().
#include "qz/time/timekeeper.hpp"
#include "tuning.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace qz::time {
namespace {

constexpr std::size_t kMinuteFormLength = 16; // YYYY-MM-DDTHH:MM
constexpr std::size_t kSecondFormLength = 19; // YYYY-MM-DDTHH:MM:SS
constexpr std::size_t kOffsetLength = 6;      // +HH:MM
constexpr std::int32_t kDecimalBase = 10;
constexpr std::int32_t kSecondsPerHourI = 3600;
constexpr std::int32_t kSecondsPerMinuteI = 60;

/// Parses exactly `count` ASCII digits at text[pos..pos+count).
[[nodiscard]] std::optional<std::int32_t>
digits(std::string_view text, std::size_t pos, std::size_t count) noexcept {
    if (pos + count > text.size()) {
        return std::nullopt;
    }
    std::int32_t value = 0;
    for (std::size_t i = pos; i < pos + count; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * kDecimalBase) + (c - '0');
    }
    return value;
}

/// Offset east of UTC in seconds from "Z" or "+HH:MM"; nullopt = malformed. `rest` is the text
/// after the time; empty means "no offset" and is handled by the caller.
[[nodiscard]] std::optional<std::int32_t> numeric_offset_s(std::string_view rest) noexcept {
    if (rest.size() != kOffsetLength || (rest[0] != '+' && rest[0] != '-') || rest[3] != ':') {
        return std::nullopt;
    }
    const auto hours = digits(rest, 1, 2);
    const auto minutes = digits(rest, 4, 2);
    if (!hours || !minutes || *hours > tuning::kIsoMaxOffsetHours ||
        *minutes > tuning::kIsoMaxOffsetMinutes) {
        return std::nullopt;
    }
    const std::int32_t magnitude = (*hours * kSecondsPerHourI) + (*minutes * kSecondsPerMinuteI);
    return (rest[0] == '-') ? -magnitude : magnitude;
}

} // namespace

Result<UnixSeconds> parse_iso8601(std::string_view text, const TimeZone& zone) noexcept {
    if (text.size() < kMinuteFormLength || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':') {
        return Errc::kBadArgs;
    }
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    const auto hour = digits(text, 11, 2);
    const auto minute = digits(text, 14, 2);
    if (!year || !month || !day || !hour || !minute) {
        return Errc::kBadArgs;
    }
    std::int32_t second = 0;
    std::size_t pos = kMinuteFormLength;
    if (text.size() > pos && text[pos] == ':') {
        const auto parsed = digits(text, pos + 1, 2);
        if (!parsed) {
            return Errc::kBadArgs;
        }
        second = *parsed;
        pos = kSecondFormLength;
    }

    const CivilDate date{*year, static_cast<std::uint8_t>(*month), static_cast<std::uint8_t>(*day)};
    const CivilTime clock{static_cast<std::uint8_t>(*hour),
                          static_cast<std::uint8_t>(*minute),
                          static_cast<std::uint8_t>(second)};
    if (!is_valid(date, clock)) {
        return Errc::kBadArgs;
    }

    const std::string_view rest = text.substr(pos);
    std::int64_t utc = 0;
    if (rest.empty()) {
        const Result<UnixSeconds> local = zone.to_utc(date, clock, GapPolicy::kEarlier);
        if (!local) {
            return Errc::kBadArgs;
        }
        utc = *local;
    } else {
        std::int32_t offset_s = 0;
        if (rest != "Z") {
            const auto parsed = numeric_offset_s(rest);
            if (!parsed) {
                return Errc::kBadArgs;
            }
            offset_s = *parsed;
        }
        utc = (static_cast<std::int64_t>(days_from_civil(date)) * kSecondsPerDay) +
              (static_cast<std::int64_t>(clock.hour) * kSecondsPerHourI) +
              (static_cast<std::int64_t>(clock.minute) * kSecondsPerMinuteI) + clock.second -
              offset_s;
    }

    const UnixSeconds window_end =
        static_cast<std::int64_t>(days_from_civil(CivilDate{tuning::kIsoEndYear, 1, 1})) *
        kSecondsPerDay;
    if (utc < 0 || utc >= window_end) {
        return Errc::kBadArgs;
    }
    return utc;
}

} // namespace qz::time
