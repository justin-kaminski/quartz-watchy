// Minimal watch face (id 1): large time only, plus one tiny status line at the bottom
// ("87% 8432 steps synced", with a power tag first when not in normal power state).
#include "face_common.hpp"

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;

constexpr std::int16_t kStatusBaselineY = 190;
constexpr std::int16_t kMeridiemGapY = 22;
constexpr std::uint32_t kMaxShownSteps = 99999;

void append_word(TextBuf<48>& line, std::string_view word) noexcept {
    if (word.empty()) {
        return;
    }
    if (!line.view().empty()) {
        line.put_char(' ');
    }
    line.put(word);
}

} // namespace

void render_minimal_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);

    const TimeText tt = format_time(s);
    c.text_aligned(0,
                   layout::kMinimalBaselineY,
                   gfx::kWidth,
                   tt.text.view(),
                   gfx::font(FontId::kHuge),
                   Align::kCenter,
                   Color::kBlack);
    if (tt.show_meridiem) {
        c.text_aligned(0,
                       static_cast<std::int16_t>(layout::kMinimalBaselineY + kMeridiemGapY),
                       static_cast<std::int16_t>(gfx::kWidth - (2 * layout::kMargin)),
                       tt.pm ? "PM" : "AM",
                       gfx::font(FontId::kMedium),
                       Align::kRight,
                       Color::kBlack);
    }

    TextBuf<48> line;
    append_word(line, power_tag(s.power));
    append_word(line, format_battery(s.battery).view());
    TextBuf<16> steps;
    steps.put_uint(s.steps.today > kMaxShownSteps ? kMaxShownSteps : s.steps.today);
    if (s.steps.today > kMaxShownSteps) {
        steps.put_char('+');
    }
    steps.put(" steps");
    append_word(line, steps.view());
    append_word(line, sync_word(s.sync));
    c.text_aligned(0,
                   kStatusBaselineY,
                   gfx::kWidth,
                   line.view(),
                   gfx::font(FontId::kSmall),
                   Align::kCenter,
                   Color::kBlack);
}

} // namespace qz::faces
