// Shared helpers for UI screens: outcomes, input mapping, text fitting, list/field drawing.
#include "screen.hpp"

#include <algorithm>

namespace qz::ui {
namespace {

using gfx::Color;

constexpr std::int16_t kScreenW = gfx::kWidth;
constexpr std::int16_t kListTop = tuning::kTitleBarH + 2;
constexpr std::int16_t kListH = static_cast<std::int16_t>(kVisibleRows) * tuning::kRowH;
constexpr std::int16_t kScrollX = kScreenW - 3;
constexpr std::int16_t kRowTextPad = 6;
constexpr std::int16_t kMarkX = 9;
constexpr std::int16_t kMarkRadius = 4;
constexpr std::int16_t kMarkTextX = 20;
constexpr std::int16_t kValueRight = kScreenW - 8;
constexpr std::int16_t kMinThumbH = 8;
constexpr std::string_view kEllipsis = "...";

// Baseline that vertically centers a line of `f` inside a band of height `band`.
std::int16_t centered_baseline(std::int16_t top, std::int16_t band, const gfx::Font& f) noexcept {
    return static_cast<std::int16_t>(top + ((band - f.line_height) / 2) + f.ascent);
}

bool is_continuation(char c) noexcept {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

} // namespace

// ---- Outcome -------------------------------------------------------------------------------
Outcome Outcome::push(ScreenId id, std::uint8_t param) noexcept {
    Outcome o;
    o.nav = {Nav::Kind::kPush, id, param};
    return o;
}
Outcome Outcome::pop() noexcept {
    Outcome o;
    o.nav.kind = Nav::Kind::kPop;
    return o;
}
Outcome Outcome::home() noexcept {
    Outcome o;
    o.nav.kind = Nav::Kind::kHome;
    return o;
}
Outcome& Outcome::with(const Action& action) noexcept {
    const bool added = actions.push_back(action);
    QZ_ASSERT(added);
    return *this;
}

Action make_action(ActionKind kind) noexcept {
    Action a;
    a.kind = kind;
    return a;
}
Action make_set_setting(settings::Key key, std::string_view value) noexcept {
    Action a = make_action(ActionKind::kSetSetting);
    a.key = key;
    const bool fits = a.value.assign(value);
    QZ_ASSERT(fits); // values are produced by the UI itself (zone names <= 40 chars)
    return a;
}

// ---- input ---------------------------------------------------------------------------------
bool is_click(const model::InputEvent& e, model::Button b) noexcept {
    return e.button == b && e.kind == model::InputKind::kClick;
}
int step_direction(const model::InputEvent& e) noexcept {
    if (e.button == model::Button::kUp) {
        return 1;
    }
    return e.button == model::Button::kDown ? -1 : 0;
}
int list_direction(const model::InputEvent& e) noexcept {
    return -step_direction(e);
}
std::uint32_t step_multiplier(const model::InputEvent& e) noexcept {
    return e.held_ms >= tuning::kAccelAfterMs ? tuning::kAccelFactor : 1U;
}
std::size_t wrap_index(std::size_t index, int delta, std::size_t count) noexcept {
    QZ_ASSERT(count > 0);
    const auto n = static_cast<std::int64_t>(count);
    const std::int64_t v = (((static_cast<std::int64_t>(index) + delta) % n) + n) % n;
    return static_cast<std::size_t>(v);
}

// ---- text ----------------------------------------------------------------------------------
FixedString<48> fit_text(std::string_view s, const gfx::Font& f, std::int16_t max_width) noexcept {
    FixedString<48> out;
    if (s.size() > FixedString<48>::capacity() - kEllipsis.size()) {
        s = s.substr(0, FixedString<48>::capacity() - kEllipsis.size());
        while (!s.empty() && is_continuation(s.back())) {
            s.remove_suffix(1);
        }
        // `s` may now end in a lead byte; the loop below re-checks widths on whole code points.
    }
    if (gfx::Canvas::text_width(s, f) <= max_width) {
        (void)out.assign(s); // fits by construction (<= 45 bytes)
        return out;
    }
    const std::int16_t dots = gfx::Canvas::text_width(kEllipsis, f);
    std::size_t n = s.size();
    while (n > 0) {
        --n;
        while (n > 0 && is_continuation(s[n])) {
            --n; // cut only at code-point starts
        }
        if (gfx::Canvas::text_width(s.substr(0, n), f) + dots <= max_width) {
            break;
        }
    }
    TextBuilder<48> b;
    b.put(s.substr(0, n)).put(kEllipsis);
    (void)out.assign(b.view()); // <= 48 bytes
    return out;
}

// ---- drawing -------------------------------------------------------------------------------
void draw_title_bar(gfx::Canvas& canvas, std::string_view title) noexcept {
    const gfx::Font& f = gfx::font(gfx::FontId::kMedium);
    canvas.fill_rect({0, 0, kScreenW, tuning::kTitleBarH}, Color::kBlack);
    const auto text = fit_text(title, f, kScreenW - (2 * kRowTextPad));
    canvas.text_aligned(0,
                        centered_baseline(0, tuning::kTitleBarH, f),
                        kScreenW,
                        text.view(),
                        f,
                        gfx::Align::kCenter,
                        Color::kWhite);
}

void draw_caption(gfx::Canvas& canvas, std::int16_t baseline, std::string_view text) noexcept {
    const gfx::Font& f = gfx::font(gfx::FontId::kSmall);
    const auto fitted = fit_text(text, f, kScreenW - (2 * kRowTextPad));
    canvas.text_aligned(
        0, baseline, kScreenW, fitted.view(), f, gfx::Align::kCenter, Color::kBlack);
}

void draw_hint(gfx::Canvas& canvas, std::string_view hint) noexcept {
    draw_caption(canvas, tuning::kHintBaseline, hint);
}

void draw_row(gfx::Canvas& canvas, std::int16_t y, const ListRow& row, bool selected) noexcept {
    const gfx::Font& f = gfx::font(gfx::FontId::kMedium);
    const Color fg = selected ? Color::kWhite : Color::kBlack;
    if (selected) {
        canvas.fill_rect({0, y, static_cast<std::int16_t>(kScrollX - 1), tuning::kRowH},
                         Color::kBlack);
    }
    std::int16_t text_x = kRowTextPad;
    if (row.mark >= 0) {
        canvas.circle(kMarkX,
                      static_cast<std::int16_t>(y + (tuning::kRowH / 2)),
                      kMarkRadius,
                      row.mark == 1,
                      fg);
        text_x = kMarkTextX;
    }
    const std::int16_t baseline = centered_baseline(y, tuning::kRowH, f);
    std::int16_t label_right = kValueRight;
    if (!row.value.view().empty()) {
        const std::int16_t vw = gfx::Canvas::text_width(row.value.view(), f);
        canvas.text(static_cast<std::int16_t>(kValueRight - vw), baseline, row.value.view(), f, fg);
        label_right = static_cast<std::int16_t>(kValueRight - vw - kRowTextPad);
    }
    const auto label =
        fit_text(row.label.view(), f, static_cast<std::int16_t>(label_right - text_x));
    canvas.text(text_x, baseline, label.view(), f, fg);
}

std::size_t window_first(std::size_t selected, std::size_t count) noexcept {
    if (count <= kVisibleRows) {
        return 0;
    }
    const std::size_t half = kVisibleRows / 2;
    const std::size_t first = selected > half ? selected - half : 0;
    return std::min(first, count - kVisibleRows);
}

void draw_scrollbar(gfx::Canvas& canvas, std::size_t first, std::size_t count) noexcept {
    if (count <= kVisibleRows) {
        return;
    }
    canvas.vline(kScrollX, kListTop, kListH, Color::kBlack);
    const auto total = static_cast<std::int32_t>(count);
    const auto thumb_h = static_cast<std::int16_t>(std::max<std::int32_t>(
        kMinThumbH, kListH * static_cast<std::int32_t>(kVisibleRows) / total));
    const std::int32_t travel = kListH - thumb_h;
    const std::int32_t offset = (travel * static_cast<std::int32_t>(first)) /
                                static_cast<std::int32_t>(count - kVisibleRows);
    const auto thumb_y = static_cast<std::int16_t>(kListTop + offset);
    canvas.fill_rect({static_cast<std::int16_t>(kScrollX - 1), thumb_y, 3, thumb_h}, Color::kBlack);
}

void draw_marked_text(gfx::Canvas& canvas,
                      std::int16_t baseline,
                      const gfx::Font& font,
                      std::string_view text,
                      std::size_t mark_begin,
                      std::size_t mark_len) noexcept {
    mark_begin = std::min(mark_begin, text.size());
    mark_len = std::min(mark_len, text.size() - mark_begin);
    const auto before = text.substr(0, mark_begin);
    const auto marked = text.substr(mark_begin, mark_len);
    const auto after = text.substr(mark_begin + mark_len);
    const std::int16_t total = gfx::Canvas::text_width(text, font);
    const auto x0 = static_cast<std::int16_t>((kScreenW - total) / 2);
    const std::int16_t bw = gfx::Canvas::text_width(before, font);
    const std::int16_t mw = gfx::Canvas::text_width(marked, font);
    canvas.text(x0, baseline, before, font, Color::kBlack);
    if (mw > 0) {
        canvas.fill_rect({static_cast<std::int16_t>(x0 + bw - 1),
                          static_cast<std::int16_t>(baseline - font.ascent - 1),
                          static_cast<std::int16_t>(mw + 2),
                          static_cast<std::int16_t>(font.line_height + 2)},
                         Color::kBlack);
        canvas.text(static_cast<std::int16_t>(x0 + bw), baseline, marked, font, Color::kWhite);
    }
    canvas.text(static_cast<std::int16_t>(x0 + bw + mw), baseline, after, font, Color::kBlack);
}

} // namespace qz::ui
