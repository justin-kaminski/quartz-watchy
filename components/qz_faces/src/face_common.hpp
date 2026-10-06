// Shared drawing and formatting helpers for the watch faces (private to qz_faces).
// Integer-only, heap-free; every function tolerates any WatchState value.
#pragma once

#include "qz/gfx/framebuffer.hpp"
#include "qz/model/types.hpp"
#include "qz/ui/ui.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qz::faces {

/// Tunables for the face layouts (200x200 panel). Geometry of the hand-drawn icons lives next to
/// the drawing code in face_common.cpp.
namespace layout {
inline constexpr std::int16_t kMargin = 6;             ///< left/right inset of content rows
inline constexpr std::int16_t kBarY = 2;               ///< top of the status bar row
inline constexpr std::int16_t kTimeTopY = 22;          ///< default face: top of the time digits
inline constexpr std::int16_t kTimeBaselineY = 82;     ///< default face: baseline of the time
inline constexpr std::int16_t kMinimalBaselineY = 112; ///< minimal face: baseline of the time
inline constexpr std::int16_t kMinTimeDigitPx = 48;    ///< readability rule: digits at least 48 px
inline constexpr std::int16_t kBatteryIconW = 26;
inline constexpr std::int16_t kBatteryIconH = 12;
inline constexpr std::int16_t kSyncIconSize = 15;
inline constexpr std::int16_t kWeatherIconSize = 20;
} // namespace layout

/// Fixed-capacity text builder (no heap). Output that does not fit is truncated.
template<std::size_t N>
class TextBuf {
public:
    void put(std::string_view s) noexcept {
        for (const char ch : s) {
            put_char(ch);
        }
    }
    void put_char(char ch) noexcept {
        if (len_ < N) {
            buf_[len_] = ch;
            ++len_;
        }
    }
    void put_uint(std::uint64_t v) noexcept {
        std::array<char, 20> digits{};
        std::size_t n = 0;
        do {
            digits[n] = static_cast<char>('0' + (v % 10U));
            v /= 10U;
            ++n;
        } while (v != 0U);
        while (n > 0) {
            --n;
            put_char(digits[n]);
        }
    }
    void put_int(std::int64_t v) noexcept {
        if (v < 0) {
            put_char('-');
            // Negate in unsigned arithmetic: well defined for INT64_MIN.
            put_uint(static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(v));
        } else {
            put_uint(static_cast<std::uint64_t>(v));
        }
    }
    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    std::array<char, N> buf_{};
    std::size_t len_ = 0;
};

/// round(num / den) with ties toward +infinity; den > 0. Never yields "-0".
[[nodiscard]] std::int32_t round_div(std::int32_t num, std::int32_t den) noexcept;

/// Whole degrees in the display unit from deci-degrees Celsius (rounded).
[[nodiscard]] std::int32_t temp_whole(std::int16_t temp_dc, model::TempUnit unit) noexcept;

/// "-12" + degree sign + "C" / "F".
[[nodiscard]] TextBuf<16> format_temp(std::int16_t temp_dc, model::TempUnit unit) noexcept;

/// "H15 L3" (display unit, no degree sign).
[[nodiscard]] TextBuf<24> format_high_low(const model::WeatherReport& wx,
                                          model::TempUnit unit) noexcept;

/// "2h old" / "<1h old" / "99h+ old" from an age in seconds.
[[nodiscard]] TextBuf<16> format_age(std::uint32_t age_s) noexcept;

/// "Tue 6 Oct" (abbreviated names); empty fields degrade to "?".
[[nodiscard]] TextBuf<24> format_date(const time::LocalDateTime& local) noexcept;

struct TimeText {
    TextBuf<8> text;
    bool pm = false;            ///< valid only when show_meridiem
    bool show_meridiem = false; ///< 12 h format with a valid time
};
/// "13:07", "1:07" (12 h) or "--:--" when the time is invalid or out of range.
[[nodiscard]] TimeText format_time(const ui::WatchState& s) noexcept;

/// Battery percentage text: "87%", "--%" (no sample), "CHG"/"USB" while on USB power.
[[nodiscard]] TextBuf<8> format_battery(const model::BatteryStatus& b) noexcept;

/// Short power-state tag ("LOW", "SAVER", "CRIT"); empty for kNormal.
[[nodiscard]] std::string_view power_tag(model::PowerLevel level) noexcept;

/// Spelled-out sync state for text-only faces ("synced", "stale", "failed", "never"); empty for
/// kNone.
[[nodiscard]] std::string_view sync_word(model::SyncIndicator sync) noexcept;

// ---- drawing (all black ink on whatever is below; callers clear the canvas) ----

/// 26x12 battery outline with fill, "?"-less: empty body when there is no sample, a bolt on USB.
void draw_battery_icon(gfx::Canvas& c,
                       std::int16_t x,
                       std::int16_t y,
                       const model::BatteryStatus& b) noexcept;
/// 15x15 sync-state glyph (check / clock / cross / question mark). kNone draws nothing.
void draw_sync_icon(gfx::Canvas& c,
                    std::int16_t x,
                    std::int16_t y,
                    model::SyncIndicator sync) noexcept;
/// 20x20 weather condition glyph.
void draw_weather_icon(gfx::Canvas& c,
                       std::int16_t x,
                       std::int16_t y,
                       model::WeatherCondition cond) noexcept;
/// White-on-black label with 2 px padding; returns the width used. Baseline at `baseline_y`.
std::int16_t
draw_tag(gfx::Canvas& c, std::int16_t x, std::int16_t baseline_y, std::string_view text) noexcept;

/// True when the weather block may be shown (valid report, freshness not hidden).
[[nodiscard]] bool weather_visible(const ui::WatchState& s) noexcept;

void render_default_face(const ui::WatchState& state, gfx::Canvas& canvas) noexcept;
void render_minimal_face(const ui::WatchState& state, gfx::Canvas& canvas) noexcept;

} // namespace qz::faces
