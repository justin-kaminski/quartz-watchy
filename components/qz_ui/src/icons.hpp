// UI icon set (private to qz_ui): 1-bpp bitmaps baked at compile time from in-repo pixel art and
// integer drawing primitives (constexpr), so the data lives in .rodata and nothing is computed at
// run time. qz_ui must not depend on qz_faces; the faces keep their own (drawn) icons.
//
// Every icon is drawn in black ink over whatever is below; clear bits are untouched.
#pragma once

#include "qz/gfx/framebuffer.hpp"
#include "qz/model/types.hpp"

#include <cstdint>

namespace qz::ui::icons {

inline constexpr std::int16_t kWeatherSize = 24; ///< weather glyphs are 24 x 24
inline constexpr std::int16_t kSyncSize = 16;    ///< sync-state glyphs are 16 x 16
inline constexpr std::int16_t kSaverSize = 16;   ///< power-saver glyph is 16 x 16
inline constexpr std::int16_t kBatteryW = 26;    ///< battery glyphs are 26 x 12 (incl. terminal)
inline constexpr std::int16_t kBatteryH = 12;
inline constexpr std::int16_t kWarningSize = 24; ///< warning triangle is 24 x 24

/// Battery glyph state. kFull..kEmpty = 4..0 filled segments.
enum class BatteryLevel : std::uint8_t {
    kUnknown = 0,
    kEmpty,
    kLow,
    kHalf,
    kHigh,
    kFull,
    kCharging
};
inline constexpr std::size_t kBatteryLevelCount = 7;

/// Glyph for a battery status: USB present -> kCharging (the percentage is meaningless then);
/// no sample -> kUnknown; otherwise by percent (< 10 empty, < 35 low, < 60 half, < 85 high).
[[nodiscard]] BatteryLevel battery_level_of(const model::BatteryStatus& b) noexcept;

[[nodiscard]] gfx::Bitmap battery(BatteryLevel level) noexcept;
/// kNone yields a blank (all clear) bitmap of the same size.
[[nodiscard]] gfx::Bitmap sync_state(model::SyncIndicator sync) noexcept;
/// Circular arrows: a sync in progress (not a SyncIndicator state).
[[nodiscard]] gfx::Bitmap sync_running() noexcept;
[[nodiscard]] gfx::Bitmap weather(model::WeatherCondition cond) noexcept;
/// Crescent moon: power-saver / sleeping.
[[nodiscard]] gfx::Bitmap saver() noexcept;
/// Triangle with an exclamation mark.
[[nodiscard]] gfx::Bitmap warning() noexcept;

/// Draws `bmp` with its top-left at (x, y), every source pixel enlarged to scale x scale.
/// scale 1 equals Canvas::bitmap. Pixels outside the canvas are clipped by the canvas.
void draw(gfx::Canvas& canvas,
          std::int16_t x,
          std::int16_t y,
          const gfx::Bitmap& bmp,
          std::int16_t scale = 1,
          gfx::Color color = gfx::Color::kBlack) noexcept;

} // namespace qz::ui::icons
