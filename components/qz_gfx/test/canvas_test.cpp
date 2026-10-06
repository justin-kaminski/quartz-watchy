// Framebuffer and Canvas (framebuffer.hpp).
//
// Strategy: exact pictures for hand-checked shapes; reference-model sweeps that compare the
// byte-mask fast paths against a per-pixel model; a universal property for EVERY primitive
// (drawing with a clip == drawing unclipped, masked to the clip); an independent integer oracle for
// circles; text and UTF-8 decoding against a hand-made font fixture (no generated fonts here);
// extreme int16 coordinates under UBSan; and a zero-allocation check of the whole render path.
#include "qz/core/crc32.hpp"
#include "qz/gfx/framebuffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

std::atomic<std::size_t>& allocation_counter() {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

} // namespace

// Called by the ASan runtime after every allocation (malloc, operator new, ...); a strong
// definition here overrides the runtime's weak empty one (same technique as qz_testkit's
// fake_allocation_test). Defined once per test binary: other qz_gfx tests must not define it.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" void __sanitizer_malloc_hook(const volatile void* /*ptr*/, std::size_t /*size*/) {
    allocation_counter().fetch_add(1, std::memory_order_relaxed);
}

namespace qz::gfx {
namespace {

using ::testing::AssertionFailure;
using ::testing::AssertionResult;
using ::testing::AssertionSuccess;

constexpr int kW = kWidth; // int copies of the frame size for loop arithmetic
constexpr int kH = kHeight;

constexpr std::int16_t i16(int value) noexcept {
    return static_cast<std::int16_t>(value);
}

constexpr Color opposite(Color c) noexcept {
    return c == Color::kBlack ? Color::kWhite : Color::kBlack;
}

std::size_t ink_count(const Framebuffer& fb) {
    std::size_t n = 0;
    for (const std::uint8_t b : fb.bits) {
        n += static_cast<std::size_t>(std::popcount(b));
    }
    return n;
}

bool in_rect(const Rect& r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

std::string describe(const Rect& r) {
    return "{" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," +
           std::to_string(r.h) + "}";
}

struct Bounds {
    int x0 = 1000;
    int y0 = 1000;
    int x1 = -1;
    int y1 = -1;
};

Bounds ink_bounds(const Framebuffer& fb) {
    Bounds b;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (fb.get(i16(x), i16(y)) == Color::kBlack) {
                b.x0 = std::min(b.x0, x);
                b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x);
                b.y1 = std::max(b.y1, y);
            }
        }
    }
    return b;
}

/// Frame region as text, '#' = black, '.' = white, one line per row.
std::string picture(const Framebuffer& fb, int x, int y, int w, int h) {
    std::string out;
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            out += fb.get(i16(x + col), i16(y + row)) == Color::kBlack ? '#' : '.';
        }
        out += '\n';
    }
    return out;
}

std::string art(std::initializer_list<std::string_view> rows) {
    std::string out;
    for (const std::string_view row : rows) {
        out += row;
        out += '\n';
    }
    return out;
}

/// The picture at (x, y) must show exactly `rows` AND the frame must hold no other ink.
AssertionResult
shows(const Framebuffer& fb, int x, int y, std::initializer_list<std::string_view> rows) {
    const std::string expected = art(rows);
    const int w = static_cast<int>(rows.begin()->size());
    const std::string actual = picture(fb, x, y, w, static_cast<int>(rows.size()));
    if (actual != expected) {
        return AssertionFailure() << "picture differs\nexpected:\n"
                                  << expected << "actual:\n"
                                  << actual;
    }
    const auto expected_ink = static_cast<std::size_t>(std::ranges::count(expected, '#'));
    if (ink_count(fb) != expected_ink) {
        return AssertionFailure() << "ink outside the picture: frame has " << ink_count(fb)
                                  << " black pixels, the picture " << expected_ink;
    }
    return AssertionSuccess();
}

std::string first_difference(const Framebuffer& a, const Framebuffer& b) {
    std::size_t count = 0;
    std::string first;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (a.get(i16(x), i16(y)) != b.get(i16(x), i16(y))) {
                if (count == 0) {
                    first = "(" + std::to_string(x) + "," + std::to_string(y) + ")";
                }
                ++count;
            }
        }
    }
    return std::to_string(count) + " pixels differ, first at " + first;
}

AssertionResult frames_equal(const Framebuffer& a, const Framebuffer& b) {
    if (a.bits == b.bits) {
        return AssertionSuccess();
    }
    return AssertionFailure() << first_difference(a, b);
}

/// Runs `draw` on a clipped canvas and `reference` on a plain frame; the frames must be equal.
template<class Draw, class Reference>
AssertionResult agree(const Rect& clip, Color ink, const Draw& draw, const Reference& reference) {
    Framebuffer actual;
    actual.clear(opposite(ink));
    Canvas canvas(actual);
    canvas.set_clip(clip);
    draw(canvas);
    Framebuffer expected;
    expected.clear(opposite(ink));
    reference(expected);
    if (actual.bits == expected.bits) {
        return AssertionSuccess();
    }
    return AssertionFailure() << first_difference(actual, expected);
}

std::vector<int> ints(std::initializer_list<std::pair<int, int>> ranges) {
    std::vector<int> out;
    for (const auto& [lo, hi] : ranges) {
        for (int v = lo; v <= hi; ++v) {
            out.push_back(v);
        }
    }
    return out;
}

constexpr std::array<Rect, 4> kSweepClips = {{
    {.x = 0, .y = 0, .w = 200, .h = 200},
    {.x = 13, .y = 13, .w = 93, .h = 93},
    {.x = 8, .y = 8, .w = 8, .h = 8},
    {.x = -30, .y = -30, .w = 100, .h = 100},
}};

// --- Hand-made font fixture ---------------------------------------------------------------------
// Code points 'A'..'G'. Glyph bitmaps are 1 bpp, rows padded to bytes, MSB first.
//   A 3x5  .#. / #.# / ### / #.# / #.#          advance 4
//   B 3x5  ##. / #.# / ##. / #.# / ##.          advance 4
//   C 2x6  x_offset 1, y_offset -4 (two rows below the baseline)   advance 5
//   D      zero-size (a space)                                     advance 3
//   E 4x4  hollow box, also the fallback glyph                     advance 6
//   F 11x2 two bytes per row                                       advance 12
//   G 3x1  y_offset +1 (entirely below the baseline)               advance 4
constexpr std::array<std::uint8_t, 25> kFixtureBitmap = {
    0x40, 0xA0, 0xE0, 0xA0, 0xA0,       // A
    0xC0, 0xA0, 0xC0, 0xA0, 0xC0,       // B
    0xC0, 0x80, 0x80, 0x80, 0xC0, 0x40, // C
    0xF0, 0x90, 0x90, 0xF0,             // E
    0xFF, 0xE0, 0x80, 0x20,             // F
    0xE0,                               // G
};

constexpr std::array<Glyph, 7> kFixtureGlyphs = {{
    {.offset = 0, .width = 3, .height = 5, .x_offset = 0, .y_offset = -5, .advance = 4},
    {.offset = 5, .width = 3, .height = 5, .x_offset = 0, .y_offset = -5, .advance = 4},
    {.offset = 10, .width = 2, .height = 6, .x_offset = 1, .y_offset = -4, .advance = 5},
    {.offset = 16, .width = 0, .height = 0, .x_offset = 0, .y_offset = 0, .advance = 3},
    {.offset = 16, .width = 4, .height = 4, .x_offset = 0, .y_offset = -4, .advance = 6},
    {.offset = 20, .width = 11, .height = 2, .x_offset = 0, .y_offset = -2, .advance = 12},
    {.offset = 24, .width = 3, .height = 1, .x_offset = 0, .y_offset = 1, .advance = 4},
}};

constexpr Font kFixtureFont{.name = "fixture",
                            .line_height = 8,
                            .ascent = 6,
                            .first = U'A',
                            .glyphs = kFixtureGlyphs,
                            .bitmap = kFixtureBitmap,
                            .fallback_index = 4};

constexpr std::uint8_t kFallbackAdvance = 1;

/// Two single-pixel glyphs: [0] = code point `first` (given advance), [1] = the fallback glyph
/// (advance 1). Code points below `first` (e.g. ASCII) therefore measure as the fallback.
class PairFont {
public:
    PairFont(char32_t first, std::uint8_t target_advance) noexcept
        : glyphs_{{{.offset = 0,
                    .width = 1,
                    .height = 1,
                    .x_offset = 0,
                    .y_offset = -1,
                    .advance = target_advance},
                   {.offset = 1,
                    .width = 1,
                    .height = 1,
                    .x_offset = 0,
                    .y_offset = -1,
                    .advance = kFallbackAdvance}}},
          font_{.name = "pair",
                .line_height = 1,
                .ascent = 1,
                .first = first,
                .glyphs = glyphs_,
                .bitmap = kBitmap,
                .fallback_index = 1} {}
    PairFont(const PairFont&) = delete;
    PairFont& operator=(const PairFont&) = delete;
    PairFont(PairFont&&) = delete;
    PairFont& operator=(PairFont&&) = delete;
    ~PairFont() = default;

    [[nodiscard]] const Font& font() const noexcept { return font_; }

private:
    static constexpr std::array<std::uint8_t, 2> kBitmap = {0x80, 0x80};
    std::array<Glyph, 2> glyphs_;
    Font font_;
};

// --- Shapes that straddle the frame edges and the test clips -------------------------------------

