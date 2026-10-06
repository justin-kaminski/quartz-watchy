// UI icon set: pixel art and integer primitives evaluated at compile time into 1-bpp bitmaps.
// No heap, no floating point; the baked arrays are plain .rodata.
#include "icons.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace qz::ui::icons {
namespace {

using gfx::Color;
using model::SyncIndicator;
using model::WeatherCondition;

/// Compile-time 1-bpp canvas (row-major, MSB first, rows padded to whole bytes). Coordinates are
/// plain ints; anything outside the sprite is ignored, so art may overhang its box.
template<std::int32_t W, std::int32_t H>
struct Sprite {
    static constexpr std::int32_t kStride = (W + 7) / 8;
    std::array<std::uint8_t, static_cast<std::size_t>(kStride) * static_cast<std::size_t>(H)>
        bits{};

    constexpr void put(std::int32_t x, std::int32_t y, bool on = true) noexcept {
        if (x < 0 || y < 0 || x >= W || y >= H) {
            return;
        }
        const auto idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(kStride)) +
                         static_cast<std::size_t>(x / 8);
        const auto mask = static_cast<std::uint8_t>(0x80U >> static_cast<unsigned>(x % 8));
        bits[idx] = on ? static_cast<std::uint8_t>(bits[idx] | mask)
                       : static_cast<std::uint8_t>(bits[idx] & static_cast<std::uint8_t>(~mask));
    }

    constexpr void
    fill(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, bool on = true) noexcept {
        for (std::int32_t j = 0; j < h; ++j) {
            for (std::int32_t i = 0; i < w; ++i) {
                put(x + i, y + j, on);
            }
        }
    }

    constexpr void
    rect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, bool on = true) noexcept {
        fill(x, y, w, 1, on);
        fill(x, y + h - 1, w, 1, on);
        fill(x, y, 1, h, on);
        fill(x + w - 1, y, 1, h, on);
    }

    /// Disc of radius r (d^2 <= r^2 + r gives the round look of a Bresenham circle).
    constexpr void disc(std::int32_t cx, std::int32_t cy, std::int32_t r, bool on = true) noexcept {
        for (std::int32_t j = -r; j <= r; ++j) {
            for (std::int32_t i = -r; i <= r; ++i) {
                if ((i * i) + (j * j) <= (r * r) + r) {
                    put(cx + i, cy + j, on);
                }
            }
        }
    }

    constexpr void
    ring(std::int32_t cx, std::int32_t cy, std::int32_t r, std::int32_t thickness = 1) noexcept {
        disc(cx, cy, r, true);
        disc(cx, cy, r - thickness, false);
    }

    constexpr void line(std::int32_t x0,
                        std::int32_t y0,
                        std::int32_t x1,
                        std::int32_t y1,
                        bool on = true) noexcept {
        const std::int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
        const std::int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0; // negative
        const std::int32_t sx = x0 < x1 ? 1 : -1;
        const std::int32_t sy = y0 < y1 ? 1 : -1;
        std::int32_t err = dx + dy;
        for (;;) {
            put(x0, y0, on);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const std::int32_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }

    /// Pixel art: '#' sets, 'o' clears, anything else leaves the pixel alone. With `halo`, a one
    /// pixel white border is cleared around every '#' first (art overlapping other art).
    template<std::size_t N>
    constexpr void art(std::int32_t x,
                       std::int32_t y,
                       const std::array<std::string_view, N>& rows,
                       bool halo = false) noexcept {
        if (halo) {
            stamp(x, y, rows, true);
        }
        stamp(x, y, rows, false);
    }

private:
    template<std::size_t N>
    constexpr void stamp(std::int32_t x,
                         std::int32_t y,
                         const std::array<std::string_view, N>& rows,
                         bool halo_pass) noexcept {
        for (std::size_t r = 0; r < N; ++r) {
            const std::string_view row = rows[r];
            for (std::size_t c = 0; c < row.size(); ++c) {
                const auto px = x + static_cast<std::int32_t>(c);
                const auto py = y + static_cast<std::int32_t>(r);
                if (halo_pass) {
                    if (row[c] == '#') {
                        fill(px - 1, py - 1, 3, 3, false);
                    }
                } else if (row[c] == '#') {
                    put(px, py, true);
                } else if (row[c] == 'o') {
                    put(px, py, false);
                }
            }
        }
    }
};

