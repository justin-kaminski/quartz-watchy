// Canvas: clipped, integer-only drawing on the 1-bpp framebuffer (framebuffer.hpp).
//
// Every shape reduces to three clipped operations - a horizontal span (byte-wise masks), a vertical
// span and a single pixel - so the clip test exists once per operation. Coordinates are widened to
// int32 on entry: the public API is int16, so x + w, cx + r, pen + glyph offset, ... cannot
// overflow. Integer arithmetic only, no exceptions, no heap.
//
// Semantics the header leaves open (all covered by tests):
//  - set_clip() REPLACES the clip (it does not intersect with the previous one) and is itself
//    intersected with the frame; an empty or inverted rectangle clips everything.
//  - Every operation, including clear(), is confined to the clip. clear() therefore fills the clip
//    rectangle, so a partial redraw (set_clip, then render a whole screen) leaves the rest alone.
//  - A Bitmap whose storage is smaller than ceil(width / 8) * height, and a glyph whose bitmap
//    data lies outside Font::bitmap, draw nothing (fail safe: a bad asset is a missing icon, not a
//    reboot loop). Padding bits at the end of each bitmap row are ignored.
//  - line() draws the same pixels whichever endpoint comes first (symmetric Bresenham).
//  - circle() uses the midpoint algorithm: the outline pixel of each row/column is the integer
//    nearest to the true circle; the filled disc is the outline's per-row hull.
//  - Text is UTF-8. Malformed sequences yield one U+FFFD per maximal invalid subpart (WHATWG
//    decoder). Code points outside the font's range use Font::fallback_index; if that index is
//    invalid too the code point is skipped (zero advance, nothing drawn).
//  - text_aligned(): when the text is wider than the box it starts at the box's left edge and
//    overflows to the right (only the canvas clip applies).
//  - text()/text_width() return the pen advance saturated at INT16_MAX.
#include "qz/gfx/framebuffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace qz::gfx {
namespace {

using Coord = std::int32_t;

constexpr std::uint8_t kInkByte = 0xFF;   // eight black pixels
constexpr std::uint8_t kPaperByte = 0x00; // eight white pixels
constexpr char32_t kReplacement = 0xFFFD; // U+FFFD, substituted for malformed UTF-8
/// The text pen is capped far beyond any visible position (an int16 origin plus this is still
/// off-screen), so arbitrarily long strings cannot overflow int32.
constexpr Coord kPenLimit = 1 << 24;
constexpr Coord kMaxTextWidth = std::numeric_limits<std::int16_t>::max();

/// Drawing target: the frame plus the clip as half-open bounds [x0, x1) x [y0, y1), always inside
/// the frame (Canvas::set_clip normalizes). An empty clip has x0 >= x1 or y0 >= y1.
struct Surface {
    Framebuffer& fb;
    Coord x0;
    Coord y0;
    Coord x1;
    Coord y1;
};

[[nodiscard]] Surface surface_of(Framebuffer& fb, const Rect& clip) noexcept {
    return Surface{fb, clip.x, clip.y, clip.x + clip.w, clip.y + clip.h};
}

[[nodiscard]] bool clip_empty(const Surface& s) noexcept {
    return s.x0 >= s.x1 || s.y0 >= s.y1;
}

[[nodiscard]] std::int16_t to_i16(Coord v) noexcept {
    return static_cast<std::int16_t>(std::clamp<Coord>(
        v, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
}

// --- Unchecked painting (callers guarantee the preconditions) ------------------------------------

void paint(std::uint8_t& byte, std::uint8_t mask, Color c) noexcept {
    if (c == Color::kBlack) {
        byte = static_cast<std::uint8_t>(byte | mask);
    } else {
        byte = static_cast<std::uint8_t>(byte & static_cast<std::uint8_t>(~mask));
    }
}

/// Pixels [xa, xb) of row y. Preconditions: 0 <= y < kHeight, 0 <= xa < xb <= kWidth.
void paint_row(Framebuffer& fb, Coord y, Coord xa, Coord xb, Color c) noexcept {
    const std::span<std::uint8_t> row =
        std::span<std::uint8_t>{fb.bits}.subspan(static_cast<std::size_t>(y) * kStride, kStride);
    const auto first = static_cast<std::size_t>(xa >> 3);
    const auto last = static_cast<std::size_t>((xb - 1) >> 3);
    // MSB = leftmost pixel: the head byte keeps the bits from (xa mod 8) rightwards, the tail
    // byte the bits up to (xb - 1) mod 8.
    const auto head = static_cast<std::uint8_t>(0xFFU >> (xa & 7));
    const auto tail = static_cast<std::uint8_t>(0xFFU << (7 - ((xb - 1) & 7)));
    if (first == last) {
        paint(row[first], static_cast<std::uint8_t>(head & tail), c);
        return;
    }
    paint(row[first], head, c);
    std::ranges::fill(row.subspan(first + 1, last - first - 1),
                      c == Color::kBlack ? kInkByte : kPaperByte);
    paint(row[last], tail, c);
}

/// Pixels [ya, yb) of column x. Preconditions: 0 <= x < kWidth, 0 <= ya < yb <= kHeight.
void paint_column(Framebuffer& fb, Coord x, Coord ya, Coord yb, Color c) noexcept {
    const auto mask = static_cast<std::uint8_t>(0x80U >> (x & 7));
    std::size_t index = (static_cast<std::size_t>(ya) * kStride) + static_cast<std::size_t>(x >> 3);
    for (Coord y = ya; y < yb; ++y) {
        paint(fb.bits[index], mask, c);
        index += kStride;
    }
}

// --- Clipped primitives
// ---------------------------------------------------------------------------

void plot(const Surface& s, Coord x, Coord y, Color c) noexcept {
    if (x >= s.x0 && x < s.x1 && y >= s.y0 && y < s.y1) {
        s.fb.set(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), c);
    }
}

/// w pixels starting at (x, y), going right.
void hspan(const Surface& s, Coord x, Coord y, Coord w, Color c) noexcept {
    if (w <= 0 || y < s.y0 || y >= s.y1) {
        return;
    }
    const Coord xa = std::max(x, s.x0);
    const Coord xb = std::min(x + w, s.x1);
    if (xa < xb) {
        paint_row(s.fb, y, xa, xb, c);
    }
}

/// h pixels starting at (x, y), going down.
void vspan(const Surface& s, Coord x, Coord y, Coord h, Color c) noexcept {
    if (h <= 0 || x < s.x0 || x >= s.x1) {
        return;
    }
    const Coord ya = std::max(y, s.y0);
    const Coord yb = std::min(y + h, s.y1);
    if (ya < yb) {
        paint_column(s.fb, x, ya, yb, c);
    }
}

void fill_box(const Surface& s, Coord x, Coord y, Coord w, Coord h, Color c) noexcept {
    if (w <= 0 || h <= 0) {
        return;
    }
    const Coord xa = std::max(x, s.x0);
    const Coord xb = std::min(x + w, s.x1);
    const Coord ya = std::max(y, s.y0);
    const Coord yb = std::min(y + h, s.y1);
    if (xa >= xb || ya >= yb) {
        return;
    }
    for (Coord row = ya; row < yb; ++row) {
        paint_row(s.fb, row, xa, xb, c);
    }
}

void outline_box(const Surface& s, Coord x, Coord y, Coord w, Coord h, Color c) noexcept {
    if (w <= 0 || h <= 0) {
        return;
    }
    hspan(s, x, y, w, c);
    if (h > 1) {
        hspan(s, x, y + h - 1, w, c);
    }
    if (h > 2) {
        vspan(s, x, y + 1, h - 2, c);
        if (w > 1) {
            vspan(s, x + w - 1, y + 1, h - 2, c);
        }
    }
}

/// Bresenham along the major axis a, from (a0, b0) to (a1, b1) with a0 <= a1 and
/// a1 - a0 >= |b1 - b0|. `steep` swaps the roles of x and y when plotting. Error ties keep b.
void walk_line(
    const Surface& s, Coord a0, Coord b0, Coord a1, Coord b1, bool steep, Color c) noexcept {
    const Coord da = a1 - a0;
    const Coord db = std::abs(b1 - b0);
    const Coord step = b1 >= b0 ? 1 : -1;
    Coord err = (2 * db) - da;
    Coord b = b0;
    for (Coord a = a0; a <= a1; ++a) {
        if (steep) {
            plot(s, b, a, c);
        } else {
            plot(s, a, b, c);
        }
        if (err > 0) {
            b += step;
            err -= 2 * da;
        }
        err += 2 * db;
    }
}

void draw_line(const Surface& s, Coord x0, Coord y0, Coord x1, Coord y1, Color c) noexcept {
    if (clip_empty(s)) {
        return;
    }
    // Both endpoints beyond the same clip edge: nothing can be visible (exact, cheap reject).
    if ((x0 < s.x0 && x1 < s.x0) || (x0 >= s.x1 && x1 >= s.x1) || (y0 < s.y0 && y1 < s.y0) ||
        (y0 >= s.y1 && y1 >= s.y1)) {
        return;
    }
    if (y0 == y1) {
        hspan(s, std::min(x0, x1), y0, std::abs(x1 - x0) + 1, c);
        return;
    }
    if (x0 == x1) {
        vspan(s, x0, std::min(y0, y1), std::abs(y1 - y0) + 1, c);
        return;
    }
    // Normalize the direction so that line(a, b) and line(b, a) rasterize identically.
    if (std::abs(x1 - x0) >= std::abs(y1 - y0)) {
        if (x0 > x1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        walk_line(s, x0, y0, x1, y1, false, c);
    } else {
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
        }
        walk_line(s, y0, x0, y1, x1, true, c);
    }
}

/// Rows cy + dy and cy - dy (once when dy == 0), each spanning [cx - half, cx + half].
void fill_row_pair(const Surface& s, Coord cx, Coord cy, Coord dy, Coord half, Color c) noexcept {
    hspan(s, cx - half, cy + dy, (2 * half) + 1, c);
    if (dy != 0) {
        hspan(s, cx - half, cy - dy, (2 * half) + 1, c);
    }
}

void plot_octants(const Surface& s, Coord cx, Coord cy, Coord x, Coord y, Color c) noexcept {
    plot(s, cx + x, cy + y, c);
    plot(s, cx - x, cy + y, c);
    plot(s, cx + x, cy - y, c);
    plot(s, cx - x, cy - y, c);
    plot(s, cx + y, cy + x, c);
    plot(s, cx - y, cy + x, c);
    plot(s, cx + y, cy - x, c);
    plot(s, cx - y, cy - x, c);
}

void draw_circle(const Surface& s, Coord cx, Coord cy, Coord r, bool filled, Color c) noexcept {
    if (r < 0 || clip_empty(s) || cx + r < s.x0 || cx - r >= s.x1 || cy + r < s.y0 ||
        cy - r >= s.y1) {
        return;
    }
    // Midpoint circle. err = (x - 1/2)^2 + (y + 1)^2 - r^2 - 1/4 for the candidate row y + 1:
    // x steps inwards exactly when that midpoint lies outside the circle.
    Coord x = r;
    Coord y = 0;
    Coord err = 1 - r;
    while (x >= y) {
        if (filled) {
            fill_row_pair(s, cx, cy, y, x, c);
            if (x != y) {
                fill_row_pair(s, cx, cy, x, y, c);
            }
        } else {
            plot_octants(s, cx, cy, x, y, c);
        }
        ++y;
        if (err < 0) {
            err += (2 * y) + 1;
        } else {
            --x;
            err += (2 * (y - x)) + 1;
        }
    }
}

[[nodiscard]] bool bit_of(std::span<const std::uint8_t> row, Coord col) noexcept {
    const auto byte = static_cast<unsigned>(row[static_cast<std::size_t>(col >> 3)]);
    return ((byte >> (7U - (static_cast<unsigned>(col) & 7U))) & 1U) != 0U;
}

/// Draws the set bits of a w x h bitmap (rows padded to whole bytes) with its top-left at (x, y).
void blit(const Surface& s,
          Coord x,
          Coord y,
          Coord w,
          Coord h,
          std::span<const std::uint8_t> bits,
          Color c) noexcept {
    if (w <= 0 || h <= 0) {
        return;
    }
    const auto stride = static_cast<std::size_t>((w + 7) / 8);
    if (bits.size() < stride * static_cast<std::size_t>(h)) {
        return; // malformed descriptor: draw nothing rather than read out of bounds
    }
    // Visible window in bitmap coordinates.
    const Coord col0 = std::max<Coord>(s.x0 - x, 0);
    const Coord col1 = std::min<Coord>(s.x1 - x, w);
    const Coord row0 = std::max<Coord>(s.y0 - y, 0);
    const Coord row1 = std::min<Coord>(s.y1 - y, h);
    for (Coord row = row0; row < row1; ++row) {
        const auto src = bits.subspan(static_cast<std::size_t>(row) * stride, stride);
        for (Coord col = col0; col < col1; ++col) {
            if (bit_of(src, col)) {
                s.fb.set(static_cast<std::int16_t>(x + col), static_cast<std::int16_t>(y + row), c);
            }
        }
    }
}

// --- UTF-8 and text layout
// ------------------------------------------------------------------------

struct LeadByte {
    unsigned tail = 0;         ///< continuation bytes that follow; 0 = not a valid lead byte
    std::uint32_t payload = 0; ///< payload bits carried by the lead byte
    std::uint8_t lo = 0x80;    ///< range of the FIRST continuation byte: excludes overlong forms,
    std::uint8_t hi = 0xBF;    ///< surrogates and code points above U+10FFFF
};

[[nodiscard]] LeadByte classify_lead(std::uint8_t lead) noexcept {
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return LeadByte{.tail = 1, .payload = lead & 0x1FU, .lo = 0x80, .hi = 0xBF};
    }
    if (lead >= 0xE0U && lead <= 0xEFU) {
        return LeadByte{.tail = 2,
                        .payload = lead & 0x0FU,
                        .lo = static_cast<std::uint8_t>(lead == 0xE0U ? 0xA0U : 0x80U),
                        .hi = static_cast<std::uint8_t>(lead == 0xEDU ? 0x9FU : 0xBFU)};
    }
    if (lead >= 0xF0U && lead <= 0xF4U) {
        return LeadByte{.tail = 3,
                        .payload = lead & 0x07U,
                        .lo = static_cast<std::uint8_t>(lead == 0xF0U ? 0x90U : 0x80U),
                        .hi = static_cast<std::uint8_t>(lead == 0xF4U ? 0x8FU : 0xBFU)};
    }
    return LeadByte{}; // 0x80..0xC1 and 0xF5..0xFF never start a sequence
}

struct Decoded {
    char32_t code_point;
    std::size_t length; ///< bytes consumed, always >= 1
};

/// Decodes the code point at the start of `text` (non-empty). A malformed sequence yields U+FFFD
/// and consumes its maximal valid prefix; the offending byte is left for the next call.
[[nodiscard]] Decoded decode_utf8(std::string_view text) noexcept {
    const auto lead = static_cast<std::uint8_t>(text.front());
    if (lead < 0x80U) {
        return {.code_point = lead, .length = 1};
    }
    const LeadByte info = classify_lead(lead);
    if (info.tail == 0U) {
        return {.code_point = kReplacement, .length = 1};
    }
    std::uint32_t value = info.payload;
    std::uint8_t lo = info.lo;
    std::uint8_t hi = info.hi;
    std::size_t used = 1;
    for (unsigned i = 0; i < info.tail; ++i) {
        if (used >= text.size()) {
            return {.code_point = kReplacement, .length = used}; // truncated at the end
        }
        const auto next = static_cast<std::uint8_t>(text[used]);
        if (next < lo || next > hi) {
            return {.code_point = kReplacement, .length = used};
        }
        value = (value << 6U) | (next & 0x3FU);
        lo = 0x80;
        hi = 0xBF;
        ++used;
    }
    return {.code_point = static_cast<char32_t>(value), .length = used};
}

[[nodiscard]] const Glyph* find_glyph(const Font& f, char32_t code_point) noexcept {
    if (code_point >= f.first) {
        const std::size_t index = code_point - f.first;
        if (index < f.glyphs.size()) {
            return &f.glyphs[index];
        }
    }
    if (f.fallback_index < f.glyphs.size()) {
        return &f.glyphs[f.fallback_index];
    }
    return nullptr;
}

/// Walks the text, calling on_glyph(glyph, pen_x) with the pen offset from the origin; returns the
/// total advance (capped at kPenLimit).
template<class OnGlyph>
[[nodiscard]] Coord layout(std::string_view utf8, const Font& f, const OnGlyph& on_glyph) noexcept {
    Coord pen = 0;
    while (!utf8.empty()) {
        const Decoded d = decode_utf8(utf8);
        utf8.remove_prefix(d.length);
        const Glyph* g = find_glyph(f, d.code_point);
        if (g == nullptr) {
            continue;
        }
        on_glyph(*g, pen);
        pen = std::min<Coord>(pen + g->advance, kPenLimit);
    }
    return pen;
}

void draw_glyph(
    const Surface& s, Coord pen, Coord baseline, const Glyph& g, const Font& f, Color c) noexcept {
    const std::size_t stride = (static_cast<std::size_t>(g.width) + 7U) / 8U;
    const std::size_t size = stride * static_cast<std::size_t>(g.height);
    if (g.width == 0 || g.height == 0 || g.offset > f.bitmap.size() ||
        size > f.bitmap.size() - g.offset) {
        return; // empty glyph (space) or bitmap data outside the font: nothing to draw
    }
    blit(s,
         pen + g.x_offset,
         baseline + g.y_offset,
         g.width,
         g.height,
         f.bitmap.subspan(g.offset, size),
         c);
}

} // namespace

