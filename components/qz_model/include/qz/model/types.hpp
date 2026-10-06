// Shared domain vocabulary (services, UI, console, app). Plain values, trivially copyable.
#pragma once

#include "qz/hal/board_io.hpp"
#include "qz/time/civil.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace qz::model {

// ---- input ----
enum class Button : std::uint8_t { kMenu = 0, kBack = 1, kUp = 2, kDown = 3 };
inline constexpr std::size_t kButtonCount = 4;
static_assert(hal::kButtonBitUp == (1U << static_cast<unsigned>(Button::kUp)));

enum class InputKind : std::uint8_t {
    kClick,  ///< released before the hold threshold
    kHold,   ///< held for the hold threshold (700 ms)
    kRepeat, ///< every 150 ms after kHold while still held (held_ms grows)
};

struct InputEvent {
    Button button = Button::kMenu;
    InputKind kind = InputKind::kClick;
    std::uint32_t held_ms = 0; ///< time since press (kHold/kRepeat)
    std::int64_t t_us = 0;     ///< raw RTC time of the event
};

// ---- settings vocabulary ----
enum class ConnectivityMode : std::uint8_t { kOff = 0, kTimeOnly = 1, kTimeWeather = 2 };
enum class TempUnit : std::uint8_t { kCelsius = 0, kFahrenheit = 1 };
using HourFormat = time::HourFormat;

struct Location {
    std::int32_t lat_e5 = 0; ///< latitude in 1e-5 degrees, -9'000'000..9'000'000
    std::int32_t lon_e5 = 0; ///< longitude in 1e-5 degrees, -18'000'000..18'000'000
    constexpr bool operator==(const Location&) const noexcept = default;
};

// ---- power ----
enum class PowerLevel : std::uint8_t { kNormal = 0, kLow, kSaver, kCritical };

struct BatteryStatus {
    std::uint16_t mv = 0;     ///< filtered battery voltage
    std::uint8_t percent = 0; ///< 0..100, rounded to 5 (meaningless while usb_present)
    PowerLevel level = PowerLevel::kNormal;
    bool usb_present = false;
    bool charging = false;
    bool valid = false; ///< at least one sample taken since cold boot
    bool faked = false; ///< console override active
};

// ---- sync / weather ----
enum class SyncIndicator : std::uint8_t { kNone = 0, kNeverSynced, kLastFailed, kStale, kOk };

enum class WeatherCondition : std::uint8_t {
    kUnknown = 0,
    kClear,
    kPartlyCloudy,
    kCloudy,
    kFog,
    kDrizzle,
    kRain,
    kSnow,
    kShowers,
    kThunder
};
enum class WeatherFreshness : std::uint8_t { kFresh = 0, kStale, kHidden };

struct WeatherReport {
    time::UnixSeconds fetched_utc = 0;
    std::int16_t temp_dc = 0; ///< current temperature, deci-degrees Celsius
    std::int16_t high_dc = 0;
    std::int16_t low_dc = 0;
    WeatherCondition condition = WeatherCondition::kUnknown;
    std::uint8_t has_high_low = 0;
    std::uint8_t valid = 0;
    std::uint8_t faked = 0;
};

// ---- steps ----
struct StepDay {
    time::DayNumber day = 0; ///< local civil day
    std::uint32_t steps = 0;
};
inline constexpr std::size_t kStepHistoryDays = 7;

struct StepsSummary {
    std::uint32_t today = 0;
    std::uint32_t goal = 0;                          ///< 0 = no goal
    std::array<StepDay, kStepHistoryDays> history{}; ///< newest first
    std::uint8_t history_count = 0;
};

// ---- wakes ----
enum class WakeCause : std::uint8_t {
    kColdBoot = 0,
    kReset,
    kTimer,
    kButton,
    kAccel,
    kUsb,
    kTetheredTick,
    kUnknown
};
inline constexpr std::size_t kWakeCauseCount = 8;

/// Per-wake record (ARCHITECTURE.md section 18). 16 bytes, stored in an RTC ring.
struct WakeRecord {
    std::uint32_t start_utc_s = 0; ///< low 32 bits of UTC seconds (raw RTC seconds if time invalid)
    std::uint16_t awake_ms = 0;
    WakeCause cause = WakeCause::kUnknown;
    std::uint8_t flags = 0; ///< WakeFlag bits
    std::uint16_t battery_mv = 0;
    PowerLevel power = PowerLevel::kNormal;
    std::uint8_t error = 0; ///< first error: static_cast<uint8_t>(Errc) + 1, 0 = none
    std::uint16_t steps_delta = 0;
    std::uint16_t reserved = 0;
};
static_assert(sizeof(WakeRecord) == 16);

// NOLINTNEXTLINE(cppcoreguidelines-use-enum-class): bit flags combined with |
enum WakeFlag : std::uint8_t {
    kWakeFlagPartialRefresh = 1U << 0U,
    kWakeFlagFullRefresh = 1U << 1U,
    kWakeFlagRadio = 1U << 2U,
    kWakeFlagInput = 1U << 3U,
    kWakeFlagTimeInvalid = 1U << 4U,
    kWakeFlagError = 1U << 5U,
};

} // namespace qz::model