// ---- shared art ----------------------------------------------------------------------------
constexpr std::array<std::string_view, 9> kCloud{
    ".......#####..........",
    "......#######.####....",
    ".....#########.#####..",
    "...##################.",
    "..####################",
    ".#####################",
    ".#####################",
    "..####################",
    "...##################.",
};
constexpr std::int32_t kCloudW = 22;

constexpr std::array<std::string_view, 7> kQuestion{
    ".###.",
    "#...#",
    "....#",
    "..##.",
    "..#..",
    ".....",
    "..#..",
};

constexpr std::array<std::string_view, 5> kArrowHead{
    "#....",
    "###..",
    "#####",
    "###..",
    "#....",
};

template<std::int32_t W, std::int32_t H>
constexpr void sun(Sprite<W, H>& s,
                   std::int32_t cx,
                   std::int32_t cy,
                   std::int32_t r,
                   std::int32_t ray_in,
                   std::int32_t ray_out) noexcept {
    s.disc(cx, cy, r);
    s.line(cx, cy - ray_in, cx, cy - ray_out);
    s.line(cx, cy + ray_in, cx, cy + ray_out);
    s.line(cx - ray_in, cy, cx - ray_out, cy);
    s.line(cx + ray_in, cy, cx + ray_out, cy);
    const std::int32_t di = (ray_in * 7) / 10;
    const std::int32_t dout = (ray_out * 7) / 10;
    s.line(cx + di, cy + di, cx + dout, cy + dout);
    s.line(cx - di, cy + di, cx - dout, cy + dout);
    s.line(cx + di, cy - di, cx + dout, cy - dout);
    s.line(cx - di, cy - di, cx - dout, cy - dout);
}

template<std::int32_t W, std::int32_t H>
constexpr void star(Sprite<W, H>& s, std::int32_t x, std::int32_t y) noexcept {
    s.fill(x - 2, y, 5, 1);
    s.fill(x, y - 2, 1, 5);
    s.put(x - 1, y - 1);
    s.put(x + 1, y - 1);
    s.put(x - 1, y + 1);
    s.put(x + 1, y + 1);
}

// ---- weather (24 x 24) ---------------------------------------------------------------------
using WeatherSprite = Sprite<24, 24>;

constexpr WeatherSprite build_weather(WeatherCondition cond) noexcept {
    WeatherSprite s;
    switch (cond) {
        case WeatherCondition::kClear:
            sun(s, 12, 12, 5, 8, 11);
            break;
        case WeatherCondition::kPartlyCloudy:
            sun(s, 8, 8, 4, 6, 8);
            s.art(2, 10, kCloud, true);
            break;
        case WeatherCondition::kCloudy:
            s.art(0, 3, kCloud);
            s.art(2, 10, kCloud, true);
            break;
        case WeatherCondition::kFog:
            s.fill(3, 4, 18, 2);
            s.fill(6, 9, 15, 2);
            s.fill(3, 14, 18, 2);
            s.fill(7, 19, 13, 2);
            break;
        case WeatherCondition::kDrizzle:
            s.art(1, 3, kCloud);
            for (std::int32_t i = 0; i < 4; ++i) {
                s.fill(4 + (i * 5), 15 + ((i % 2) * 3), 2, 2);
            }
            break;
        case WeatherCondition::kRain:
            s.art(1, 3, kCloud);
            for (std::int32_t x = 6; x <= 16; x += 5) {
                s.line(x + 1, 14, x - 1, 19);
            }
            for (std::int32_t x = 9; x <= 19; x += 5) {
                s.line(x + 1, 18, x - 1, 23);
            }
            break;
        case WeatherCondition::kShowers:
            s.art(1, 3, kCloud);
            for (std::int32_t x = 6; x <= 16; x += 5) {
                s.line(x + 1, 14, x - 1, 19);
                s.line(x + 2, 14, x, 19);
            }
            for (std::int32_t x = 9; x <= 19; x += 5) {
                s.line(x + 1, 18, x - 1, 23);
                s.line(x + 2, 18, x, 23);
            }
            break;
        case WeatherCondition::kSnow:
            s.art(1, 3, kCloud);
            star(s, 6, 16);
            star(s, 12, 16);
            star(s, 18, 16);
            star(s, 9, 21);
            star(s, 15, 21);
            break;
        case WeatherCondition::kThunder:
            s.art(1, 2, kCloud);
            for (std::int32_t k = 0; k < 3; ++k) {
                s.line(13 + k, 12, 9 + k, 18);
            }
            s.fill(9, 18, 7, 1);
            s.line(14, 18, 10, 23);
            s.line(15, 18, 11, 23);
            break;
        case WeatherCondition::kUnknown:
            s.ring(12, 12, 10, 2);
            s.art(10, 8, kQuestion);
            break;
    }
    return s;
}

