// 1-bpp 200x200 framebuffer and drawing (integer-only: bit-exact on host and target).
#pragma once

#include "qz/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::gfx {

inline constexpr std::int16_t kWidth = 200;
inline constexpr std::int16_t kHeight = 200;
inline constexpr std::size_t kStride = 25;                 ///< bytes per row
inline constexpr std::size_t kFrameBytes = kStride * 200U; ///< 5000

enum class Color : std::uint8_t { kWhite = 0, kBlack = 1 };

/// Row-major, MSB = leftmost pixel, bit 1 = black ink. (The SSD1681 driver converts to the
/// controller's RAM polarity.) Trivially copyable; 5000 bytes.
struct Framebuffer {
    std::array<std::uint8_t, kFrameBytes> bits{};

    void clear(Color c = Color::kWhite) noexcept;
    [[nodiscard]] Color get(std::int16_t x,
                            std::int16_t y) const noexcept;     ///< out of range = white
    void set(std::int16_t x, std::int16_t y, Color c) noexcept; ///< out of range ignored
    [[nodiscard]] std::uint32_t crc32() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bits; }
};

struct Rect {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::int16_t w = 0;
    std::int16_t h = 0;
};

/// 1-bpp bitmap asset (icons, glyphs): row-major, MSB-first, rows padded to whole bytes.
struct Bitmap {
    std::int16_t width = 0;
    std::int16_t height = 0;
    std::span<const std::uint8_t> bits;
};

/// Proportional bitmap font generated at build time from BDF sources (tools/fontgen.py).
struct Glyph {
    std::uint32_t offset; ///< into Font::bitmap
    std::uint8_t width;
    std::uint8_t height;
    std::int8_t x_offset;
    std::int8_t y_offset; ///< from baseline to the glyph's top row (negative = above)
    std::uint8_t advance;
};

struct Font {
    std::string_view name;
    std::uint8_t line_height;
    std::uint8_t ascent;
    char32_t first; ///< contiguous code point range [first, first + glyphs.size())
    std::span<const Glyph> glyphs;
    std::span<const std::uint8_t> bitmap;
    std::uint8_t fallback_index; ///< glyph used for missing code points
};

/// Fonts compiled into the image (generated). Stable ids, used by faces and screens.
enum class FontId : std::uint8_t { kSmall = 0, kMedium, kLarge, kHuge, kGiant, kCount };
[[nodiscard]] const Font& font(FontId id) noexcept;

enum class Align : std::uint8_t { kLeft, kCenter, kRight };

/// Drawing surface over a framebuffer with a clip rectangle. Not thread-safe. UTF-8 text.
class Canvas {
public:
    explicit Canvas(Framebuffer& fb) noexcept;
    void set_clip(const Rect& r) noexcept;
    void reset_clip() noexcept;
    void clear(Color c = Color::kWhite) noexcept;
    void pixel(std::int16_t x, std::int16_t y, Color c) noexcept;
    void hline(std::int16_t x, std::int16_t y, std::int16_t w, Color c) noexcept;
    void vline(std::int16_t x, std::int16_t y, std::int16_t h, Color c) noexcept;
    void line(std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1, Color c) noexcept;
    void rect(const Rect& r, Color c) noexcept;
    void fill_rect(const Rect& r, Color c) noexcept;
    void
    circle(std::int16_t cx, std::int16_t cy, std::int16_t radius, bool filled, Color c) noexcept;
    /// Draws set bits of `bmp` in color c (clear bits untouched).
    void bitmap(std::int16_t x, std::int16_t y, const Bitmap& bmp, Color c) noexcept;
    /// Draws text with its baseline at y; returns the advance width in pixels.
    std::int16_t
    text(std::int16_t x, std::int16_t y, std::string_view utf8, const Font& f, Color c) noexcept;
    /// Draws text aligned inside [x, x + width) on baseline y.
    void text_aligned(std::int16_t x,
                      std::int16_t y,
                      std::int16_t width,
                      std::string_view utf8,
                      const Font& f,
                      Align a,
                      Color c) noexcept;
    [[nodiscard]] static std::int16_t text_width(std::string_view utf8, const Font& f) noexcept;
    [[nodiscard]] Framebuffer& framebuffer() noexcept { return fb_; }

private:
    Framebuffer& fb_;
    Rect clip_{0, 0, kWidth, kHeight};
};

/// Byte sink for encoders (host file writer, console base64 writer, test buffers).
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual Status write(std::span<const std::uint8_t> bytes) = 0;
};

/// Deterministic PNG (1-bit grayscale, stored/uncompressed deflate blocks, fixed chunk order,
/// no timestamps): identical bytes on every platform. Black ink = 0 (PNG grayscale black).
Status encode_png(const Framebuffer& fb, ByteSink& out) noexcept;

} // namespace qz::gfx