Canvas::Canvas(Framebuffer& fb) noexcept : fb_(fb) {}

void Canvas::set_clip(const Rect& r) noexcept {
    const Coord xa = std::max<Coord>(r.x, 0);
    const Coord ya = std::max<Coord>(r.y, 0);
    const Coord xb = std::min<Coord>(r.x + r.w, kWidth);
    const Coord yb = std::min<Coord>(r.y + r.h, kHeight);
    if (r.w <= 0 || r.h <= 0 || xa >= xb || ya >= yb) {
        clip_ = Rect{}; // empty: everything is clipped
        return;
    }
    clip_ = Rect{.x = static_cast<std::int16_t>(xa),
                 .y = static_cast<std::int16_t>(ya),
                 .w = static_cast<std::int16_t>(xb - xa),
                 .h = static_cast<std::int16_t>(yb - ya)};
}

void Canvas::reset_clip() noexcept {
    clip_ = Rect{.x = 0, .y = 0, .w = kWidth, .h = kHeight};
}

void Canvas::clear(Color c) noexcept {
    const Surface s = surface_of(fb_, clip_);
    fill_box(s, s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0, c);
}

void Canvas::pixel(std::int16_t x, std::int16_t y, Color c) noexcept {
    plot(surface_of(fb_, clip_), x, y, c);
}