void draw_pixels(Canvas& c, Color ink) {
    for (int y = -10; y < 215; y += 7) {
        for (int x = -10; x < 215; x += 5) {
            c.pixel(i16(x), i16(y), ink);
        }
    }
}

void draw_hlines(Canvas& c, Color ink) {
    c.hline(-20, 100, 260, ink);
    c.hline(30, 5, 400, ink);
    c.hline(60, 60, 41, ink);
    c.hline(-5, 199, 20, ink);
    c.hline(190, 0, 50, ink);
    c.hline(40, 150, 7, ink);
}

void draw_vlines(Canvas& c, Color ink) {
    c.vline(100, -30, 300, ink);
    c.vline(-4, 0, 50, ink);
    c.vline(70, 20, 13, ink);
    c.vline(199, 120, 100, ink);
    c.vline(55, 45, 200, ink);
}

void draw_lines(Canvas& c, Color ink) {
    std::uint32_t state = 0x1234ABCDU;
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        return static_cast<int>(state % 321U) - 60; // -60 .. 260
    };
    for (int i = 0; i < 80; ++i) {
        const int x0 = next();
        const int y0 = next();
        const int x1 = next();
        const int y1 = next();
        c.line(i16(x0), i16(y0), i16(x1), i16(y1), ink);
    }
    c.line(0, 0, 199, 199, ink);
    c.line(199, 0, 0, 199, ink);
    c.line(-50, 100, 250, 100, ink);
    c.line(100, -50, 100, 250, ink);
    c.line(-40, 30, 240, 170, ink);
    c.line(30, 240, 170, -40, ink);
}

void draw_rects(Canvas& c, Color ink) {
    c.rect({.x = -10, .y = -10, .w = 120, .h = 120}, ink);
    c.rect({.x = 150, .y = 150, .w = 100, .h = 100}, ink);
    c.rect({.x = 40, .y = 40, .w = 1, .h = 150}, ink);
    c.rect({.x = 60, .y = 60, .w = 120, .h = 1}, ink);
    c.rect({.x = 45, .y = 45, .w = 110, .h = 110}, ink);
    c.rect({.x = 70, .y = 70, .w = 2, .h = 2}, ink);
}

void draw_fill_rects(Canvas& c, Color ink) {
    c.fill_rect({.x = -10, .y = -10, .w = 70, .h = 70}, ink);
    c.fill_rect({.x = 120, .y = 120, .w = 100, .h = 100}, ink);
    c.fill_rect({.x = 80, .y = 30, .w = 10, .h = 200}, ink);
    c.fill_rect({.x = 20, .y = 90, .w = 300, .h = 5}, ink);
}

void draw_circles(Canvas& c, Color ink) {
    c.circle(100, 100, 60, false, ink);
    c.circle(100, 100, 150, false, ink);
    c.circle(0, 0, 50, true, ink);
    c.circle(199, 199, 40, false, ink);
    c.circle(100, 100, 30, true, ink);
    c.circle(60, 140, 45, false, ink);
    c.circle(140, 60, 45, true, ink);
}

constexpr std::array<std::uint8_t, 39> kBlobBits = [] {
    std::array<std::uint8_t, 39> a{};
    for (std::size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<std::uint8_t>(((i * 157U) + 53U) & 0xFFU);
    }
    return a;
}();
constexpr Bitmap kBlob{.width = 20, .height = 13, .bits = kBlobBits}; // 3 bytes per row

void draw_bitmaps(Canvas& c, Color ink) {
    c.bitmap(-5, -3, kBlob, ink);
    c.bitmap(190, 190, kBlob, ink);
    c.bitmap(95, 95, kBlob, ink);
    c.bitmap(180, -6, kBlob, ink);
    c.bitmap(45, 45, kBlob, ink);
    c.bitmap(60, 100, kBlob, ink);
}

void draw_texts(Canvas& c, Color ink) {
    (void)c.text(-3, 60, "ABCDEFGABCDEFG", kFixtureFont, ink);
    (void)c.text(40, 100, "ABAB", kFixtureFont, ink);
    (void)c.text(150, 195, "FFG", kFixtureFont, ink);
    c.text_aligned(30, 120, 140, "BCA", kFixtureFont, Align::kCenter, ink);
    (void)c.text(190, 20, "AAAA", kFixtureFont, ink);
    (void)c.text(-20, 10, "ABC", kFixtureFont, ink);
    (void)c.text(45, 55, "ECA", kFixtureFont, ink);
}

void draw_clear(Canvas& c, Color ink) {
    c.clear(ink);
}

struct Shape {
    std::string_view name;
    void (*draw)(Canvas&, Color);
};

constexpr std::array<Shape, 10> kShapes = {{
    {.name = "pixel", .draw = draw_pixels},
    {.name = "hline", .draw = draw_hlines},
    {.name = "vline", .draw = draw_vlines},
    {.name = "line", .draw = draw_lines},
    {.name = "rect", .draw = draw_rects},
    {.name = "fill_rect", .draw = draw_fill_rects},
    {.name = "circle", .draw = draw_circles},
    {.name = "bitmap", .draw = draw_bitmaps},
    {.name = "text", .draw = draw_texts},
    {.name = "clear", .draw = draw_clear},
}};

// ===================================================================================================
// Framebuffer
// ===================================================================================================

static_assert(sizeof(Framebuffer) == kFrameBytes);
static_assert(kFrameBytes == 5000U);
static_assert(kStride * 8U == static_cast<std::size_t>(kWidth));
static_assert(std::is_trivially_copyable_v<Framebuffer>);

TEST(Framebuffer, StartsWhiteAndExposesItsBytes) {
    const Framebuffer fb;
    EXPECT_EQ(ink_count(fb), 0U);
    EXPECT_EQ(fb.bytes().size(), kFrameBytes);
    EXPECT_EQ(fb.bytes().data(), fb.bits.data());
}

TEST(Framebuffer, ClearFillsEveryByte) {
    Framebuffer fb;
    fb.clear(Color::kBlack);
    EXPECT_TRUE(std::ranges::all_of(fb.bits, [](std::uint8_t b) { return b == 0xFF; }));
    fb.clear(); // default argument is white
    EXPECT_TRUE(std::ranges::all_of(fb.bits, [](std::uint8_t b) { return b == 0x00; }));
    fb.clear(Color::kBlack);
    fb.clear(Color::kWhite);
    EXPECT_EQ(ink_count(fb), 0U);
}

TEST(Framebuffer, BitLayoutIsRowMajorMsbFirst) {
    struct Probe {
        int x;
        int y;
        std::size_t byte;
        int mask;
    };
    constexpr std::array<Probe, 9> kProbes = {{
        {.x = 0, .y = 0, .byte = 0, .mask = 0x80},
        {.x = 1, .y = 0, .byte = 0, .mask = 0x40},
        {.x = 7, .y = 0, .byte = 0, .mask = 0x01},
        {.x = 8, .y = 0, .byte = 1, .mask = 0x80},
        {.x = 15, .y = 0, .byte = 1, .mask = 0x01},
        {.x = 199, .y = 0, .byte = 24, .mask = 0x01},
        {.x = 0, .y = 1, .byte = 25, .mask = 0x80},
        {.x = 3, .y = 2, .byte = 50, .mask = 0x10},
        {.x = 199, .y = 199, .byte = 4999, .mask = 0x01},
    }};
    for (const Probe& p : kProbes) {
        Framebuffer fb;
        fb.set(i16(p.x), i16(p.y), Color::kBlack);
        EXPECT_EQ(static_cast<int>(fb.bits[p.byte]), p.mask) << "x=" << p.x << " y=" << p.y;
        EXPECT_EQ(ink_count(fb), 1U);
        EXPECT_EQ(fb.get(i16(p.x), i16(p.y)), Color::kBlack);
    }
}

TEST(Framebuffer, SetWhiteClearsOnlyThatPixelAndSetIsIdempotent) {
    Framebuffer fb;
    fb.clear(Color::kBlack);
    fb.set(10, 5, Color::kWhite);
    fb.set(10, 5, Color::kWhite);
    EXPECT_EQ(fb.get(10, 5), Color::kWhite);
    EXPECT_EQ(fb.get(9, 5), Color::kBlack);
    EXPECT_EQ(fb.get(11, 5), Color::kBlack);
    EXPECT_EQ(ink_count(fb), (kFrameBytes * 8U) - 1U);
    fb.set(10, 5, Color::kBlack);
    fb.set(10, 5, Color::kBlack);
    EXPECT_EQ(ink_count(fb), kFrameBytes * 8U);
}

TEST(Framebuffer, OutOfRangeGetIsWhite) {
    Framebuffer fb;
    fb.clear(Color::kBlack); // on a black frame "white" can only come from the range rule
    for (const int bad : {-1, 200, 255, 32767, -32768, 1000}) {
        EXPECT_EQ(fb.get(i16(bad), 0), Color::kWhite) << bad;
        EXPECT_EQ(fb.get(0, i16(bad)), Color::kWhite) << bad;
        EXPECT_EQ(fb.get(i16(bad), i16(bad)), Color::kWhite) << bad;
    }
    EXPECT_EQ(fb.get(199, 199), Color::kBlack); // the last valid pixel is still readable
}