constexpr std::size_t kWeatherCount = 10; ///< WeatherCondition kUnknown..kThunder

constexpr std::array<WeatherSprite, kWeatherCount> make_weather_table() noexcept {
    std::array<WeatherSprite, kWeatherCount> t{};
    for (std::size_t i = 0; i < kWeatherCount; ++i) {
        t[i] = build_weather(static_cast<WeatherCondition>(i));
    }
    return t;
}
constexpr auto kWeatherTable = make_weather_table();

// ---- sync (16 x 16) ------------------------------------------------------------------------
using SyncSprite = Sprite<16, 16>;

constexpr SyncSprite build_sync(SyncIndicator sync) noexcept {
    SyncSprite s;
    switch (sync) {
        case SyncIndicator::kOk: // check mark in a ring
            s.ring(8, 8, 7, 2);
            s.line(5, 8, 7, 10);
            s.line(5, 9, 7, 11);
            s.line(7, 10, 11, 5);
            s.line(7, 11, 11, 6);
            break;
        case SyncIndicator::kStale: // clock face: the data is old
            s.ring(8, 8, 7, 2);
            s.fill(8, 4, 1, 5);
            s.fill(8, 8, 4, 1);
            break;
        case SyncIndicator::kLastFailed: // cross knocked out of a solid disc
            s.disc(8, 8, 7);
            s.line(5, 5, 11, 11, false);
            s.line(11, 5, 5, 11, false);
            s.line(6, 5, 11, 10, false);
            s.line(10, 5, 5, 10, false);
            break;
        case SyncIndicator::kNeverSynced: // question mark in a ring
            s.ring(8, 8, 7, 2);
            s.art(6, 4, kQuestion);
            break;
        case SyncIndicator::kNone:
            break;
    }
    return s;
}

constexpr std::size_t kSyncCount = 5; ///< SyncIndicator kNone..kOk

constexpr std::array<SyncSprite, kSyncCount> make_sync_table() noexcept {
    std::array<SyncSprite, kSyncCount> t{};
    for (std::size_t i = 0; i < kSyncCount; ++i) {
        t[i] = build_sync(static_cast<SyncIndicator>(i));
    }
    return t;
}
constexpr auto kSyncTable = make_sync_table();

constexpr SyncSprite build_running() noexcept {
    SyncSprite s;
    s.ring(8, 8, 7, 2);
    s.fill(9, 0, 7, 6, false); // gap in the upper right
    s.art(9, 1, kArrowHead);
    return s;
}
constexpr auto kRunningSprite = build_running();

constexpr Sprite<16, 16> build_saver() noexcept {
    Sprite<16, 16> s;
    s.disc(8, 8, 7);
    s.disc(11, 6, 6, false);
    return s;
}
constexpr auto kSaverSprite = build_saver();

constexpr Sprite<24, 24> build_warning() noexcept {
    Sprite<24, 24> s;
    for (std::int32_t k = 0; k < 2; ++k) {
        s.line(12, 2 + k, 1 + k, 21);
        s.line(12, 2 + k, 22 - k, 21);
        s.fill(1, 21 - k, 22, 1);
    }
    s.fill(11, 8, 3, 7);
    s.fill(11, 17, 3, 3);
    return s;
}
constexpr auto kWarningSprite = build_warning();

// ---- battery (26 x 12) ---------------------------------------------------------------------
using BatterySprite = Sprite<26, 12>;

constexpr std::int32_t kSegmentCount = 4;
constexpr std::int32_t kSegmentW = 4;
constexpr std::int32_t kSegmentPitch = 5;

constexpr std::int32_t segments_of(BatteryLevel level) noexcept {
    switch (level) {
        case BatteryLevel::kLow:
            return 1;
        case BatteryLevel::kHalf:
            return 2;
        case BatteryLevel::kHigh:
            return 3;
        case BatteryLevel::kFull:
            return kSegmentCount;
        case BatteryLevel::kUnknown:
        case BatteryLevel::kEmpty:
        case BatteryLevel::kCharging:
            break;
    }
    return 0;
}

