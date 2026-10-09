// Settings schema, parsing, formatting and validation. One validation path for UI editors,
// console `settings set` and the provisioning form (ARCHITECTURE.md sections 7 and 13).
#include "internal.hpp"
#include "qz/core/assert.hpp"
#include "qz/settings/settings.hpp"
#include "qz/time/tz.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace qz::settings {
namespace {

// Persisted enum value == index into the choices list == underlying enum value.
static_assert(static_cast<unsigned>(model::HourFormat::k24h) == 0);
static_assert(static_cast<unsigned>(model::HourFormat::k12h) == 1);
static_assert(static_cast<unsigned>(model::TempUnit::kCelsius) == 0);
static_assert(static_cast<unsigned>(model::TempUnit::kFahrenheit) == 1);
static_assert(static_cast<unsigned>(model::ConnectivityMode::kOff) == 0);
static_assert(static_cast<unsigned>(model::ConnectivityMode::kTimeOnly) == 1);
static_assert(static_cast<unsigned>(model::ConnectivityMode::kTimeWeather) == 2);

constexpr std::array<std::string_view, 2> kHourChoices{"24h", "12h"};
constexpr std::array<std::string_view, 2> kUnitChoices{"c", "f"};
constexpr std::array<std::string_view, 3> kConnChoices{"off", "time", "time+weather"};
constexpr std::array<std::string_view, 2> kBoolChoices{"off", "on"};
constexpr std::array<std::string_view, 5> kSyncChoices{"6", "12", "24", "48", "168"};
constexpr std::array<std::string_view, 5> kWeatherChoices{"30", "60", "120", "180", "360"};

constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::kCount);

// Order must match enum Key (checked below).
constexpr std::array<KeyInfo, kKeyCount> kSchema{{
    {Key::kHourFormat, "tfmt", ValueType::kEnum, 0, 1, kHourChoices, "clock format: 24h | 12h"},
    {Key::kTimeZone, "tz", ValueType::kZoneName, 0, 0, {}, "IANA zone name from the built-in list"},
    {Key::kTempUnit, "units", ValueType::kEnum, 0, 1, kUnitChoices, "temperature unit: c | f"},
    {Key::kConnectivity,
     "conn",
     ValueType::kEnum,
     0,
     2,
     kConnChoices,
     "connectivity: off | time | time+weather"},
    {Key::kWeatherHighLow,
     "wx_hilo",
     ValueType::kBool,
     0,
     1,
     kBoolChoices,
     "show daily high/low: on | off"},
    {Key::kLatitude,
     "lat",
     ValueType::kDegrees,
     -tuning::kLatMaxE5,
     tuning::kLatMaxE5,
     {},
     "latitude in decimal degrees, or unset"},
    {Key::kLongitude,
     "lon",
     ValueType::kDegrees,
     -tuning::kLonMaxE5,
     tuning::kLonMaxE5,
     {},
     "longitude in decimal degrees, or unset"},
    {Key::kSyncIntervalH,
     "sync_h",
     ValueType::kUInt,
     6,
     168,
     kSyncChoices,
     "hours between time syncs: 6 | 12 | 24 | 48 | 168"},
    {Key::kWeatherIntervalMin,
     "wx_min",
     ValueType::kUInt,
     30,
     360,
     kWeatherChoices,
     "minutes between weather updates: 30 | 60 | 120 | 180 | 360"},
    {Key::kStepGoal,
     "goal",
     ValueType::kUInt,
     0,
     tuning::kStepGoalMax,
     {},
     "daily step goal, multiple of 500; 0 = off"},
    {Key::kVibration, "vib", ValueType::kBool, 0, 1, kBoolChoices, "vibration: on | off"},
    {Key::kFace, "face", ValueType::kUInt, 0, tuning::kFaceIdMax, {}, "watch face id"},
    {Key::kTapWake, "tapwake", ValueType::kBool, 0, 1, kBoolChoices, "wake on wrist tap: on | off"},
    {Key::kPhoneSync,
     "phone",
     ValueType::kBool,
     0,
     1,
     kBoolChoices,
     "phone sync over Bluetooth: on | off (off = never powered)"},
}};

constexpr bool schema_is_ordered() {
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        if (static_cast<std::size_t>(kSchema[i].key) != i) {
            return false;
        }
    }
    return true;
}
static_assert(schema_is_ordered(), "kSchema order must match enum Key");