TEST(Framebuffer, OutOfRangeSetIsIgnored) {
    Framebuffer fb;
    fb.clear(Color::kBlack);
    const Framebuffer before = fb;
    for (const int bad : {-1, 200, 255, 32767, -32768, 1000}) {
        fb.set(i16(bad), 0, Color::kWhite);
        fb.set(0, i16(bad), Color::kWhite);
        fb.set(i16(bad), i16(bad), Color::kWhite);
    }
    EXPECT_TRUE(frames_equal(fb, before));
    Framebuffer white;
    white.set(200, 0, Color::kBlack);
    white.set(0, 200, Color::kBlack);
    white.set(-1, -1, Color::kBlack);
    EXPECT_EQ(ink_count(white), 0U);
    white.set(199, 199, Color::kBlack); // the last valid pixel still works
    EXPECT_EQ(white.get(199, 199), Color::kBlack);
}

TEST(Framebuffer, Crc32OfConstantFramesMatchesZlib) {
    Framebuffer fb;
    EXPECT_EQ(fb.crc32(), 0xD8E50EA8U); // zlib.crc32(bytes(5000))
    fb.clear(Color::kBlack);
    EXPECT_EQ(fb.crc32(), 0x338AE894U); // zlib.crc32(b"\xff" * 5000)
    fb.clear();
    EXPECT_EQ(fb.crc32(), qz::crc32(fb.bytes()));
}

TEST(Framebuffer, Crc32DistinguishesSinglePixelFrames) {
    const Framebuffer blank;
    std::vector<std::uint32_t> seen;
    for (const std::pair<int, int>& p :
         {std::pair{0, 0}, {199, 0}, {0, 199}, {199, 199}, {100, 100}}) {
        Framebuffer one;
        one.set(i16(p.first), i16(p.second), Color::kBlack);
        EXPECT_NE(one.crc32(), blank.crc32());
        seen.push_back(one.crc32());
    }
    std::ranges::sort(seen);
    EXPECT_EQ(std::ranges::adjacent_find(seen), seen.end()) << "single-pixel frames collide";
}

// ===================================================================================================
// Canvas: clip handling
// ===================================================================================================

TEST(Canvas, FramebufferAccessorReturnsTheBackingFrame) {
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(&canvas.framebuffer(), &fb);
    canvas.pixel(3, 4, Color::kBlack);
    EXPECT_EQ(fb.get(3, 4), Color::kBlack);
}

TEST(Canvas, DefaultClipIsTheWholeFrame) {
    Framebuffer fb;
    Canvas canvas(fb);
    for (const int x : {0, 199}) {
        for (const int y : {0, 199}) {
            canvas.pixel(i16(x), i16(y), Color::kBlack);
        }
    }
    canvas.pixel(-1, 0, Color::kBlack);
    canvas.pixel(200, 0, Color::kBlack);
    canvas.pixel(0, -1, Color::kBlack);
    canvas.pixel(0, 200, Color::kBlack);
    EXPECT_EQ(ink_count(fb), 4U);
}

TEST(Canvas, PixelHonoursClipAndFrame) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.set_clip({.x = 10, .y = 20, .w = 5, .h = 4});
    for (int y = 10; y < 40; ++y) {
        for (int x = 0; x < 30; ++x) {
            canvas.pixel(i16(x), i16(y), Color::kBlack);
        }
    }
    EXPECT_TRUE(shows(fb, 10, 20, {"#####", "#####", "#####", "#####"}));
}

TEST(Canvas, SetClipIsIntersectedWithTheFrame) {
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.set_clip({.x = -10, .y = -10, .w = 20, .h = 20}); // effective [0,10) x [0,10)
        canvas.fill_rect({.x = -50, .y = -50, .w = 400, .h = 400}, Color::kBlack);
        EXPECT_EQ(ink_count(fb), 100U);
        EXPECT_EQ(fb.get(9, 9), Color::kBlack);
        EXPECT_EQ(fb.get(10, 0), Color::kWhite);
        EXPECT_EQ(fb.get(0, 10), Color::kWhite);
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.set_clip({.x = 190, .y = 190, .w = 100, .h = 100}); // effective [190,200)^2
        canvas.fill_rect({.x = -50, .y = -50, .w = 400, .h = 400}, Color::kBlack);
        EXPECT_EQ(ink_count(fb), 100U);
        EXPECT_EQ(fb.get(190, 190), Color::kBlack);
        EXPECT_EQ(fb.get(189, 190), Color::kWhite);
    }
}

TEST(Canvas, SetClipReplacesThePreviousClip) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.set_clip({.x = 0, .y = 0, .w = 10, .h = 10});
    canvas.set_clip({.x = 100, .y = 100, .w = 10, .h = 10}); // disjoint from the first
    canvas.fill_rect({.x = 0, .y = 0, .w = 200, .h = 200}, Color::kBlack);
    EXPECT_EQ(ink_count(fb), 100U);
    EXPECT_EQ(fb.get(5, 5), Color::kWhite);
    EXPECT_EQ(fb.get(105, 105), Color::kBlack);
}

TEST(Canvas, EmptyOrInvertedClipDrawsNothing) {
    constexpr std::array<Rect, 8> kEmpty = {{
        {.x = 10, .y = 10, .w = 0, .h = 50},
        {.x = 10, .y = 10, .w = 50, .h = 0},
        {.x = 10, .y = 10, .w = -5, .h = 50},
        {.x = 10, .y = 10, .w = 50, .h = -5},
        {.x = 300, .y = 300, .w = 5, .h = 5},
        {.x = -20, .y = -20, .w = 10, .h = 10},
        {.x = 200, .y = 0, .w = 10, .h = 10},
        {.x = 0, .y = 0, .w = 0, .h = 0},
    }};
    for (const Rect& clip : kEmpty) {
        for (const Shape& shape : kShapes) {
            Framebuffer fb;
            Canvas canvas(fb);
            canvas.set_clip(clip);
            shape.draw(canvas, Color::kBlack);
            EXPECT_EQ(ink_count(fb), 0U) << shape.name << " clip " << describe(clip);
        }
    }
}

TEST(Canvas, ResetClipRestoresTheWholeFrame) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.set_clip({.x = 50, .y = 50, .w = 10, .h = 10});
    canvas.fill_rect({.x = 0, .y = 0, .w = 200, .h = 200}, Color::kBlack);
    EXPECT_EQ(ink_count(fb), 100U);
    canvas.reset_clip();
    canvas.fill_rect({.x = 0, .y = 0, .w = 200, .h = 200}, Color::kBlack);
    EXPECT_EQ(ink_count(fb), kFrameBytes * 8U);
    // reset also recovers from an empty clip
    canvas.set_clip({.x = 0, .y = 0, .w = 0, .h = 0});
    canvas.reset_clip();
    canvas.pixel(0, 0, Color::kWhite);
    EXPECT_EQ(fb.get(0, 0), Color::kWhite);
}

TEST(Canvas, ClearFillsOnlyTheClip) {
    Framebuffer fb;
    fb.clear(Color::kBlack);
    Canvas canvas(fb);
    canvas.set_clip({.x = 20, .y = 30, .w = 50, .h = 60});
    canvas.clear(); // default argument is white
    EXPECT_EQ(ink_count(fb), (kFrameBytes * 8U) - (std::size_t{50} * std::size_t{60}));
    EXPECT_EQ(fb.get(20, 30), Color::kWhite);
    EXPECT_EQ(fb.get(69, 89), Color::kWhite);
    EXPECT_EQ(fb.get(70, 89), Color::kBlack);
    EXPECT_EQ(fb.get(69, 90), Color::kBlack);
    canvas.reset_clip();
    canvas.clear(Color::kBlack);
    EXPECT_EQ(ink_count(fb), kFrameBytes * 8U);
    canvas.clear(Color::kWhite);
    EXPECT_EQ(ink_count(fb), 0U);
}

// The universal clipping property, for every primitive: drawing with a clip produces the unclipped
// drawing inside the clip and leaves every pixel outside it alone.
constexpr std::array<Rect, 12> kPropertyClips = {{
    {.x = 0, .y = 0, .w = 200, .h = 200},
    {.x = 50, .y = 50, .w = 100, .h = 100},
    {.x = 0, .y = 0, .w = 1, .h = 1},
    {.x = 199, .y = 199, .w = 1, .h = 1},
    {.x = -30, .y = -30, .w = 100, .h = 100},
    {.x = 100, .y = -50, .w = 300, .h = 120},
    {.x = 10, .y = 10, .w = 0, .h = 50},
    {.x = 10, .y = 10, .w = 50, .h = -5},
    {.x = 500, .y = 500, .w = 10, .h = 10},
    {.x = 75, .y = 0, .w = 1, .h = 200},
    {.x = 0, .y = 120, .w = 200, .h = 1},
    {.x = 13, .y = 29, .w = 37, .h = 41},
}};

AssertionResult
clip_property(const Shape& shape, const Rect& clip, Color ink, const Framebuffer& unclipped) {
    Framebuffer clipped;
    clipped.clear(opposite(ink));
    Canvas canvas(clipped);
    canvas.set_clip(clip);
    shape.draw(canvas, ink);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Color expected =
                in_rect(clip, x, y) ? unclipped.get(i16(x), i16(y)) : opposite(ink);
            if (clipped.get(i16(x), i16(y)) != expected) {
                return AssertionFailure()
                       << shape.name << " clip " << describe(clip) << " ink "
                       << static_cast<int>(ink) << ": pixel (" << x << "," << y << ") differs";
            }
        }
    }
    return AssertionSuccess();
}

TEST(Canvas, ClippedDrawingEqualsUnclippedDrawingMaskedToTheClipForEveryPrimitive) {
    for (const Shape& shape : kShapes) {
        for (const Color ink : {Color::kBlack, Color::kWhite}) {
            Framebuffer unclipped;
            unclipped.clear(opposite(ink));
            Canvas full(unclipped);
            shape.draw(full, ink);
            for (const Rect& clip : kPropertyClips) {
                ASSERT_TRUE(clip_property(shape, clip, ink, unclipped));
            }
        }
    }
}