void Canvas::hline(std::int16_t x, std::int16_t y, std::int16_t w, Color c) noexcept {
    hspan(surface_of(fb_, clip_), x, y, w, c);
}

void Canvas::vline(std::int16_t x, std::int16_t y, std::int16_t h, Color c) noexcept {
    vspan(surface_of(fb_, clip_), x, y, h, c);
}

void Canvas::line(
    std::int16_t x0, std::int16_t y0, std::int16_t x1, std::int16_t y1, Color c) noexcept {
    draw_line(surface_of(fb_, clip_), x0, y0, x1, y1, c);
}

void Canvas::rect(const Rect& r, Color c) noexcept {
    outline_box(surface_of(fb_, clip_), r.x, r.y, r.w, r.h, c);
}

void Canvas::fill_rect(const Rect& r, Color c) noexcept {
    fill_box(surface_of(fb_, clip_), r.x, r.y, r.w, r.h, c);
}

void Canvas::circle(
    std::int16_t cx, std::int16_t cy, std::int16_t radius, bool filled, Color c) noexcept {
    draw_circle(surface_of(fb_, clip_), cx, cy, radius, filled, c);
}

void Canvas::bitmap(std::int16_t x, std::int16_t y, const Bitmap& bmp, Color c) noexcept {
    blit(surface_of(fb_, clip_), x, y, bmp.width, bmp.height, bmp.bits, c);
}

