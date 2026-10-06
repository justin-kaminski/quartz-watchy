// Shared pieces of the command catalog (commands_*.cpp). Private to qz_console.
#pragma once

#include "qz/console/registry.hpp"
#include "qz/model/types.hpp"
#include "qz/time/civil.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console::detail {

/// Tunables of the catalog (ARCHITECTURE.md section 2: one constexpr table per component).
struct CatalogTuning {
    /// Hold threshold and first repeat of a synthesized `btn` event: 700 ms hold, 150 ms repeat
    /// period (model::InputKind docs, ARCHITECTURE.md section 15).
    static constexpr std::uint32_t kHoldMs = 700;
    static constexpr std::uint32_t kRepeatMs = 850;
    /// `steps inject` accepts +-this many steps per call. [TUNE] generous for walking tests.
    static constexpr std::int64_t kMaxStepDelta = 1'000'000;
    /// `battery fake` range in millivolts: from below the critical threshold (3400) to above a full
    /// cell. [TUNE]
    static constexpr std::int64_t kFakeMvMin = 2500;
    static constexpr std::int64_t kFakeMvMax = 4600;
    /// Weather temperatures in deci-degrees Celsius (-90.0 .. +60.0). [TUNE]
    static constexpr std::int64_t kTempMinDc = -900;
    static constexpr std::int64_t kTempMaxDc = 600;
    /// `vibrate`: default pulse and the protocol's 1..1000 ms range (ARCHITECTURE.md section 16).
    static constexpr std::int64_t kVibrateDefaultMs = 200;
    static constexpr std::int64_t kVibrateMaxMs = 1000;
    /// `sleep`: 1..3600 s (ARCHITECTURE.md section 16).
    static constexpr std::int64_t kSleepMaxS = 3600;
    /// `log wakes`: default and maximum rows (a row is ~140 bytes of JSON; the line limit is
    /// 16 KiB).
    static constexpr std::int64_t kWakeRowsDefault = 16;
    static constexpr std::int64_t kWakeRowsMax = 64;
    /// Panel geometry of `display dump` [R1]: 200x200, 1 bpp, MSB first.
    static constexpr std::size_t kPanelWidth = 200;
    static constexpr std::size_t kPanelHeight = 200;
    static constexpr std::size_t kFramebufferBytes = kPanelWidth * kPanelHeight / 8U;
};

/// Whole-string decimal integer in [min, max]; kBadArgs otherwise (no sign other than '-', no
/// spaces, no trailing text).
[[nodiscard]] Result<std::int64_t>
parse_integer(std::string_view text, std::int64_t min, std::int64_t max) noexcept;

/// ASCII case-insensitive substring test (empty needle matches).
[[nodiscard]] bool contains_ignore_case(std::string_view haystack,
                                        std::string_view needle) noexcept;

/// kUnsupported when the firmware identity says the radio is compiled out (belt and braces next to
/// the dispatcher's kFlagNeedsRadio gate).
[[nodiscard]] Status require_radio(const DeviceApi& api) noexcept;

// ---- text forms (all write into the caller's buffer and return a view of it) ----
[[nodiscard]] std::string_view format_utc_iso(std::span<char> out, time::UnixSeconds t) noexcept;
/// "YYYY-MM-DDTHH:MM:SS+HH:MM"
[[nodiscard]] std::string_view format_local_iso(std::span<char> out,
                                                const time::LocalDateTime& local) noexcept;
/// "YYYY-MM-DD"
[[nodiscard]] std::string_view format_date(std::span<char> out, time::DayNumber day) noexcept;
/// Eight lowercase hex digits.
[[nodiscard]] std::string_view format_hex32(std::span<char> out, std::uint32_t value) noexcept;
/// Decimal text of `value`.
[[nodiscard]] std::string_view format_decimal(std::span<char> out, std::int64_t value) noexcept;

// ---- vocabulary names ----
[[nodiscard]] std::string_view button_name(model::Button button) noexcept;
[[nodiscard]] Result<model::Button> parse_button(std::string_view name) noexcept;
[[nodiscard]] Result<model::InputKind> parse_input_kind(std::string_view name) noexcept;
[[nodiscard]] std::string_view power_level_name(model::PowerLevel level) noexcept;
[[nodiscard]] std::string_view weather_condition_name(model::WeatherCondition condition) noexcept;
/// A condition name ("partly_cloudy") or its numeric code ("2").
[[nodiscard]] Result<model::WeatherCondition>
parse_weather_condition(std::string_view text) noexcept;
[[nodiscard]] std::string_view freshness_name(model::WeatherFreshness freshness) noexcept;
[[nodiscard]] std::string_view wake_cause_name(model::WakeCause cause) noexcept;
[[nodiscard]] std::string_view sync_indicator_name(model::SyncIndicator indicator) noexcept;
/// Token of a stored "Errc + 1" byte (0 = none -> empty view; unknown values -> "unknown").
[[nodiscard]] std::string_view stored_error_token(std::uint8_t errc_plus_one) noexcept;

// ---- command tables (one per commands_*.cpp) ----
[[nodiscard]] std::span<const Command>
time_commands() noexcept; ///< commands_time.cpp: time, tz, settings
[[nodiscard]] std::span<const Command>
device_commands() noexcept; ///< commands_device.cpp: btn, steps, battery, weather, screen, face,
                            ///< display
[[nodiscard]] std::span<const Command>
radio_commands() noexcept; ///< commands_radio.cpp: wifi, sync, provision
[[nodiscard]] std::span<const Command>
platform_commands() noexcept; ///< commands_platform.cpp: log, diag, selftest, actuators

} // namespace qz::console::detail