// Guards the property above against vacuity: every shape really crosses the test clip.
/// {black pixels inside, black pixels outside} the rectangle.
std::pair<std::size_t, std::size_t> ink_split(const Framebuffer& fb, const Rect& r) {
    std::size_t inside = 0;
    std::size_t outside = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (fb.get(i16(x), i16(y)) == Color::kBlack) {
                (in_rect(r, x, y) ? inside : outside) += 1U;
            }
        }
    }
    return {inside, outside};
}

TEST(Canvas, EveryTestShapeCrossesTheClipBoundary) {
    const Rect clip{.x = 50, .y = 50, .w = 100, .h = 100};
    for (const Shape& shape : kShapes) {
        Framebuffer fb;
        Canvas canvas(fb);
        shape.draw(canvas, Color::kBlack);
        const auto [inside, outside] = ink_split(fb, clip);
        EXPECT_GT(inside, 0U) << shape.name;
        EXPECT_GT(outside, 0U) << shape.name;
    }
}

// ===================================================================================================
// Canvas: spans, rectangles
// ===================================================================================================

void ref_hline(Framebuffer& fb, const Rect& clip, int x, int y, int w, Color c) {
    for (int xx = std::max(x, 0); xx < std::min(x + w, kW); ++xx) {
        if (in_rect(clip, xx, y)) {
            fb.set(i16(xx), i16(y), c);
        }
    }
}

void ref_vline(Framebuffer& fb, const Rect& clip, int x, int y, int h, Color c) {
    for (int yy = std::max(y, 0); yy < std::min(y + h, kH); ++yy) {
        if (in_rect(clip, x, yy)) {
            fb.set(i16(x), i16(yy), c);
        }
    }
}

void ref_box(Framebuffer& fb, const Rect& clip, const Rect& r, bool outline_only, Color c) {
    for (int yy = std::max<int>(r.y, 0); yy < std::min<int>(r.y + r.h, kH); ++yy) {
        for (int xx = std::max<int>(r.x, 0); xx < std::min<int>(r.x + r.w, kW); ++xx) {
            const bool border =
                xx == r.x || xx == r.x + r.w - 1 || yy == r.y || yy == r.y + r.h - 1;
            if (in_rect(clip, xx, yy) && (!outline_only || border)) {
                fb.set(i16(xx), i16(yy), c);
            }
        }
    }
}

const std::vector<int>& span_lengths() {
    static const std::vector<int> kLengths = {-1, 0,  1,   2,   3,   7,   8,   9,
                                              15, 16, 17,  23,  24,  25,  31,  32,
                                              33, 64, 100, 193, 200, 201, 230, 32767};
    return kLengths;
}

AssertionResult hline_sweep(const Rect& clip, Color ink) {
    const std::vector<int> xs = ints({{-12, 24}, {60, 75}, {184, 212}});
    for (const int y : {-1, 6, 10, 199, 200}) {
        for (const int x : xs) {
            for (const int w : span_lengths()) {
                const AssertionResult r = agree(
                    clip,
                    ink,
                    [&](Canvas& c) { c.hline(i16(x), i16(y), i16(w), ink); },
                    [&](Framebuffer& fb) { ref_hline(fb, clip, x, y, w, ink); });
                if (!r) {
                    return AssertionFailure() << "hline(" << x << "," << y << "," << w << ") clip "
                                              << describe(clip) << ": " << r.message();
                }
            }
        }
    }
    return AssertionSuccess();
}

AssertionResult vline_sweep(const Rect& clip, Color ink) {
    const std::vector<int> ys = ints({{-12, 24}, {60, 75}, {184, 212}});
    for (const int x : {-1, 0, 7, 8, 10, 100, 199, 200}) {
        for (const int y : ys) {
            for (const int h : span_lengths()) {
                const AssertionResult r = agree(
                    clip,
                    ink,
                    [&](Canvas& c) { c.vline(i16(x), i16(y), i16(h), ink); },
                    [&](Framebuffer& fb) { ref_vline(fb, clip, x, y, h, ink); });
                if (!r) {
                    return AssertionFailure() << "vline(" << x << "," << y << "," << h << ") clip "
                                              << describe(clip) << ": " << r.message();
                }
            }
        }
    }
    return AssertionSuccess();
}

AssertionResult box_case(const Rect& clip, Color ink, bool outline_only, const Rect& r) {
    const AssertionResult same = agree(
        clip,
        ink,
        [&](Canvas& c) {
            if (outline_only) {
                c.rect(r, ink);
            } else {
                c.fill_rect(r, ink);
            }
        },
        [&](Framebuffer& fb) { ref_box(fb, clip, r, outline_only, ink); });
    if (same) {
        return same;
    }
    return AssertionFailure() << (outline_only ? "rect" : "fill_rect") << describe(r) << " clip "
                              << describe(clip) << ": " << same.message();
}

AssertionResult box_sweep(const Rect& clip, Color ink, bool outline_only) {
    for (const int y : {-5, 0, 6, 195, 205}) {
        for (const int x : {-5, 0, 3, 8, 13, 195, 199, 205}) {
            for (const int h : {-1, 0, 1, 7, 200}) {
                for (const int w : {-1, 0, 1, 5, 8, 17, 200, 400}) {
                    const Rect r{.x = i16(x), .y = i16(y), .w = i16(w), .h = i16(h)};
                    const AssertionResult result = box_case(clip, ink, outline_only, r);
                    if (!result) {
                        return result;
                    }
                }
            }
        }
    }
    return AssertionSuccess();
}

TEST(CanvasSpans, HlinePictures) {
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.hline(3, 2, 14, Color::kBlack); // x = 3..16, spans three bytes
        EXPECT_TRUE(shows(fb, 0, 2, {"...##############..."}));
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.hline(9, 0, 3, Color::kBlack); // inside a single byte
        EXPECT_TRUE(shows(fb, 0, 0, {".........###...."}));
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.hline(8, 0, 8, Color::kBlack); // exactly one aligned byte
        EXPECT_EQ(fb.bits[1], 0xFF);
        EXPECT_EQ(ink_count(fb), 8U);
    }
    {
        Framebuffer fb;
        fb.clear(Color::kBlack);
        Canvas canvas(fb);
        canvas.hline(5, 7, 10, Color::kWhite); // white run through a black frame
        EXPECT_EQ(ink_count(fb), (kFrameBytes * 8U) - 10U);
        EXPECT_EQ(fb.get(4, 7), Color::kBlack);
        EXPECT_EQ(fb.get(5, 7), Color::kWhite);
        EXPECT_EQ(fb.get(14, 7), Color::kWhite);
        EXPECT_EQ(fb.get(15, 7), Color::kBlack);
    }
}

TEST(CanvasSpans, VlinePicture) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.vline(10, 3, 4, Color::kBlack);
    EXPECT_TRUE(shows(fb, 9, 2, {"...", ".#.", ".#.", ".#.", ".#.", "..."}));
}

TEST(CanvasSpans, NonPositiveLengthsDrawNothing) {
    Framebuffer fb;
    Canvas canvas(fb);
    for (const int len : {0, -1, -200, -32768}) {
        canvas.hline(10, 10, i16(len), Color::kBlack);
        canvas.vline(10, 10, i16(len), Color::kBlack);
    }
    EXPECT_EQ(ink_count(fb), 0U);
}

TEST(CanvasSpans, HlineMatchesPixelReference) {
    for (const Rect& clip : kSweepClips) {
        for (const Color ink : {Color::kBlack, Color::kWhite}) {
            ASSERT_TRUE(hline_sweep(clip, ink));
        }
    }
}

TEST(CanvasSpans, VlineMatchesPixelReference) {
    for (const Rect& clip : kSweepClips) {
        for (const Color ink : {Color::kBlack, Color::kWhite}) {
            ASSERT_TRUE(vline_sweep(clip, ink));
        }
    }
}

TEST(CanvasRect, OutlinePicture) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.rect({.x = 10, .y = 20, .w = 5, .h = 4}, Color::kBlack);
    EXPECT_TRUE(shows(fb, 10, 20, {"#####", "#...#", "#...#", "#####"}));
}

TEST(CanvasRect, DegenerateSizesAreSolid) {
    struct Case {
        std::int16_t w;
        std::int16_t h;
        std::vector<std::string_view> rows;
    };
    const std::vector<Case> cases = {
        {1, 1, {"#"}},
        {1, 3, {"#", "#", "#"}},
        {3, 1, {"###"}},
        {2, 2, {"##", "##"}},
        {2, 5, {"##", "##", "##", "##", "##"}},
        {5, 2, {"#####", "#####"}},
        {3, 3, {"###", "#.#", "###"}},
    };
    for (const Case& c : cases) {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.rect({.x = 30, .y = 40, .w = c.w, .h = c.h}, Color::kBlack);
        std::string expected;
        for (const std::string_view row : c.rows) {
            expected += row;
            expected += '\n';
        }
        EXPECT_EQ(picture(fb, 30, 40, c.w, c.h), expected) << c.w << "x" << c.h;
        EXPECT_EQ(ink_count(fb), static_cast<std::size_t>(std::ranges::count(expected, '#')));
    }
}

