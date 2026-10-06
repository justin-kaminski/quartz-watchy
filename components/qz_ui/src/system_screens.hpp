// Render functions of the system screens (ARCHITECTURE.md section 15) and the text formatting
// they share. Private to qz_ui. Everything here is pure: integer-only, no heap, tolerates any
// WatchState value (invalid time, absurd strings, out-of-range enums).
//
// Input behaviour of these screens lives in screen_stubs.cpp (SystemScreen::handle).
#pragma once

#include "icons.hpp"
#include "screen.hpp"

namespace qz::ui {

/// Draws one of: StepsHistory, WeatherDetail, SyncNow, Provisioning, Diagnostics (page index
/// `page`, clamped), About, FactoryReset, ChargeMe, StatusOverlay. Other ids draw nothing.
/// The canvas must be cleared by the caller (Ui::render does).
void render_system_screen(ScreenId id,
                          std::uint8_t page,
                          const WatchState& state,
                          gfx::Canvas& canvas) noexcept;

/// Text helpers (exposed for tests). Outputs are truncated, never overflow.
namespace fmt {

/// "-12" + degree sign + "C"/"F" from deci-degrees Celsius (rounded, never "-0").
[[nodiscard]] TextBuilder<16> temperature(std::int16_t temp_dc, model::TempUnit unit) noexcept;
/// "H15 L3" in the display unit.
[[nodiscard]] TextBuilder<24> high_low(const model::WeatherReport& wx,
                                       model::TempUnit unit) noexcept;
/// "just now" (< 1 min), "5 min ago", "3 h ago", "2 d ago", "99+ d ago".
[[nodiscard]] TextBuilder<20> age(std::uint32_t age_s) noexcept;
/// Decimal with thousands separators: "12,345".
[[nodiscard]] TextBuilder<16> count(std::uint32_t value) noexcept;
/// "14:32" (24 h) or "2:32 PM" (12 h); "--:--" when `time` is out of range.
[[nodiscard]] TextBuilder<12> clock(const time::CivilTime& time, time::HourFormat fmt) noexcept;
/// "6 Oct 14:32" for the local wall time of `utc` under the state's UTC offset and hour format;
/// "never" when `utc` is 0 or negative, "?" when the state's time is not valid.
[[nodiscard]] TextBuilder<24> last_sync(const WatchState& state) noexcept;
/// "4:59" from seconds (minutes may exceed 59).
[[nodiscard]] TextBuilder<12> minutes_seconds(std::uint32_t seconds) noexcept;
/// "12.3 s" / "4.0 min" from milliseconds.
[[nodiscard]] TextBuilder<16> duration_ms(std::uint32_t ms) noexcept;
/// "+12.3 ppm" from parts per billion.
[[nodiscard]] TextBuilder<16> drift(std::int32_t ppb) noexcept;
/// "+05:30" / "-08:00" from seconds east of UTC.
[[nodiscard]] TextBuilder<12> utc_offset(std::int32_t offset_s) noexcept;

[[nodiscard]] std::string_view condition_name(model::WeatherCondition cond) noexcept;
[[nodiscard]] std::string_view power_name(model::PowerLevel level) noexcept;
[[nodiscard]] std::string_view wake_cause_name(model::WakeCause cause) noexcept;
[[nodiscard]] std::string_view connectivity_name(model::ConnectivityMode mode) noexcept;
[[nodiscard]] std::string_view sync_word(model::SyncIndicator sync) noexcept;
/// Short human text for a failed operation ("Timed out", "No Wi-Fi set up", ...).
[[nodiscard]] std::string_view error_text(Errc code) noexcept;

} // namespace fmt
} // namespace qz::ui