// ---- text helpers (ASCII only, locale independent) ----

constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

/// Plain decimal digits only: no sign, no whitespace, at most 10 digits. Result <= UINT32_MAX.
bool parse_uint(std::string_view text, std::int64_t& out) noexcept {
    if (text.empty() || text.size() > tuning::kUIntDigitsMax) {
        return false;
    }
    std::int64_t v = 0;
    for (const char c : text) {
        if (!is_digit(c)) {
            return false;
        }
        v = (v * 10) + (c - '0');
    }
    if (std::cmp_greater(v, UINT32_MAX)) {
        return false;
    }
    out = v;
    return true;
}

/// Fraction digits after the '.': first 5 kept (scaled to 1e-5), the 6th decides rounding.
bool parse_fraction(std::string_view digits, std::int64_t& frac, bool& round_up) noexcept {
    if (digits.empty()) {
        return false; // "12." is almost certainly a typo
    }
    frac = 0;
    round_up = false;
    std::size_t kept = 0;
    for (const char c : digits) {
        if (!is_digit(c)) {
            return false;
        }
        if (kept < tuning::kDegreeFractionDigits) {
            frac = (frac * 10) + (c - '0');
            ++kept;
        } else if (kept == tuning::kDegreeFractionDigits && !round_up) {
            round_up = c >= '5';
            ++kept;
        }
    }
    // Scale a short fraction ("5" -> 50000).
    for (std::size_t i = std::min(kept, tuning::kDegreeFractionDigits);
         i < tuning::kDegreeFractionDigits;
         ++i) {
        frac *= 10;
    }
    return true;
}

/// "[+-]D{1,3}[.D+]" -> 1e-5 degrees, rounding half away from zero on the 6th fraction digit.
bool parse_degrees(std::string_view text, std::int64_t& out) noexcept {
    bool negative = false;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole_text = text.substr(0, dot);
    if (whole_text.empty() || whole_text.size() > tuning::kDegreeIntegerDigitsMax) {
        return false;
    }
    std::int64_t whole = 0;
    if (!parse_uint(whole_text, whole)) {
        return false;
    }
    std::int64_t frac = 0;
    bool round_up = false;
    if (dot != std::string_view::npos && !parse_fraction(text.substr(dot + 1), frac, round_up)) {
        return false;
    }
    const std::int64_t magnitude = (whole * tuning::kDegreeScale) + frac + (round_up ? 1 : 0);
    out = negative ? -magnitude : magnitude;
    return true;
}

bool parse_bool(std::string_view text, std::int64_t& out) noexcept {
    constexpr std::array<std::string_view, 4> kTrue{"on", "1", "true", "yes"};
    constexpr std::array<std::string_view, 4> kFalse{"off", "0", "false", "no"};
    for (const auto t : kTrue) {
        if (iequals(text, t)) {
            out = 1;
            return true;
        }
    }
    for (const auto f : kFalse) {
        if (iequals(text, f)) {
            out = 0;
            return true;
        }
    }
    return false;
}

bool parse_enum(const KeyInfo& ki, std::string_view text, std::int64_t& out) noexcept {
    for (std::size_t i = 0; i < ki.choices.size(); ++i) {
        if (iequals(text, ki.choices[i])) {
            out = static_cast<std::int64_t>(i);
            return true;
        }
    }
    return false;
}

/// Writes into a small stack buffer, then copies atomically (nothing is written if it won't fit).
class Formatter {
public:
    void put(char c) noexcept {
        if (len_ < buf_.size()) {
            buf_[len_] = c;
        }
        ++len_;
    }
    void put(std::string_view s) noexcept {
        for (const char c : s) {
            put(c);
        }
    }
    void put_uint(std::uint64_t v, std::size_t min_digits) noexcept {
        std::array<char, 20> tmp{};
        std::size_t n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        for (std::size_t pad = n; pad < min_digits; ++pad) {
            put('0');
        }
        while (n > 0) {
            put(tmp[--n]);
        }
    }
    [[nodiscard]] std::size_t flush(std::span<char> out) const noexcept {
        if (len_ > buf_.size() || len_ > out.size()) {
            return 0;
        }
        for (std::size_t i = 0; i < len_; ++i) {
            out[i] = buf_[i];
        }
        return len_;
    }

private:
    std::array<char, 48> buf_{};
    std::size_t len_ = 0;
};