TEST(CanvasRect, NonPositiveSizesDrawNothing) {
    Framebuffer fb;
    Canvas canvas(fb);
    for (const std::pair<int, int>& size :
         {std::pair{0, 5}, {5, 0}, {-3, 5}, {5, -3}, {0, 0}, {-1, -1}}) {
        const Rect r{.x = 20, .y = 20, .w = i16(size.first), .h = i16(size.second)};
        canvas.rect(r, Color::kBlack);
        canvas.fill_rect(r, Color::kBlack);
    }
    EXPECT_EQ(ink_count(fb), 0U);
}

TEST(CanvasRect, FillPictureAndColors) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.fill_rect({.x = 5, .y = 5, .w = 3, .h = 2}, Color::kBlack);
    EXPECT_TRUE(shows(fb, 5, 5, {"###", "###"}));
    canvas.fill_rect({.x = 6, .y = 5, .w = 1, .h = 1}, Color::kWhite); // white fill erases
    EXPECT_EQ(fb.get(6, 5), Color::kWhite);
    EXPECT_EQ(ink_count(fb), 5U);
}

TEST(CanvasRect, RectMatchesPixelReference) {
    for (const Rect& clip : kSweepClips) {
        for (const Color ink : {Color::kBlack, Color::kWhite}) {
            ASSERT_TRUE(box_sweep(clip, ink, true));
        }
    }
}

TEST(CanvasRect, FillRectMatchesPixelReference) {
    for (const Rect& clip : kSweepClips) {
        for (const Color ink : {Color::kBlack, Color::kWhite}) {
            ASSERT_TRUE(box_sweep(clip, ink, false));
        }
    }
}

// ===================================================================================================
// Canvas: lines
// ===================================================================================================

Framebuffer line_frame(int x0, int y0, int x1, int y1) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.line(i16(x0), i16(y0), i16(x1), i16(y1), Color::kBlack);
    return fb;
}

TEST(CanvasLine, ExactPictures) {
    EXPECT_TRUE(
        shows(line_frame(5, 5, 12, 8), 5, 5, {"##......", "..##....", "....##..", "......##"}));
    EXPECT_TRUE(shows(line_frame(5, 5, 8, 12),
                      5,
                      5,
                      {"#...", "#...", ".#..", ".#..", "..#.", "..#.", "...#", "...#"}));
    EXPECT_TRUE(
        shows(line_frame(5, 8, 12, 5), 5, 5, {"......##", "....##..", "..##....", "##......"}));
    EXPECT_TRUE(shows(line_frame(5, 5, 9, 9), 5, 5, {"#....", ".#...", "..#..", "...#.", "....#"}));
    EXPECT_TRUE(shows(line_frame(9, 5, 5, 9), 5, 5, {"....#", "...#.", "..#..", ".#...", "#...."}));
    EXPECT_TRUE(shows(line_frame(5, 5, 9, 5), 5, 5, {"#####"}));
    EXPECT_TRUE(shows(line_frame(5, 5, 5, 9), 5, 5, {"#", "#", "#", "#", "#"}));
    EXPECT_TRUE(shows(line_frame(5, 5, 5, 5), 5, 5, {"#"}));
    // slope 1/2: ties resolve towards the start of the (normalized) line
    EXPECT_TRUE(shows(line_frame(0, 0, 10, 5),
                      0,
                      0,
                      {"##.........",
                       "..##.......",
                       "....##.....",
                       "......##...",
                       "........##.",
                       "..........#"}));
}

TEST(CanvasLine, EndpointOrderDoesNotChangeThePixels) {
    constexpr std::array<std::pair<int, int>, 12> kPoints = {{{0, 0},
                                                              {7, 3},
                                                              {100, 50},
                                                              {199, 199},
                                                              {120, 10},
                                                              {10, 190},
                                                              {50, 50},
                                                              {63, 64},
                                                              {1, 199},
                                                              {199, 0},
                                                              {64, 64},
                                                              {33, 120}}};
    for (const auto& a : kPoints) {
        for (const auto& b : kPoints) {
            const Framebuffer forward = line_frame(a.first, a.second, b.first, b.second);
            const Framebuffer backward = line_frame(b.first, b.second, a.first, a.second);
            ASSERT_TRUE(frames_equal(forward, backward))
                << "(" << a.first << "," << a.second << ")-(" << b.first << "," << b.second << ")";
        }
    }
}

// Bresenham invariants that do not depend on the implementation: one pixel per step along the
// major axis, within half a pixel of the ideal line, endpoints included.
AssertionResult line_is_ideal(const std::pair<int, int>& a, const std::pair<int, int>& b) {
    const int dx = b.first - a.first;
    const int dy = b.second - a.second;
    const Framebuffer fb = line_frame(a.first, a.second, b.first, b.second);
    const int major = std::max(std::abs(dx), std::abs(dy));
    if (ink_count(fb) != static_cast<std::size_t>(major) + 1U) {
        return AssertionFailure() << "pixel count " << ink_count(fb) << ", expected " << major + 1;
    }
    if (fb.get(i16(a.first), i16(a.second)) != Color::kBlack ||
        fb.get(i16(b.first), i16(b.second)) != Color::kBlack) {
        return AssertionFailure() << "an endpoint is not inked";
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            // Distance to the ideal line along the minor axis is |cross| / major: 2 * |cross| <=
            // major.
            const long long cross = (static_cast<long long>(y - a.second) * dx) -
                                    (static_cast<long long>(x - a.first) * dy);
            if (fb.get(i16(x), i16(y)) == Color::kBlack && 2 * std::llabs(cross) > major) {
                return AssertionFailure()
                       << "pixel (" << x << "," << y << ") is more than half a pixel off the line";
            }
        }
    }
    return AssertionSuccess();
}

TEST(CanvasLine, StaysWithinHalfAPixelOfTheIdealLineWithOnePixelPerMajorStep) {
    constexpr std::array<std::pair<int, int>, 11> kPoints = {{{0, 0},
                                                              {7, 3},
                                                              {100, 50},
                                                              {199, 199},
                                                              {120, 10},
                                                              {10, 190},
                                                              {50, 50},
                                                              {63, 64},
                                                              {1, 199},
                                                              {199, 0},
                                                              {33, 120}}};
    for (const auto& a : kPoints) {
        for (const auto& b : kPoints) {
            ASSERT_TRUE(line_is_ideal(a, b))
                << "(" << a.first << "," << a.second << ")-(" << b.first << "," << b.second << ")";
        }
    }
}

TEST(CanvasLine, LinesFarOutsideTheClipDrawNothingAndAreCheap) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.line(-30000, -20000, -10, 500, Color::kBlack);  // entirely left of the frame
    canvas.line(300, -30000, 32767, 32767, Color::kBlack); // entirely right
    canvas.line(-100, -100, 400, -1, Color::kBlack);       // entirely above
    canvas.line(-100, 200, 400, 32000, Color::kBlack);     // entirely below
    EXPECT_EQ(ink_count(fb), 0U);
    // A line that only touches the frame in one corner pixel.
    canvas.line(-10, -10, 0, 0, Color::kBlack);
    EXPECT_TRUE(shows(fb, 0, 0, {"#"}));
}

// ===================================================================================================
// Canvas: circles
// ===================================================================================================

/// x of the octant pixel for row y: the integer nearest to sqrt(r^2 - y^2), i.e. the smallest
/// x >= 0 with (2x + 1)^2 > 4 (r^2 - y^2). (Never a tie: the right side is an odd square.)
int nearest_x(int r, int y) {
    const long long target =
        4LL * ((static_cast<long long>(r) * r) - (static_cast<long long>(y) * y));
    int x = 0;
    while (((2LL * x) + 1) * ((2LL * x) + 1) <= target) {
        ++x;
    }
    return x;
}

Framebuffer reference_outline(int cx, int cy, int r) {
    Framebuffer fb;
    for (int y = 0; y <= r; ++y) {
        const int x = nearest_x(r, y);
        if (x < y) {
            break; // past the 45 degree point
        }
        for (const int sx : {-1, 1}) {
            for (const int sy : {-1, 1}) {
                fb.set(i16(cx + (sx * x)), i16(cy + (sy * y)), Color::kBlack);
                fb.set(i16(cx + (sx * y)), i16(cy + (sy * x)), Color::kBlack);
            }
        }
    }
    return fb;
}

Framebuffer circle_frame(int cx, int cy, int r, bool filled) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.circle(i16(cx), i16(cy), i16(r), filled, Color::kBlack);
    return fb;
}

TEST(CanvasCircle, SmallRadiusPictures) {
    EXPECT_TRUE(shows(circle_frame(10, 10, 0, false), 10, 10, {"#"}));
    EXPECT_TRUE(shows(circle_frame(10, 10, 1, false), 9, 9, {".#.", "#.#", ".#."}));
    EXPECT_TRUE(
        shows(circle_frame(10, 10, 2, false), 8, 8, {".###.", "#...#", "#...#", "#...#", ".###."}));
    EXPECT_TRUE(
        shows(circle_frame(10, 10, 3, false),
              7,
              7,
              {"..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.."}));
    EXPECT_TRUE(shows(circle_frame(10, 10, 4, false),
                      6,
                      6,
                      {"...###...",
                       ".##...##.",
                       ".#.....#.",
                       "#.......#",
                       "#.......#",
                       "#.......#",
                       ".#.....#.",
                       ".##...##.",
                       "...###..."}));
}

TEST(CanvasCircle, FilledSmallRadiusPictures) {
    EXPECT_TRUE(shows(circle_frame(10, 10, 0, true), 10, 10, {"#"}));
    EXPECT_TRUE(shows(circle_frame(10, 10, 1, true), 9, 9, {".#.", "###", ".#."}));
    EXPECT_TRUE(
        shows(circle_frame(10, 10, 2, true), 8, 8, {".###.", "#####", "#####", "#####", ".###."}));
    EXPECT_TRUE(
        shows(circle_frame(10, 10, 3, true),
              7,
              7,
              {"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."}));
}