constexpr BatterySprite build_battery(BatteryLevel level) noexcept {
    BatterySprite s;
    s.rect(0, 0, 24, 12);
    s.fill(24, 3, 2, 6); // terminal nub
    for (std::int32_t i = 0; i < segments_of(level); ++i) {
        s.fill(2 + (i * kSegmentPitch), 2, kSegmentW, 8);
    }
    if (level == BatteryLevel::kCharging) { // bolt: the percentage is meaningless on USB
        s.line(13, 2, 9, 6);
        s.line(14, 2, 10, 6);
        s.fill(9, 6, 7, 1);
        s.line(15, 6, 11, 10);
        s.line(14, 6, 10, 10);
    } else if (level == BatteryLevel::kUnknown) {
        s.art(10, 2, kQuestion);
    }
    return s;
}

constexpr std::array<BatterySprite, kBatteryLevelCount> make_battery_table() noexcept {
    std::array<BatterySprite, kBatteryLevelCount> t{};
    for (std::size_t i = 0; i < kBatteryLevelCount; ++i) {
        t[i] = build_battery(static_cast<BatteryLevel>(i));
    }
    return t;
}
constexpr auto kBatteryTable = make_battery_table();

template<std::int32_t W, std::int32_t H>
gfx::Bitmap bitmap_of(const Sprite<W, H>& s) noexcept {
    return {static_cast<std::int16_t>(W), static_cast<std::int16_t>(H), s.bits};
}

} // namespace

BatteryLevel battery_level_of(const model::BatteryStatus& b) noexcept {
    constexpr std::uint8_t kLowBelow = 35;
    constexpr std::uint8_t kHalfBelow = 60;
    constexpr std::uint8_t kHighBelow = 85;
    constexpr std::uint8_t kEmptyBelow = 10;
    if (b.usb_present) {
        return BatteryLevel::kCharging;
    }
    if (!b.valid) {
        return BatteryLevel::kUnknown;
    }
    if (b.percent < kEmptyBelow) {
        return BatteryLevel::kEmpty;
    }
    if (b.percent < kLowBelow) {
        return BatteryLevel::kLow;
    }
    if (b.percent < kHalfBelow) {
        return BatteryLevel::kHalf;
    }
    return b.percent < kHighBelow ? BatteryLevel::kHigh : BatteryLevel::kFull;
}

gfx::Bitmap battery(BatteryLevel level) noexcept {
    const auto i = std::min(static_cast<std::size_t>(level), kBatteryLevelCount - 1);
    return bitmap_of(kBatteryTable[i]);
}

gfx::Bitmap sync_state(SyncIndicator sync) noexcept {
    const auto i = static_cast<std::size_t>(sync);
    return bitmap_of(kSyncTable[i < kSyncCount ? i : 0]);
}

gfx::Bitmap sync_running() noexcept {
    return bitmap_of(kRunningSprite);
}

gfx::Bitmap weather(WeatherCondition cond) noexcept {
    const auto i = static_cast<std::size_t>(cond);
    return bitmap_of(kWeatherTable[i < kWeatherCount ? i : 0]);
}

gfx::Bitmap saver() noexcept {
    return bitmap_of(kSaverSprite);
}

gfx::Bitmap warning() noexcept {
    return bitmap_of(kWarningSprite);
}

void draw(gfx::Canvas& canvas,
          std::int16_t x,
          std::int16_t y,
          const gfx::Bitmap& bmp,
          std::int16_t scale,
          Color color) noexcept {
    if (scale <= 1) {
        canvas.bitmap(x, y, bmp, color);
        return;
    }
    const auto stride = static_cast<std::size_t>((bmp.width + 7) / 8);
    if (bmp.width <= 0 || bmp.height <= 0 ||
        bmp.bits.size() < stride * static_cast<std::size_t>(bmp.height)) {
        return;
    }
    for (std::int16_t row = 0; row < bmp.height; ++row) {
        std::int16_t run_start = -1;
        for (std::int16_t col = 0; col <= bmp.width; ++col) {
            bool on = false;
            if (col < bmp.width) {
                const auto idx =
                    (static_cast<std::size_t>(row) * stride) + (static_cast<std::size_t>(col) / 8U);
                on = (bmp.bits[idx] & (0x80U >> (static_cast<unsigned>(col) % 8U))) != 0U;
            }
            if (on && run_start < 0) {
                run_start = col;
            } else if (!on && run_start >= 0) {
                canvas.fill_rect({static_cast<std::int16_t>(x + (run_start * scale)),
                                  static_cast<std::int16_t>(y + (row * scale)),
                                  static_cast<std::int16_t>((col - run_start) * scale),
                                  scale},
                                 color);
                run_start = -1;
            }
        }
    }
}

} // namespace qz::ui::icons
