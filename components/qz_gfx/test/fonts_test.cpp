// Tests for the generated fonts (WP-06): glyph metrics against values derived by hand from the
// Spleen BDF sources, structural invariants of every glyph table, Latin-1 / clock coverage, the
// kHuge "88:88" width and height bound, the flash budget, and a render-to-framebuffer smoke test.
#include "qz/gfx/framebuffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>

namespace qz::gfx {
namespace {

// gtest macros inflate the cognitive-complexity score of table-driven checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)

constexpr std::array<FontId, 4> kAllFonts{
    FontId::kSmall, FontId::kMedium, FontId::kLarge, FontId::kHuge};
constexpr std::size_t kFontBudgetBytes = std::size_t{40} * 1024;

[[nodiscard]] const Glyph& glyph_of(const Font& f, char32_t cp) {
    EXPECT_GE(static_cast<std::uint32_t>(cp), static_cast<std::uint32_t>(f.first));
    EXPECT_LT(static_cast<std::size_t>(cp - f.first), f.glyphs.size());
    return f.glyphs[cp - f.first];
}

[[nodiscard]] bool is_fallback(const Font& f, char32_t cp) {
    const Glyph& g = glyph_of(f, cp);
    const Glyph& fb = f.glyphs[f.fallback_index];
    return g.offset == fb.offset && g.width == fb.width && g.height == fb.height &&
           g.x_offset == fb.x_offset && g.y_offset == fb.y_offset && g.advance == fb.advance;
}

struct Ink {
    int min_x = 1000;
    int min_y = 1000;
    int max_x = -1;
    int max_y = -1;
    int count = 0;
    [[nodiscard]] int width() const { return max_x - min_x + 1; }
    [[nodiscard]] int height() const { return max_y - min_y + 1; }
};

[[nodiscard]] Ink ink_of(const Framebuffer& fb) {
    Ink ink;
    for (std::int16_t y = 0; y < kHeight; ++y) {
        for (std::int16_t x = 0; x < kWidth; ++x) {
            if (fb.get(x, y) == Color::kBlack) {
                ink.min_x = std::min<int>(ink.min_x, x);
                ink.min_y = std::min<int>(ink.min_y, y);
                ink.max_x = std::max<int>(ink.max_x, x);
                ink.max_y = std::max<int>(ink.max_y, y);
                ++ink.count;
            }
        }
    }
    return ink;
}

// --- Table structure ----------------------------------------------------------------------------

TEST(FontsTest, EveryFontIdResolvesToADistinctFont) {
    std::set<const Font*> seen;
    for (const FontId id : kAllFonts) {
        const Font& f = font(id);
        EXPECT_FALSE(f.name.empty());
        EXPECT_GT(f.line_height, 0);
        EXPECT_GT(f.ascent, 0);
        EXPECT_LE(f.ascent, f.line_height);
        EXPECT_EQ(f.first, 0x20U);
        EXPECT_FALSE(f.glyphs.empty());
        EXPECT_FALSE(f.bitmap.empty());
        EXPECT_LT(f.fallback_index, f.glyphs.size());
        seen.insert(&f);
    }
    EXPECT_EQ(seen.size(), kAllFonts.size());
}

TEST(FontsTest, SameIdGivesSameObject) {
    for (const FontId id : kAllFonts) {
        EXPECT_EQ(&font(id), &font(id));
    }
}

TEST(FontsTest, UnknownIdFallsBackToSmall) {
    EXPECT_EQ(&font(FontId::kCount), &font(FontId::kSmall));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately invalid id
    EXPECT_EQ(&font(static_cast<FontId>(200)), &font(FontId::kSmall));
}

TEST(FontsTest, FontsGrowInSize) {
    int previous = 0;
    for (const FontId id : kAllFonts) {
        EXPECT_GT(font(id).line_height, previous);
        previous = font(id).line_height;
    }
}

TEST(FontsTest, EveryGlyphStaysInsideBitmapAndLineBox) {
    for (const FontId id : kAllFonts) {
        const Font& f = font(id);
        const int descent = f.line_height - f.ascent;
        for (std::size_t i = 0; i < f.glyphs.size(); ++i) {
            const Glyph& g = f.glyphs[i];
            const std::size_t row_bytes = (static_cast<std::size_t>(g.width) + 7U) / 8U;
            const std::size_t bytes = row_bytes * g.height;
            EXPECT_LE(static_cast<std::size_t>(g.offset) + bytes, f.bitmap.size())
                << f.name << " glyph " << i;
            EXPECT_EQ(g.width == 0, g.height == 0) << f.name << " glyph " << i;
            EXPECT_GT(g.advance, 0) << f.name << " glyph " << i;
            if (g.height > 0) {
                EXPECT_GE(g.y_offset, -f.ascent) << f.name << " glyph " << i;
                EXPECT_LE(g.y_offset + g.height, descent) << f.name << " glyph " << i;
                // Tight boxes: first and last row and column of every glyph carry ink.
                const std::uint8_t* rows = f.bitmap.data() + g.offset;
                std::uint8_t first_row = 0;
                std::uint8_t last_row = 0;
                for (std::size_t b = 0; b < row_bytes; ++b) {
                    first_row = static_cast<std::uint8_t>(first_row | rows[b]);
                    last_row = static_cast<std::uint8_t>(last_row |
                                                         rows[((g.height - 1) * row_bytes) + b]);
                }
                EXPECT_NE(first_row, 0) << f.name << " glyph " << i;
                EXPECT_NE(last_row, 0) << f.name << " glyph " << i;
                // Padding bits past the width are zero.
                if (g.width % 8 != 0) {
                    const auto pad_mask = static_cast<std::uint8_t>(0xFFU >> (g.width % 8));
                    for (std::size_t r = 0; r < g.height; ++r) {
                        EXPECT_EQ(rows[(r * row_bytes) + row_bytes - 1] & pad_mask, 0)
                            << f.name << " glyph " << i << " row " << r;
                    }
                }
            }
        }
    }
}

TEST(FontsTest, TotalFontDataFitsBudget) {
    std::size_t total = 0;
    for (const FontId id : kAllFonts) {
        total += font(id).glyphs.size_bytes() + font(id).bitmap.size_bytes();
    }
    EXPECT_LE(total, kFontBudgetBytes);
    EXPECT_GT(total, 0U);
}

// --- Metrics (expected values derived by hand from the BDF bitmaps) -----------------------------

TEST(FontsTest, MediumMetricsMatchBdfDerivedValues) {
    const Font& f = font(FontId::kMedium);
    EXPECT_EQ(f.name, "spleen-8x16");
    EXPECT_EQ(f.line_height, 16);
    EXPECT_EQ(f.ascent, 12);

    const Glyph& space = glyph_of(f, U' ');
    EXPECT_EQ(space.width, 0);
    EXPECT_EQ(space.height, 0);
    EXPECT_EQ(space.advance, 8);

    // '!': BDF rows 2..8 and 10..11 are 0x18 -> columns 3..4, 10 rows tall, top 10 above baseline.
    const Glyph& bang = glyph_of(f, U'!');
    EXPECT_EQ(bang.width, 2);
    EXPECT_EQ(bang.height, 10);
    EXPECT_EQ(bang.x_offset, 3);
    EXPECT_EQ(bang.y_offset, -10);
    EXPECT_EQ(bang.advance, 8);
    EXPECT_EQ(f.bitmap[bang.offset], 0xC0);
    EXPECT_EQ(f.bitmap[bang.offset + 7], 0x00); // the gap row between the bar and the dot
    EXPECT_EQ(f.bitmap[bang.offset + 9], 0xC0);

    // '-': BDF row 7 is 0x7E -> a single row, columns 1..6, 5 above baseline.
    const Glyph& minus = glyph_of(f, U'-');
    EXPECT_EQ(minus.width, 6);
    EXPECT_EQ(minus.height, 1);
    EXPECT_EQ(minus.x_offset, 1);
    EXPECT_EQ(minus.y_offset, -5);
    EXPECT_EQ(f.bitmap[minus.offset], 0xFC);
}

TEST(FontsTest, MonospaceAdvancesMatchTheCell) {
    EXPECT_EQ(glyph_of(font(FontId::kSmall), U'W').advance, 6);
    EXPECT_EQ(glyph_of(font(FontId::kMedium), U'W').advance, 8);
    EXPECT_EQ(glyph_of(font(FontId::kLarge), U'W').advance, 16);
    EXPECT_EQ(glyph_of(font(FontId::kHuge), U'8').advance, 36);
}

TEST(FontsTest, DescendersSitBelowTheBaselineAndCapsAboveIt) {
    for (const FontId id : {FontId::kSmall, FontId::kMedium, FontId::kLarge}) {
        const Font& f = font(id);
        const Glyph& cap = glyph_of(f, U'H');
        EXPECT_EQ(cap.y_offset + cap.height, 0) << f.name << ": caps rest on the baseline";
        const Glyph& g = glyph_of(f, U'g');
        EXPECT_GT(g.y_offset + g.height, 0) << f.name << ": 'g' descends";
    }
}

// --- Coverage -----------------------------------------------------------------------------------

TEST(FontsTest, SmallMediumLargeCoverLatin1) {
    for (const FontId id : {FontId::kSmall, FontId::kMedium, FontId::kLarge}) {
        const Font& f = font(id);
        EXPECT_EQ(static_cast<std::uint32_t>(f.first), 0x20U);
        ASSERT_EQ(f.glyphs.size(), 0xE0U) << f.name; // U+0020..U+00FF
        EXPECT_EQ(f.fallback_index, U'?' - f.first);
        for (char32_t cp = 0x20; cp <= 0xFF; ++cp) {
            const bool printable = cp <= 0x7E || cp >= 0xA0;
            if (printable && cp != U'?') {
                EXPECT_FALSE(is_fallback(f, cp))
                    << f.name << " U+" << std::hex << static_cast<std::uint32_t>(cp);
            }
            if (!printable) {
                EXPECT_TRUE(is_fallback(f, cp))
                    << f.name << " control U+" << std::hex << static_cast<std::uint32_t>(cp);
            }
            const bool blank = cp == 0x20 || cp == 0xA0 || cp == 0xAD;
            if (printable && !blank) {
                EXPECT_GT(glyph_of(f, cp).width, 0)
                    << f.name << " U+" << std::hex << static_cast<std::uint32_t>(cp);
            }
        }
        // Accented letters differ from their base letter.
        EXPECT_NE(glyph_of(f, 0xE9).offset, glyph_of(f, U'e').offset) << f.name;
        EXPECT_GT(glyph_of(f, 0xB0).width, 0) << f.name << ": degree sign";
    }
}

TEST(FontsTest, HugeCoversClockCharacters) {
    const Font& f = font(FontId::kHuge);
    for (const char32_t cp : std::u32string_view(U"0123456789:-%.")) {
        EXPECT_FALSE(is_fallback(f, cp)) << "U+" << std::hex << static_cast<std::uint32_t>(cp);
        EXPECT_GT(glyph_of(f, cp).width, 0) << "U+" << std::hex << static_cast<std::uint32_t>(cp);
    }
    // Space is the fallback (blank) and advances like a digit.
    EXPECT_EQ(f.fallback_index, 0);
    EXPECT_EQ(glyph_of(f, U' ').width, 0);
    EXPECT_EQ(glyph_of(f, U' ').advance, glyph_of(f, U'8').advance);
    // Characters outside the set render as blank cells, not garbage.
    EXPECT_TRUE(is_fallback(f, U'/'));
    EXPECT_EQ(glyph_of(f, U'/').width, 0); // inside the table range but not a clock character
    EXPECT_EQ(Canvas::text_width("A", f), glyph_of(f, U' ').advance);
}

TEST(FontsTest, HugeDigitsAreAtLeast48PixelsTall) {
    const Font& f = font(FontId::kHuge);
    for (char32_t cp = U'0'; cp <= U'9'; ++cp) {
        EXPECT_GE(glyph_of(f, cp).height, 48)
            << "digit U+" << std::hex << static_cast<std::uint32_t>(cp);
    }
    EXPECT_GE(glyph_of(f, U'8').height, 60);
    EXPECT_EQ(glyph_of(f, U'8').y_offset + glyph_of(f, U'8').height,
              0); // digits sit on the baseline
}

TEST(FontsTest, HugeClockTextFitsTheDisplayWidth) {
    const Font& f = font(FontId::kHuge);
    EXPECT_EQ(Canvas::text_width("88:88", f), 180);
    for (int h = 0; h < 100; h += 11) {
        for (int m = 0; m < 100; m += 7) {
            const std::array<char, 5> t{static_cast<char>('0' + (h / 10)),
                                        static_cast<char>('0' + (h % 10)),
                                        ':',
                                        static_cast<char>('0' + (m / 10)),
                                        static_cast<char>('0' + (m % 10))};
            EXPECT_LE(Canvas::text_width(std::string_view(t.data(), t.size()), f), kWidth);
        }
    }
}

// --- Rendering smoke tests ----------------------------------------------------------------------

TEST(FontsTest, RenderedHugeClockInkFitsDisplayAndIsTallEnough) {
    Framebuffer fb;
    Canvas canvas(fb);
    const Font& f = font(FontId::kHuge);
    const std::int16_t advance = canvas.text(10, 100, "88:88", f, Color::kBlack);
    EXPECT_EQ(advance, 180);
    const Ink ink = ink_of(fb);
    EXPECT_GT(ink.count, 500);
    EXPECT_GE(ink.height(), 48);
    EXPECT_LE(ink.width(), kWidth);
    EXPECT_GE(ink.min_x, 10);
    EXPECT_LT(ink.max_x, 10 + 180);
    EXPECT_EQ(ink.max_y, 99); // bottom ink row is directly above the baseline at y = 100
    // Centered by the canvas: inside the panel with room to spare.
    Framebuffer centered;
    Canvas c2(centered);
    c2.text_aligned(0, 120, kWidth, "88:88", f, Align::kCenter, Color::kBlack);
    const Ink ci = ink_of(centered);
    EXPECT_GE(ci.min_x, 10);
    EXPECT_LE(ci.max_x, kWidth - 10);
}

TEST(FontsTest, EveryHugeDigitRendersDistinctly) {
    std::set<std::uint32_t> crcs;
    for (char c = '0'; c <= '9'; ++c) {
        Framebuffer fb;
        Canvas canvas(fb);
        const char s[2] = {c, '\0'};
        (void)canvas.text(20, 100, s, font(FontId::kHuge), Color::kBlack);
        crcs.insert(fb.crc32());
    }
    EXPECT_EQ(crcs.size(), 10U);
}

TEST(FontsTest, MediumTextRendersDeterministicallyAndMatchesWidth) {
    constexpr std::string_view kText = "Qu\xC3\xA4rtz 12:34 \xC2\xB0"
                                       "C";
    auto render = [&] {
        Framebuffer fb;
        Canvas canvas(fb);
        const std::int16_t w = canvas.text(4, 20, kText, font(FontId::kMedium), Color::kBlack);
        EXPECT_EQ(w, Canvas::text_width(kText, font(FontId::kMedium)));
        return fb;
    };
    const Framebuffer a = render();
    const Framebuffer b = render();
    EXPECT_EQ(a.crc32(), b.crc32());
    const Ink ink = ink_of(a);
    EXPECT_GT(ink.count, 100);
    EXPECT_GE(ink.min_y, 20 - 12); // above-baseline extent bounded by the ascent
    EXPECT_LT(ink.max_y, 20 + 4);  // descent
    EXPECT_EQ(Canvas::text_width(kText, font(FontId::kMedium)), 8 * 15);
}

TEST(FontsTest, EveryFontDrawsMissingCodePointsAsFallback) {
    for (const FontId id : {FontId::kSmall, FontId::kMedium, FontId::kLarge}) {
        Framebuffer a;
        Framebuffer b;
        Canvas ca(a);
        Canvas cb(b);
        (void)ca.text(5, 40, "\xE2\x82\xAC", font(id), Color::kBlack); // U+20AC, not in Latin-1
        (void)cb.text(5, 40, "?", font(id), Color::kBlack);
        EXPECT_EQ(a.crc32(), b.crc32()) << font(id).name;
    }
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::gfx