TEST(CanvasCircle, OutlineMatchesTheNearestIntegerOracle) {
    for (int r = 0; r <= 40; ++r) {
        for (const std::pair<int, int>& centre : {std::pair{100, 100}, {47, 61}, {64, 64}}) {
            ASSERT_TRUE(frames_equal(circle_frame(centre.first, centre.second, r, false),
                                     reference_outline(centre.first, centre.second, r)))
                << "radius " << r << " centre (" << centre.first << "," << centre.second << ")";
        }
    }
}

AssertionResult disc_is_row_hull_of_outline(int r) {
    const Framebuffer outline = reference_outline(100, 100, r);
    const Framebuffer filled = circle_frame(100, 100, r, true);
    for (int y = 0; y < kH; ++y) {
        int lo = -1;
        int hi = -1;
        for (int x = 0; x < kW; ++x) {
            if (outline.get(i16(x), i16(y)) == Color::kBlack) {
                lo = lo < 0 ? x : lo;
                hi = x;
            }
        }
        for (int x = 0; x < kW; ++x) {
            const bool expected = lo >= 0 && x >= lo && x <= hi;
            if ((filled.get(i16(x), i16(y)) == Color::kBlack) != expected) {
                return AssertionFailure()
                       << "radius " << r << " differs at (" << x << "," << y << ")";
            }
        }
    }
    return AssertionSuccess();
}

TEST(CanvasCircle, FilledDiscIsTheRowHullOfTheOutline) {
    for (int r = 0; r <= 40; ++r) {
        ASSERT_TRUE(disc_is_row_hull_of_outline(r));
    }
}

TEST(CanvasCircle, NegativeRadiusDrawsNothing) {
    for (const int r : {-1, -5, -32768}) {
        EXPECT_EQ(ink_count(circle_frame(100, 100, r, false)), 0U);
        EXPECT_EQ(ink_count(circle_frame(100, 100, r, true)), 0U);
    }
}

TEST(CanvasCircle, HugeRadiiAreClippedWithoutOverflow) {
    // Disc around the frame covers it completely; its outline is thousands of pixels away.
    EXPECT_EQ(ink_count(circle_frame(100, 100, 32767, true)), kFrameBytes * 8U);
    EXPECT_EQ(ink_count(circle_frame(100, 100, 32767, false)), 0U);
    // Disc whose near edge is far beyond the frame corner: nothing visible.
    EXPECT_EQ(ink_count(circle_frame(32767, 32767, 32767, true)), 0U);
    // Entirely outside by the bounding box.
    EXPECT_EQ(ink_count(circle_frame(-100, 100, 50, true)), 0U);
    EXPECT_EQ(ink_count(circle_frame(300, 100, 50, false)), 0U);
}

// ===================================================================================================
// Canvas: bitmaps
// ===================================================================================================

// 11x3 icon; the unused low five bits of every second byte are deliberately set to 1.
//   #.#.#.#.#.#
//   ###########
//   #.........#
constexpr std::array<std::uint8_t, 6> kIconBits = {0xAA, 0xBF, 0xFF, 0xFF, 0x80, 0x3F};
constexpr Bitmap kIcon{.width = 11, .height = 3, .bits = kIconBits};

bool icon_bit(int col, int row) {
    const std::size_t index =
        (static_cast<std::size_t>(row) * 2U) + (static_cast<std::size_t>(col) / 8U);
    return ((kIconBits[index] >> (7 - (col % 8))) & 1) != 0;
}

TEST(CanvasBitmap, DrawsSetBitsAndIgnoresRowPadding) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.bitmap(20, 30, kIcon, Color::kBlack);
    EXPECT_TRUE(shows(fb, 20, 30, {"#.#.#.#.#.#", "###########", "#.........#"}));
    EXPECT_EQ(ink_count(fb), 19U); // nothing leaked into the padding columns x = 31..35
}

TEST(CanvasBitmap, WhiteInkErasesSetBits) {
    Framebuffer fb;
    fb.clear(Color::kBlack);
    Canvas canvas(fb);
    canvas.bitmap(20, 30, kIcon, Color::kWhite);
    EXPECT_EQ(ink_count(fb), (kFrameBytes * 8U) - 19U);
    EXPECT_EQ(fb.get(20, 30), Color::kWhite); // icon bit (0, 0) is set
    EXPECT_EQ(fb.get(21, 30), Color::kBlack); // icon bit (1, 0) is clear: background untouched
}

Framebuffer checkerboard() {
    Framebuffer fb;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            fb.set(i16(x), i16(y), ((x + y) % 2 == 0) ? Color::kBlack : Color::kWhite);
        }
    }
    return fb;
}

TEST(CanvasBitmap, ClearBitsLeaveTheBackgroundUntouched) {
    const Framebuffer checker = checkerboard();
    Framebuffer drawn = checker;
    Canvas canvas(drawn);
    canvas.bitmap(20, 30, kIcon, Color::kBlack);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 11; ++col) {
            const Color expected =
                icon_bit(col, row) ? Color::kBlack : checker.get(i16(20 + col), i16(30 + row));
            EXPECT_EQ(drawn.get(i16(20 + col), i16(30 + row)), expected) << col << "," << row;
        }
    }
    EXPECT_EQ(drawn.get(19, 30), checker.get(19, 30));
    EXPECT_EQ(drawn.get(31, 30), checker.get(31, 30));
}

TEST(CanvasBitmap, ClipsAgainstEveryFrameEdge) {
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.bitmap(-4, -1, kIcon, Color::kBlack); // columns 4..10, rows 1..2 remain
        EXPECT_TRUE(shows(fb, 0, 0, {"#######", "......#"}));
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.bitmap(195, 198, kIcon, Color::kBlack); // columns 0..4, rows 0..1 remain
        EXPECT_TRUE(shows(fb, 195, 198, {"#.#.#", "#####"}));
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.bitmap(-11, 0, kIcon, Color::kBlack); // exactly off the left edge
        canvas.bitmap(200, 0, kIcon, Color::kBlack); // exactly off the right edge
        canvas.bitmap(0, -3, kIcon, Color::kBlack);  // exactly above
        canvas.bitmap(0, 200, kIcon, Color::kBlack); // exactly below
        EXPECT_EQ(ink_count(fb), 0U);
    }
}

TEST(CanvasBitmap, InvalidDescriptorsDrawNothing) {
    Framebuffer fb;
    Canvas canvas(fb);
    const std::array<std::uint8_t, 6> data = kIconBits;
    canvas.bitmap(10, 10, Bitmap{}, Color::kBlack);
    canvas.bitmap(10, 10, Bitmap{.width = 0, .height = 3, .bits = data}, Color::kBlack);
    canvas.bitmap(10, 10, Bitmap{.width = -5, .height = 3, .bits = data}, Color::kBlack);
    canvas.bitmap(10, 10, Bitmap{.width = 11, .height = 0, .bits = data}, Color::kBlack);
    canvas.bitmap(10, 10, Bitmap{.width = 11, .height = -2, .bits = data}, Color::kBlack);
    // storage one byte short of ceil(11 / 8) * 3 = 6 bytes
    canvas.bitmap(
        10, 10, Bitmap{.width = 11, .height = 3, .bits = std::span{data}.first(5)}, Color::kBlack);
    canvas.bitmap(10, 10, Bitmap{.width = 8, .height = 1, .bits = {}}, Color::kBlack);
    EXPECT_EQ(ink_count(fb), 0U);
    // Extra storage is fine.
    std::array<std::uint8_t, 10> padded{};
    std::ranges::copy(kIconBits, padded.begin());
    canvas.bitmap(10, 10, Bitmap{.width = 11, .height = 3, .bits = padded}, Color::kBlack);
    EXPECT_EQ(ink_count(fb), 19U);
}

// ===================================================================================================
// Canvas: text (fixture font)
// ===================================================================================================

Framebuffer text_frame(std::int16_t x, std::int16_t y, std::string_view text, const Font& font) {
    Framebuffer fb;
    Canvas canvas(fb);
    (void)canvas.text(x, y, text, font, Color::kBlack);
    return fb;
}

TEST(CanvasText, DrawsGlyphsAtTheBaselineWithTheirAdvance) {
    const Framebuffer fb = text_frame(10, 20, "AB", kFixtureFont);
    EXPECT_TRUE(shows(fb, 10, 15, {".#..##.", "#.#.#.#", "###.##.", "#.#.#.#", "#.#.##."}));
}

TEST(CanvasText, GlyphOffsetsMoveTheInk) {
    // C: x_offset 1, y_offset -4, 6 rows (two below the baseline).
    EXPECT_TRUE(shows(text_frame(10, 20, "C", kFixtureFont),
                      10,
                      16,
                      {".##.", ".#..", ".#..", ".#..", ".##.", "..#."}));
    // G sits entirely below the baseline (y_offset +1).
    EXPECT_TRUE(shows(text_frame(10, 20, "G", kFixtureFont), 10, 20, {"....", "###.", "...."}));
}

TEST(CanvasText, WideGlyphsUseMultiByteRows) {
    EXPECT_TRUE(
        shows(text_frame(10, 20, "F", kFixtureFont), 10, 18, {"###########", "#.........#"}));
}

TEST(CanvasText, ZeroSizeGlyphOnlyAdvances) {
    const Framebuffer fb = text_frame(10, 20, "ADA", kFixtureFont);
    EXPECT_TRUE(
        shows(fb, 10, 15, {".#......#.", "#.#....#.#", "###....###", "#.#....#.#", "#.#....#.#"}));
}

