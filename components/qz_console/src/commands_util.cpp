// Parsing and formatting helpers of the command catalog. No heap, no exceptions.
#include "commands_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

namespace qz::console::detail {
namespace {

/// Writes `value` as exactly `width` zero-padded decimal digits at `out`.
void put_digits(char* out, std::uint32_t value, std::size_t width) noexcept {
    for (std::size_t i = width; i > 0; --i) {
        out[i - 1] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    }
}

/// Longest decimal accepted by parse_integer: 18 digits always fit an int64 and are far beyond any
/// argument range of the catalog.
constexpr std::size_t kMaxDigits = 18;

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

template<class E>
struct NameEntry {
    E value;
    std::string_view name;
};

template<class E, std::size_t N>
std::string_view
name_of(const std::array<NameEntry<E>, N>& table, E value, std::string_view fallback) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return fallback;
}

constexpr std::array<NameEntry<model::Button>, 4> kButtons{{
    {model::Button::kMenu, "menu"},
    {model::Button::kBack, "back"},
    {model::Button::kUp, "up"},
    {model::Button::kDown, "down"},
}};

constexpr std::array<NameEntry<model::InputKind>, 3> kInputKinds{{
    {model::InputKind::kClick, "click"},
    {model::InputKind::kHold, "hold"},
    {model::InputKind::kRepeat, "repeat"},
}};

constexpr std::array<NameEntry<model::PowerLevel>, 4> kPowerLevels{{
    {model::PowerLevel::kNormal, "normal"},
    {model::PowerLevel::kLow, "low"},
    {model::PowerLevel::kSaver, "saver"},
    {model::PowerLevel::kCritical, "critical"},
}};

constexpr std::array<NameEntry<model::WeatherCondition>, 10> kConditions{{
    {model::WeatherCondition::kUnknown, "unknown"},
    {model::WeatherCondition::kClear, "clear"},
    {model::WeatherCondition::kPartlyCloudy, "partly_cloudy"},
    {model::WeatherCondition::kCloudy, "cloudy"},
    {model::WeatherCondition::kFog, "fog"},
    {model::WeatherCondition::kDrizzle, "drizzle"},
    {model::WeatherCondition::kRain, "rain"},
    {model::WeatherCondition::kSnow, "snow"},
    {model::WeatherCondition::kShowers, "showers"},
    {model::WeatherCondition::kThunder, "thunder"},
}};

constexpr std::array<NameEntry<model::WeatherFreshness>, 3> kFreshness{{
    {model::WeatherFreshness::kFresh, "fresh"},
    {model::WeatherFreshness::kStale, "stale"},
    {model::WeatherFreshness::kHidden, "hidden"},
}};

constexpr std::array<NameEntry<model::WakeCause>, 8> kWakeCauses{{
    {model::WakeCause::kColdBoot, "coldboot"},
    {model::WakeCause::kReset, "reset"},
    {model::WakeCause::kTimer, "timer"},
    {model::WakeCause::kButton, "button"},
    {model::WakeCause::kAccel, "accel"},
    {model::WakeCause::kUsb, "usb"},
    {model::WakeCause::kTetheredTick, "tick"},
    {model::WakeCause::kUnknown, "unknown"},
}};

constexpr std::array<NameEntry<model::SyncIndicator>, 5> kIndicators{{
    {model::SyncIndicator::kNone, "none"},
    {model::SyncIndicator::kNeverSynced, "never"},
    {model::SyncIndicator::kLastFailed, "failed"},
    {model::SyncIndicator::kStale, "stale"},
    {model::SyncIndicator::kOk, "ok"},
}};

} // namespace

Result<std::int64_t>
parse_integer(std::string_view text, std::int64_t min, std::int64_t max) noexcept {
    const bool negative = !text.empty() && text.front() == '-';
    const std::string_view digits = negative ? text.substr(1) : text;
    if (digits.empty() || digits.size() > kMaxDigits) {
        return Errc::kBadArgs;
    }
    std::int64_t magnitude = 0; // at most 18 digits: cannot overflow an int64
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return Errc::kBadArgs;
        }
        magnitude = (magnitude * 10) + (c - '0');
    }
    const std::int64_t value = negative ? -magnitude : magnitude;
    if (value < min || value > max) {
        return Errc::kBadArgs;
    }
    return value;
}

bool contains_ignore_case(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool same = true;
        for (std::size_t i = 0; i < needle.size() && same; ++i) {
            same = lower(haystack[start + i]) == lower(needle[i]);
        }
        if (same) {
            return true;
        }
    }
    return needle.empty();
}

Status require_radio(const DeviceApi& api) noexcept {
    if (!api.firmware().radio_compiled) {
        return Errc::kUnsupported;
    }
    return ok();
}

