// Text formatting for civil time (English names in v1). Integer-only, heap-free, no NUL written:
// every formatter either fills its output completely and returns the length, or writes nothing and
// returns 0 (buffer too small, or a value that has no text form).
#include "qz/time/civil.hpp"
#include "time_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::time {
namespace {

constexpr std::array<std::string_view, 7> kWeekdayNames{
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<std::string_view, 7> kWeekdayAbbreviations{
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<std::string_view, 12> kMonthNames{"January",
                                                       "February",
                                                       "March",
                                                       "April",
                                                       "May",
                                                       "June",
                                                       "July",
                                                       "August",
                                                       "September",
                                                       "October",
                                                       "November",
                                                       "December"};
constexpr std::array<std::string_view, 12> kMonthAbbreviations{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

constexpr std::uint8_t kHoursPerDay = 24;
constexpr std::uint8_t kMinutesPerHour = 60;
constexpr std::uint8_t kNoonHour = 12;
constexpr std::uint8_t kFirstTwoDigitHour = 10;
constexpr std::size_t kHhmm24hLength = 5;   ///< "HH:MM"
constexpr std::size_t kHhmmShortLength = 4; ///< "H:MM"
constexpr std::size_t kIsoLength = 20;      ///< "YYYY-MM-DDTHH:MM:SSZ"
constexpr std::int32_t kMaxIsoYear = 9'999;
constexpr std::size_t kIsoYearDigits = 4;
constexpr std::int64_t kSecondsPerHourValue = detail::kSecondsPerHour;

/// Sequential writer into a buffer whose capacity the caller has already checked.
class Writer {
public:
    explicit Writer(std::span<char> out) noexcept : out_(out) {}

    void put(char c) noexcept {
        out_[pos_] = c;
        ++pos_;
    }
    /// Zero-padded decimal, exactly `digits` wide (value must fit).
    void put_number(std::int64_t value, std::size_t digits) noexcept {
        for (std::size_t i = digits; i > 0; --i) {
            out_[pos_ + i - 1] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        pos_ += digits;
    }
    [[nodiscard]] std::size_t size() const noexcept { return pos_; }

private:
    std::span<char> out_;
    std::size_t pos_ = 0;
};

} // namespace

std::string_view weekday_name(Weekday wd, bool abbreviated) noexcept {
    const auto index = static_cast<std::size_t>(wd);
    if (index >= kWeekdayNames.size()) {
        return {};
    }
    return abbreviated ? kWeekdayAbbreviations[index] : kWeekdayNames[index];
}

std::string_view month_name(std::uint8_t month, bool abbreviated) noexcept {
    if (month < 1 || month > kMonthNames.size()) {
        return {};
    }
    return abbreviated ? kMonthAbbreviations[month - 1U] : kMonthNames[month - 1U];
}

std::size_t
format_hhmm(std::span<char> out, const CivilTime& t, HourFormat fmt, bool* is_pm) noexcept {
    if (t.hour >= kHoursPerDay || t.minute >= kMinutesPerHour) {
        return 0;
    }
    std::uint8_t shown_hour = t.hour;
    if (fmt == HourFormat::k12h) {
        shown_hour = static_cast<std::uint8_t>(t.hour % kNoonHour);
        if (shown_hour == 0) {
            shown_hour = kNoonHour;
        }
    }
    const bool padded = (fmt == HourFormat::k24h) || (shown_hour >= kFirstTwoDigitHour);
    if (out.size() < (padded ? kHhmm24hLength : kHhmmShortLength)) {
        return 0;
    }
    Writer writer{out};
    if (padded) {
        writer.put_number(shown_hour, 2);
    } else {
        writer.put_number(shown_hour, 1);
    }
    writer.put(':');
    writer.put_number(t.minute, 2);
    if (is_pm != nullptr) {
        *is_pm = t.hour >= kNoonHour; // the faithful PM flag in either hour format
    }
    return writer.size();
}

std::size_t format_iso8601_utc(std::span<char> out, UnixSeconds t) noexcept {
    if (out.size() < kIsoLength) {
        return 0;
    }
    const CivilDate date = civil_from_days(day_of(t));
    if (date.year < 0 || date.year > kMaxIsoYear) {
        return 0; // no four-digit form (also catches the saturated day numbers of huge |t|)
    }
    const std::int64_t second_of_day = detail::floor_mod(t, kSecondsPerDay);
    Writer writer{out};
    writer.put_number(date.year, kIsoYearDigits);
    writer.put('-');
    writer.put_number(date.month, 2);
    writer.put('-');
    writer.put_number(date.day, 2);
    writer.put('T');
    writer.put_number(second_of_day / kSecondsPerHourValue, 2);
    writer.put(':');
    writer.put_number((second_of_day % kSecondsPerHourValue) / kSecondsPerMinute, 2);
    writer.put(':');
    writer.put_number(second_of_day % kSecondsPerMinute, 2);
    writer.put('Z');
    return writer.size();
}

} // namespace qz::time