TEST(CanvasText, MissingCodePointsUseTheFallbackGlyph) {
    // 'Z' and '@' are outside 'A'..'G'; "\xC3\xA9" is U+00E9; "\xFF" is malformed UTF-8 (U+FFFD).
    for (const std::string_view missing : {"Z", "@", "\xC3\xA9", "\xFF", "\x01", "E"}) {
        EXPECT_TRUE(shows(
            text_frame(10, 20, missing, kFixtureFont), 10, 16, {"####", "#..#", "#..#", "####"}))
            << missing;
        EXPECT_EQ(Canvas::text_width(missing, kFixtureFont), 6);
    }
}

TEST(CanvasText, ReturnValueIsTheAdvanceWidthAndMatchesTextWidth) {
    struct Case {
        std::string_view text;
        int width;
    };
    const std::array<Case, 8> cases = {{
        {"", 0},
        {"A", 4},
        {"AB", 8},
        {"ABC", 13},
        {"ADA", 11},
        {"FG", 16},
        {"AZB", 14},       // 'Z' is missing: the 6 px fallback box
        {"\xC3\xA9x", 12}, // two missing code points: 6 + 6
    }};
    Framebuffer fb;
    Canvas canvas(fb);
    for (const Case& c : cases) {
        EXPECT_EQ(Canvas::text_width(c.text, kFixtureFont), c.width) << c.text;
        EXPECT_EQ(canvas.text(10, 20, c.text, kFixtureFont, Color::kBlack), c.width) << c.text;
    }
}

TEST(CanvasText, EmptyStringDrawsNothing) {
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(canvas.text(10, 20, "", kFixtureFont, Color::kBlack), 0);
    EXPECT_EQ(ink_count(fb), 0U);
}

TEST(CanvasText, ClipsPartialGlyphsExactly) {
    Framebuffer fb;
    Canvas canvas(fb);
    canvas.set_clip({.x = 12, .y = 16, .w = 3, .h = 2});
    (void)canvas.text(10, 20, "AB", kFixtureFont, Color::kBlack);
    // Only column 2 of A (x = 12) and column 0 of B (x = 14) reach rows 16..17.
    EXPECT_TRUE(shows(fb, 12, 16, {"#.#", "#.#"}));
}

TEST(CanvasText, GlyphsStartingOffTheFrameAreClippedNotWrapped) {
    const Framebuffer left = text_frame(-1, 20, "A", kFixtureFont); // column 0 of A is off-screen
    EXPECT_TRUE(shows(left, 0, 15, {"#.", ".#", "##", ".#", ".#"}));
    EXPECT_EQ(ink_count(text_frame(-3, 20, "A", kFixtureFont)), 0U);
    EXPECT_EQ(ink_count(text_frame(200, 20, "A", kFixtureFont)), 0U);
    const Framebuffer right = text_frame(198, 20, "A", kFixtureFont); // columns 0 and 1 visible
    EXPECT_EQ(ink_count(right), 6U);
}

TEST(CanvasText, AlignmentPlacesTheInkInsideTheBox) {
    struct Case {
        Align align;
        std::int16_t width;
        int ink_x0;
        int ink_x1;
    };
    // "AB" is 8 px wide with ink from its start to start + 6; the box starts at x = 20.
    const std::array<Case, 8> cases = {{
        {Align::kLeft, 50, 20, 26},
        {Align::kCenter, 50, 41, 47}, // slack 42 -> half 21
        {Align::kRight, 50, 62, 68},  // slack 42
        {Align::kCenter, 45, 38, 44}, // slack 37 -> half rounds down to 18
        {Align::kRight, 45, 57, 63},  // slack 37
        {Align::kLeft, 8, 20, 26},    // the text exactly fills the box
        {Align::kCenter, 8, 20, 26},
        {Align::kRight, 8, 20, 26},
    }};
    for (const Case& c : cases) {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.text_aligned(20, 50, c.width, "AB", kFixtureFont, c.align, Color::kBlack);
        const Bounds b = ink_bounds(fb);
        EXPECT_EQ(b.x0, c.ink_x0) << static_cast<int>(c.align) << "/" << c.width;
        EXPECT_EQ(b.x1, c.ink_x1) << static_cast<int>(c.align) << "/" << c.width;
    }
}

TEST(CanvasText, TextWiderThanTheBoxStartsAtTheLeftEdgeForEveryAlignment) {
    for (const Align a : {Align::kLeft, Align::kCenter, Align::kRight}) {
        for (const std::int16_t width : {std::int16_t{5}, std::int16_t{0}, std::int16_t{-20}}) {
            Framebuffer fb;
            Canvas canvas(fb);
            canvas.text_aligned(20, 50, width, "AB", kFixtureFont, a, Color::kBlack);
            EXPECT_EQ(ink_bounds(fb).x0, 20) << static_cast<int>(a) << " width " << width;
            EXPECT_EQ(ink_bounds(fb).x1, 26);
        }
    }
}

TEST(CanvasText, TextWidthAndReturnSaturateAtInt16Max) {
    const std::string long_text(10000, 'B'); // 10000 * 4 = 40000 px
    EXPECT_EQ(Canvas::text_width(long_text, kFixtureFont), 32767);
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(canvas.text(-32768, 50, long_text, kFixtureFont, Color::kBlack), 32767);
    EXPECT_EQ(canvas.text(0, 50, long_text, kFixtureFont, Color::kBlack), 32767);
    // The visible part is exactly what a short text would draw: B glyphs every 4 px.
    EXPECT_EQ(ink_bounds(fb).x0, 0);
    EXPECT_EQ(ink_bounds(fb).x1, 198); // B's last glyph at pen 196 has ink in column 2
}

TEST(CanvasText, ExtremeOriginsDoNotOverflow) {
    constexpr std::array<std::int16_t, 4> kEdge = {std::numeric_limits<std::int16_t>::min(),
                                                   -32767,
                                                   32766,
                                                   std::numeric_limits<std::int16_t>::max()};
    Framebuffer fb;
    Canvas canvas(fb);
    for (const std::int16_t x : kEdge) {
        for (const std::int16_t y : kEdge) {
            (void)canvas.text(x, y, "ABCDEFG", kFixtureFont, Color::kBlack);
            for (const Align a : {Align::kLeft, Align::kCenter, Align::kRight}) {
                canvas.text_aligned(x, y, x, "AB", kFixtureFont, a, Color::kBlack);
                canvas.text_aligned(x,
                                    y,
                                    std::numeric_limits<std::int16_t>::max(),
                                    "AB",
                                    kFixtureFont,
                                    a,
                                    Color::kBlack);
            }
        }
    }
    EXPECT_EQ(ink_count(fb), 0U);
}

// --- UTF-8
// ----------------------------------------------------------------------------------------

TEST(CanvasUtf8, ValidSequencesOfEveryLengthAndBoundary) {
    struct Case {
        std::string_view bytes;
        char32_t code_point;
    };
    const std::array<Case, 14> cases = {{
        {"A", 0x41},
        {"\x7F", 0x7F},
        {"\xC2\x80", 0x80},
        {"\xC3\xA9", 0xE9},
        {"\xDF\xBF", 0x7FF},
        {"\xE0\xA0\x80", 0x800},
        {"\xE2\x82\xAC", 0x20AC},
        {"\xED\x9F\xBF", 0xD7FF},
        {"\xEE\x80\x80", 0xE000},
        {"\xEF\xBF\xBD", 0xFFFD},
        {"\xEF\xBF\xBF", 0xFFFF},
        {"\xF0\x90\x80\x80", 0x10000},
        {"\xF0\x9F\x98\x80", 0x1F600},
        {"\xF4\x8F\xBF\xBF", 0x10FFFF},
    }};
    for (const Case& c : cases) {
        const PairFont match(c.code_point, 9);
        EXPECT_EQ(Canvas::text_width(c.bytes, match.font()), 9)
            << std::hex << static_cast<std::uint32_t>(c.code_point);
        // The same bytes against a font that starts elsewhere must miss (not decode to garbage).
        const PairFont other(c.code_point + 5, 9);
        EXPECT_EQ(Canvas::text_width(c.bytes, other.font()), 1)
            << std::hex << static_cast<std::uint32_t>(c.code_point);
    }
}