std::int16_t Canvas::text(
    std::int16_t x, std::int16_t y, std::string_view utf8, const Font& f, Color c) noexcept {
    const Surface s = surface_of(fb_, clip_);
    const Coord advance = layout(
        utf8, f, [&](const Glyph& g, Coord pen) noexcept { draw_glyph(s, x + pen, y, g, f, c); });
    return static_cast<std::int16_t>(std::min(advance, kMaxTextWidth));
}

void Canvas::text_aligned(std::int16_t x,
                          std::int16_t y,
                          std::int16_t width,
                          std::string_view utf8,
                          const Font& f,
                          Align a,
                          Color c) noexcept {
    const Coord slack = static_cast<Coord>(width) - text_width(utf8, f);
    Coord offset = 0;
    if (slack > 0) {
        if (a == Align::kCenter) {
            offset = slack / 2;
        } else if (a == Align::kRight) {
            offset = slack;
        }
    }
    (void)text(to_i16(x + offset), y, utf8, f, c); // the advance is not needed here
}

std::int16_t Canvas::text_width(std::string_view utf8, const Font& f) noexcept {
    const Coord advance = layout(utf8, f, [](const Glyph& /*g*/, Coord /*pen*/) noexcept {});
    return static_cast<std::int16_t>(std::min(advance, kMaxTextWidth));
}

} // namespace qz::gfx