bool uint_choice_allowed(const KeyInfo& ki, std::int64_t value) noexcept {
    if (ki.choices.empty()) {
        return true;
    }
    for (const auto choice : ki.choices) {
        std::int64_t c = 0;
        if (parse_uint(choice, c) && c == value) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---- public schema accessors ----

std::span<const KeyInfo> schema() noexcept {
    return kSchema;
}

const KeyInfo* find_key(std::string_view name) noexcept {
    for (const KeyInfo& ki : kSchema) {
        if (ki.name == name) {
            return &ki;
        }
    }
    return nullptr;
}

const KeyInfo& info(Key key) noexcept {
    QZ_ASSERT(static_cast<std::size_t>(key) < kKeyCount);
    return kSchema[static_cast<std::size_t>(key)];
}

Settings defaults() noexcept {
    return Settings{}; // first-boot defaults are the member initializers (OPEN_QUESTIONS Q-03)
}

namespace detail {

bool is_numeric_key(Key key) noexcept {
    return key != Key::kTimeZone && key != Key::kCount;
}

std::int64_t to_number(const Settings& s, Key key) noexcept {
    switch (key) {
        case Key::kHourFormat:
            return static_cast<std::int64_t>(s.hour_format);
        case Key::kTempUnit:
            return static_cast<std::int64_t>(s.temp_unit);
        case Key::kConnectivity:
            return static_cast<std::int64_t>(s.connectivity);
        case Key::kWeatherHighLow:
            return s.weather_high_low ? 1 : 0;
        case Key::kLatitude:
            return s.location.lat_e5;
        case Key::kLongitude:
            return s.location.lon_e5;
        case Key::kSyncIntervalH:
            return s.sync_interval_h;
        case Key::kWeatherIntervalMin:
            return s.weather_interval_min;
        case Key::kStepGoal:
            return s.step_goal;
        case Key::kVibration:
            return s.vibration ? 1 : 0;
        case Key::kFace:
            return s.face_id;
        case Key::kTapWake:
            return s.tap_wake ? 1 : 0;
        case Key::kPhoneSync:
            return s.phone_sync ? 1 : 0;
        case Key::kTimeZone:
        case Key::kCount:
            break;
    }
    QZ_ASSERT(false && "to_number on a non-numeric key");
    return 0;
}

Status check_number(const KeyInfo& ki, std::int64_t value) noexcept {
    if (value < ki.min || value > ki.max) {
        return Errc::kBadArgs;
    }
    if (ki.type == ValueType::kUInt) {
        if (!uint_choice_allowed(ki, value)) {
            return Errc::kBadArgs;
        }
        if (ki.key == Key::kStepGoal && value % tuning::kStepGoalMultiple != 0) {
            return Errc::kBadArgs;
        }
    }
    return ok();
}

Status apply_number(Settings& s, Key key, std::int64_t value) noexcept {
    if (!is_numeric_key(key)) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(check_number(info(key), value));
    switch (key) {
        case Key::kHourFormat:
            s.hour_format = static_cast<model::HourFormat>(value);
            break;
        case Key::kTempUnit:
            s.temp_unit = static_cast<model::TempUnit>(value);
            break;
        case Key::kConnectivity:
            s.connectivity = static_cast<model::ConnectivityMode>(value);
            break;
        case Key::kWeatherHighLow:
            s.weather_high_low = value != 0;
            break;
        case Key::kLatitude:
            s.location.lat_e5 = static_cast<std::int32_t>(value);
            s.location_set = true;
            break;
        case Key::kLongitude:
            s.location.lon_e5 = static_cast<std::int32_t>(value);
            s.location_set = true;
            break;
        case Key::kSyncIntervalH:
            s.sync_interval_h = static_cast<std::uint16_t>(value);
            break;
        case Key::kWeatherIntervalMin:
            s.weather_interval_min = static_cast<std::uint16_t>(value);
            break;
        case Key::kStepGoal:
            s.step_goal = static_cast<std::uint32_t>(value);
            break;
        case Key::kVibration:
            s.vibration = value != 0;
            break;
        case Key::kFace:
            s.face_id = static_cast<std::uint8_t>(value);
            break;
        case Key::kTapWake:
            s.tap_wake = value != 0;
            break;
        case Key::kPhoneSync:
            s.phone_sync = value != 0;
            break;
        case Key::kTimeZone:
        case Key::kCount:
            return Errc::kBadArgs;
    }
    return ok();
}

Status apply_zone_name(Settings& s, std::string_view name) noexcept {
    const time::TzEntry* zone = time::find_zone(name);
    if (zone == nullptr) {
        return Errc::kBadArgs;
    }
    FixedString<40> new_name;
    FixedString<64> new_posix;
    if (!new_name.assign(zone->name) || !new_posix.assign(zone->posix)) {
        return Errc::kInternal; // built-in table entry exceeds the stored field size
    }
    s.tz_name = new_name;
    s.tz_posix = new_posix;
    return ok();
}

} // namespace detail

// ---- validation, parsing, formatting ----

Status validate(const Settings& s, bool (*face_ok)(std::uint8_t id)) noexcept {
    for (const KeyInfo& ki : kSchema) {
        if (!detail::is_numeric_key(ki.key)) {
            continue;
        }
        if (!s.location_set && (ki.key == Key::kLatitude || ki.key == Key::kLongitude)) {
            continue; // location is optional; unset coordinates are not checked
        }
        QZ_RETURN_IF_ERROR(detail::check_number(ki, detail::to_number(s, ki.key)));
    }
    // A zone is valid if it is in the built-in list, or (a name removed in a later tzdata
    // release) its stored POSIX fallback still parses.
    if (s.tz_name.empty()) {
        return Errc::kBadArgs;
    }
    if (time::find_zone(s.tz_name.view()) == nullptr && !time::TimeZone::parse(s.tz_posix.view())) {
        return Errc::kBadArgs;
    }
    if (face_ok != nullptr && !face_ok(s.face_id)) {
        return Errc::kBadArgs;
    }
    return ok();
}

Status set_from_string(Settings& s, Key key, std::string_view value) noexcept {
    if (static_cast<std::size_t>(key) >= kKeyCount) {
        return Errc::kBadArgs;
    }
    const KeyInfo& ki = info(key);
    Settings next = s;
    std::int64_t number = 0;
    switch (ki.type) {
        case ValueType::kZoneName:
            QZ_RETURN_IF_ERROR(detail::apply_zone_name(next, value));
            s = next;
            return ok();
        case ValueType::kBool:
            if (!parse_bool(value, number)) {
                return Errc::kBadArgs;
            }
            break;
        case ValueType::kEnum:
            if (!parse_enum(ki, value, number)) {
                return Errc::kBadArgs;
            }
            break;
        case ValueType::kUInt:
            if (!parse_uint(value, number)) {
                return Errc::kBadArgs;
            }
            break;
        case ValueType::kDegrees:
            if (iequals(value, "unset")) {
                next.location_set = false;
                next.location = {};
                s = next;
                return ok();
            }
            if (!parse_degrees(value, number)) {
                return Errc::kBadArgs;
            }
            break;
    }
    QZ_RETURN_IF_ERROR(detail::apply_number(next, key, number));
    s = next;
    return ok();
}

std::size_t format_value(const Settings& s, Key key, std::span<char> out) noexcept {
    if (static_cast<std::size_t>(key) >= kKeyCount) {
        return 0;
    }
    const KeyInfo& ki = info(key);
    Formatter f;
    switch (ki.type) {
        case ValueType::kZoneName:
            f.put(s.tz_name.view());
            break;
        case ValueType::kBool:
        case ValueType::kEnum: {
            const auto index = static_cast<std::size_t>(detail::to_number(s, key));
            if (index >= ki.choices.size()) {
                return 0;
            }
            f.put(ki.choices[index]);
            break;
        }
        case ValueType::kUInt:
            f.put_uint(static_cast<std::uint64_t>(detail::to_number(s, key)), 0);
            break;
        case ValueType::kDegrees: {
            if (!s.location_set) {
                f.put("unset");
                break;
            }
            const std::int64_t v = detail::to_number(s, key);
            const std::uint64_t magnitude =
                v < 0 ? static_cast<std::uint64_t>(-v) : static_cast<std::uint64_t>(v);
            if (v < 0) {
                f.put('-');
            }
            f.put_uint(magnitude / tuning::kDegreeScale, 0);
            f.put('.');
            f.put_uint(magnitude % tuning::kDegreeScale, tuning::kDegreeFractionDigits);
            break;
        }
    }
    return f.flush(out);
}

} // namespace qz::settings