TEST(CanvasUtf8, MalformedSequencesYieldOneReplacementPerMaximalSubpart) {
    struct Case {
        std::string_view bytes;
        int width; // 5 per U+FFFD, 1 per valid code point the font lacks (fallback glyph)
    };
    const PairFont font(0xFFFD, 5);
    const std::array<Case, 24> cases = {{
        {"\xC0\x80", 10}, // overlong 2-byte (C0 is never a lead): lead + stray continuation
        {"\xC1\xBF", 10},
        {"\xE0\x80\x80", 15}, // overlong 3-byte (E0 needs A0..BF)
        {"\xE0\x9F\xBF", 15},
        {"\xED\xA0\x80", 15},     // U+D800 surrogate (ED needs 80..9F)
        {"\xED\xBF\xBF", 15},     // U+DFFF surrogate
        {"\xF0\x80\x80\x80", 20}, // overlong 4-byte (F0 needs 90..BF)
        {"\xF0\x8F\xBF\xBF", 20},
        {"\xF4\x90\x80\x80", 20}, // above U+10FFFF (F4 needs 80..8F)
        {"\xF5\x80\x80\x80", 20}, // F5..FF are never leads
        {"\xF8\x88\x80\x80\x80", 25},
        {"\xFF", 5},
        {"\xFE", 5},
        {"\x80", 5}, // stray continuation bytes
        {"\xBF", 5},
        {"\x80\x80", 10},
        {"\xC2", 5}, // truncated at the end of the text: one replacement
        {"\xE2\x82", 5},
        {"\xF0\x9F\x98", 5},
        {"\xF0\x9F", 5},
        {"\xC2\xC2", 10}, // a lead byte cannot continue a sequence: two truncations
        {"\xE2\x82\xE2\x82", 10},
        {"\xF0\x9F\xC3\xA9\xC3\xA9", 7}, // truncated 4-byte, then two valid 2-byte (fallback)
        {"\xE2\x28\xA1", 11},            // E2 truncated, '(' valid (fallback), stray A1
    }};
    for (const Case& c : cases) {
        EXPECT_EQ(Canvas::text_width(c.bytes, font.font()), c.width)
            << c.bytes.size() << " bytes starting 0x" << std::hex
            << static_cast<unsigned>(static_cast<unsigned char>(c.bytes.front()));
    }
    // Resynchronization: the byte after a broken sequence is decoded normally.
    EXPECT_EQ(Canvas::text_width("\xE2\x82"
                                 "A",
                                 font.font()),
              5 + 1);
    EXPECT_EQ(Canvas::text_width("\xC2"
                                 "A",
                                 font.font()),
              5 + 1);
    EXPECT_EQ(Canvas::text_width("A\xFF"
                                 "A",
                                 font.font()),
              1 + 5 + 1);
}

TEST(CanvasUtf8, EmbeddedNulIsAnOrdinaryCodePoint) {
    const PairFont font(0, 7); // U+0000 is glyph 0
    EXPECT_EQ(Canvas::text_width(std::string_view("\0\0A", 3), font.font()), 7 + 7 + 1);
}

TEST(CanvasUtf8, MultiByteGlyphsAreDrawnAtTheirPenPositions) {
    const PairFont font(0xE9, 7); // "é" advances 7
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(canvas.text(10, 20, "\xC3\xA9\xC3\xA9", font.font(), Color::kBlack), 14);
    EXPECT_EQ(fb.get(10, 19), Color::kBlack);
    EXPECT_EQ(fb.get(17, 19), Color::kBlack);
    EXPECT_EQ(ink_count(fb), 2U);
    // A malformed byte after them renders the fallback glyph (advance 1) at pen x = 24.
    EXPECT_EQ(canvas.text(10, 40, "\xC3\xA9\xC3\xA9\xFF", font.font(), Color::kBlack), 15);
    EXPECT_EQ(fb.get(24, 39), Color::kBlack);
}

// --- Degenerate fonts
// -----------------------------------------------------------------------------

TEST(CanvasText, FontWithoutGlyphsHasNoWidthAndDrawsNothing) {
    const Font empty{.name = "empty",
                     .line_height = 0,
                     .ascent = 0,
                     .first = U'A',
                     .glyphs = {},
                     .bitmap = {},
                     .fallback_index = 0};
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(Canvas::text_width("AB\xFF", empty), 0);
    EXPECT_EQ(canvas.text(10, 20, "AB", empty, Color::kBlack), 0);
    canvas.text_aligned(10, 20, 100, "AB", empty, Align::kCenter, Color::kBlack);
    EXPECT_EQ(ink_count(fb), 0U);
}

TEST(CanvasText, InvalidFallbackIndexSkipsMissingCodePoints) {
    Font font = kFixtureFont;
    font.fallback_index = 200; // beyond the glyph table
    EXPECT_EQ(Canvas::text_width("AZB", font), 8);
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(canvas.text(10, 20, "AZB", font, Color::kBlack), 8);
    EXPECT_TRUE(shows(fb, 10, 15, {".#..##.", "#.#.#.#", "###.##.", "#.#.#.#", "#.#.##."}));
}

TEST(CanvasText, GlyphDataOutsideTheFontBitmapIsSkippedButStillAdvances) {
    constexpr std::array<std::uint8_t, 5> kBits = {0x40, 0xA0, 0xE0, 0xA0, 0xA0};
    constexpr std::array<Glyph, 4> kGlyphs = {{
        {.offset = 0, .width = 3, .height = 5, .x_offset = 0, .y_offset = -5, .advance = 4},
        {.offset = 3,
         .width = 3,
         .height = 5,
         .x_offset = 0,
         .y_offset = -5,
         .advance = 5}, // needs 5, has 2
        {.offset = 0xFFFFFFFFU,
         .width = 3,
         .height = 5,
         .x_offset = 0,
         .y_offset = -5,
         .advance = 6},
        {.offset = 5,
         .width = 3,
         .height = 5,
         .x_offset = 0,
         .y_offset = -5,
         .advance = 7}, // starts at the end
    }};
    const Font font{.name = "bad",
                    .line_height = 6,
                    .ascent = 5,
                    .first = U'A',
                    .glyphs = kGlyphs,
                    .bitmap = kBits,
                    .fallback_index = 0};
    Framebuffer fb;
    Canvas canvas(fb);
    EXPECT_EQ(canvas.text(10, 20, "ABCD", font, Color::kBlack), 4 + 5 + 6 + 7);
    EXPECT_EQ(ink_count(fb), 10U); // only the valid 'A' glyph
}

// ===================================================================================================
// Extreme coordinates (run under UBSan: any int overflow aborts the test)
// ===================================================================================================

TEST(Canvas, ExtremeInt16CoordinatesNeitherOverflowNorDrawStrayPixels) {
    constexpr std::int16_t kMin = std::numeric_limits<std::int16_t>::min();
    constexpr std::int16_t kMax = std::numeric_limits<std::int16_t>::max();
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.hline(kMin, 5, kMax, Color::kBlack); // ends at x = -1
        canvas.vline(5, kMin, kMax, Color::kBlack);
        canvas.hline(kMax, 5, kMax, Color::kBlack);
        canvas.vline(5, kMax, kMax, Color::kBlack);
        canvas.rect({.x = kMin, .y = kMin, .w = kMax, .h = kMax}, Color::kBlack);
        canvas.fill_rect({.x = kMin, .y = kMin, .w = kMax, .h = kMax}, Color::kBlack);
        canvas.pixel(kMin, kMin, Color::kBlack);
        canvas.pixel(kMax, kMax, Color::kBlack);
        EXPECT_EQ(ink_count(fb), 0U);
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.fill_rect({.x = 0, .y = 0, .w = kMax, .h = kMax}, Color::kBlack);
        EXPECT_EQ(ink_count(fb), kFrameBytes * 8U);
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.rect({.x = 0, .y = 0, .w = kMax, .h = kMax},
                    Color::kBlack); // only top and left edge visible
        EXPECT_EQ(ink_count(fb), 200U + 199U);
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.line(kMin, kMin, kMax, kMax, Color::kBlack); // exact diagonal through the frame
        EXPECT_EQ(ink_count(fb), 200U);
        EXPECT_EQ(fb.get(0, 0), Color::kBlack);
        EXPECT_EQ(fb.get(199, 199), Color::kBlack);
        EXPECT_EQ(fb.get(100, 101), Color::kWhite);
    }
    {
        Framebuffer fb;
        Canvas canvas(fb);
        canvas.line(kMin, 100, kMax, 100, Color::kBlack);
        canvas.line(100, kMin, 100, kMax, Color::kBlack);
        canvas.line(kMin, kMin, kMax, 0, Color::kBlack);
        canvas.line(kMax, kMin, kMin, kMax, Color::kBlack);
        EXPECT_GE(ink_count(fb), 399U);
    }
}

// ===================================================================================================
// Render path: no heap
// ===================================================================================================

std::size_t allocations_so_far() {
    return allocation_counter().load(std::memory_order_relaxed);
}

/// True if the allocator hook is live (ASan build): a probe allocation must be counted.
bool allocation_counting_works() {
    const std::size_t before = allocations_so_far();
    auto* probe = new std::uint8_t[64];              // NOLINT(cppcoreguidelines-owning-memory)
    *static_cast<volatile std::uint8_t*>(probe) = 1; // keep the allocation observable
    const std::size_t after = allocations_so_far();
    delete[] probe; // NOLINT(cppcoreguidelines-owning-memory)
    return after > before;
}

class CountingSink final : public ByteSink {
public:
    Status write(std::span<const std::uint8_t> bytes) override {
        size_ += bytes.size();
        return ok();
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::size_t size_ = 0;
};

TEST(RenderPath, MakesNoHeapAllocations) {
    if (!allocation_counting_works()) {
        GTEST_SKIP() << "allocation counting needs the AddressSanitizer allocator hook";
    }
    Framebuffer fb;
    CountingSink sink;
    const std::size_t before = allocations_so_far();

    Canvas canvas(fb);
    for (const Shape& shape : kShapes) {
        shape.draw(canvas, Color::kBlack);
        canvas.set_clip({.x = 50, .y = 50, .w = 100, .h = 100});
        shape.draw(canvas, Color::kWhite);
        canvas.reset_clip();
    }
    (void)Canvas::text_width("ABCDEFG", kFixtureFont);
    const std::uint32_t crc = fb.crc32();
    const Status png = encode_png(fb, sink);

    const std::size_t after = allocations_so_far();
    EXPECT_EQ(after, before) << "the render path allocated";
    EXPECT_TRUE(png.has_value());
    EXPECT_EQ(sink.size(), 5268U);
    EXPECT_EQ(crc, fb.crc32());
}

} // namespace
} // namespace qz::gfx