std::string_view format_date(std::span<char> out, time::DayNumber day) noexcept {
    constexpr std::size_t kLen = 10; // YYYY-MM-DD
    if (out.size() < kLen) {
        return {};
    }
    const time::CivilDate date = time::civil_from_days(day);
    put_digits(out.data(), static_cast<std::uint32_t>(date.year) % 10'000U, 4);
    out[4] = '-';
    put_digits(out.data() + 5, date.month, 2);
    out[7] = '-';
    put_digits(out.data() + 8, date.day, 2);
    return {out.data(), kLen};
}

namespace {

void put_time(char* out, const time::CivilTime& t) noexcept {
    put_digits(out, t.hour, 2);
    out[2] = ':';
    put_digits(out + 3, t.minute, 2);
    out[5] = ':';
    put_digits(out + 6, t.second, 2);
}

} // namespace

std::string_view format_utc_iso(std::span<char> out, time::UnixSeconds t) noexcept {
    const std::size_t len = time::format_iso8601_utc(out, t);
    return {out.data(), len};
}

std::string_view format_local_iso(std::span<char> out, const time::LocalDateTime& local) noexcept {
    constexpr std::size_t kLen = 25; // YYYY-MM-DDTHH:MM:SS+HH:MM
    if (out.size() < kLen) {
        return {};
    }
    put_digits(out.data(), static_cast<std::uint32_t>(local.date.year) % 10'000U, 4);
    out[4] = '-';
    put_digits(out.data() + 5, local.date.month, 2);
    out[7] = '-';
    put_digits(out.data() + 8, local.date.day, 2);
    out[10] = 'T';
    put_time(out.data() + 11, local.time);
    const std::int32_t offset = local.utc_offset_s;
    const std::uint32_t minutes = static_cast<std::uint32_t>(offset < 0 ? -offset : offset) / 60U;
    out[19] = offset < 0 ? '-' : '+';
    put_digits(out.data() + 20, (minutes / 60U) % 100U, 2);
    out[22] = ':';
    put_digits(out.data() + 23, minutes % 60U, 2);
    return {out.data(), kLen};
}

std::string_view format_hex32(std::span<char> out, std::uint32_t value) noexcept {
    constexpr std::size_t kLen = 8;
    if (out.size() < kLen) {
        return {};
    }
    constexpr std::string_view kDigits = "0123456789abcdef";
    for (std::size_t i = kLen; i > 0; --i) {
        out[i - 1] = kDigits[value & 0xFU];
        value >>= 4U;
    }
    return {out.data(), kLen};
}

std::string_view format_decimal(std::span<char> out, std::int64_t value) noexcept {
    const auto result = std::to_chars(out.data(), out.data() + out.size(), value);
    if (result.ec != std::errc{}) {
        return {};
    }
    return {out.data(), static_cast<std::size_t>(result.ptr - out.data())};
}

std::string_view button_name(model::Button button) noexcept {
    return name_of(kButtons, button, "unknown");
}

Result<model::Button> parse_button(std::string_view name) noexcept {
    for (const auto& entry : kButtons) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return Errc::kBadArgs;
}

Result<model::InputKind> parse_input_kind(std::string_view name) noexcept {
    for (const auto& entry : kInputKinds) {
        if (entry.name == name) {
            return entry.value;
        }
    }
    return Errc::kBadArgs;
}

std::string_view power_level_name(model::PowerLevel level) noexcept {
    return name_of(kPowerLevels, level, "unknown");
}

std::string_view weather_condition_name(model::WeatherCondition condition) noexcept {
    return name_of(kConditions, condition, "unknown");
}

Result<model::WeatherCondition> parse_weather_condition(std::string_view text) noexcept {
    for (const auto& entry : kConditions) {
        if (entry.name == text) {
            return entry.value;
        }
    }
    const Result<std::int64_t> code =
        parse_integer(text, 0, static_cast<std::int64_t>(kConditions.size()) - 1);
    if (!code) {
        return Errc::kBadArgs;
    }
    return static_cast<model::WeatherCondition>(*code);
}

std::string_view freshness_name(model::WeatherFreshness freshness) noexcept {
    return name_of(kFreshness, freshness, "hidden");
}

std::string_view wake_cause_name(model::WakeCause cause) noexcept {
    return name_of(kWakeCauses, cause, "unknown");
}

std::string_view sync_indicator_name(model::SyncIndicator indicator) noexcept {
    return name_of(kIndicators, indicator, "none");
}

std::string_view stored_error_token(std::uint8_t errc_plus_one) noexcept {
    if (errc_plus_one == 0) {
        return {};
    }
    const unsigned code = errc_plus_one - 1U;
    if (code > static_cast<unsigned>(Errc::kInternal)) {
        return "unknown";
    }
    return to_token(static_cast<Errc>(code));
}

} // namespace qz::console::detail
