// Stacked watch face (id 3): hours above minutes in the Giant digits, a steps-goal bar between
// them, status row on top (power tag, battery, weather, sync) and the date + steps at the bottom.
// 12 h: no leading zero on the hour, AM/PM beside it. Invalid time: "--" over "--".
#include "face_common.hpp"

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;

constexpr Color kInk = Color::kBlack;
constexpr std::int16_t kHourBase = 88;
constexpr std::int16_t kMinuteBase = 168;
constexpr Rect kGoalBar{layout::kMargin, 92, gfx::kWidth - (2 * layout::kMargin), 6};
constexpr std::int16_t kFooterBase = 194;
constexpr std::int16_t kMeridiemX = 168;

void two_digits(TextBuf<4>& out, std::uint32_t v, bool pad) noexcept {
    if (pad || v >= 10) {
        out.put_char(static_cast<char>('0' + ((v / 10) % 10)));
    }
    out.put_char(static_cast<char>('0' + (v % 10)));
}

void draw_goal_bar(const ui::WatchState& s, Canvas& c) noexcept {
    if (s.steps.goal == 0) {
        c.hline(
            kGoalBar.x, static_cast<std::int16_t>(kGoalBar.y + (kGoalBar.h / 2)), kGoalBar.w, kInk);
        return;
    }
    c.rect(kGoalBar, kInk);
    const std::uint32_t done = s.steps.today >= s.steps.goal ? s.steps.goal : s.steps.today;
    const auto fill = static_cast<std::int16_t>(
        (static_cast<std::uint64_t>(done) * static_cast<std::uint64_t>(kGoalBar.w - 2)) /
        s.steps.goal);
    c.fill_rect({static_cast<std::int16_t>(kGoalBar.x + 1),
                 static_cast<std::int16_t>(kGoalBar.y + 1),
                 fill,
                 static_cast<std::int16_t>(kGoalBar.h - 2)},
                kInk);
}

} // namespace

void render_stacked_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);
    const gfx::Font& giant = gfx::font(FontId::kGiant);
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const gfx::Font& medium = gfx::font(FontId::kMedium);

    const std::int16_t free_x = draw_status_row(c, s, layout::kBarY);
    (void)draw_weather_compact(c, static_cast<std::int16_t>(free_x + 4), 0, s, small);

    const TimeText tt = format_time(s);
    TextBuf<4> hours;
    TextBuf<4> minutes;
    if (tt.text.view() == "--:--") {
        hours.put("--");
        minutes.put("--");
    } else {
        std::uint32_t h = s.local.time.hour;
        const bool twelve = s.hour_format == model::HourFormat::k12h;
        if (twelve) {
            h %= 12;
            h = h == 0 ? 12 : h;
        }
        two_digits(hours, h, !twelve);
        two_digits(minutes, s.local.time.minute, true);
    }
    c.text_aligned(0, kHourBase, gfx::kWidth, hours.view(), giant, Align::kCenter, kInk);
    c.text_aligned(0, kMinuteBase, gfx::kWidth, minutes.view(), giant, Align::kCenter, kInk);
    if (tt.show_meridiem) {
        c.text(kMeridiemX, kHourBase, tt.pm ? "PM" : "AM", medium, kInk);
    }
    draw_goal_bar(s, c);

    if (s.time_valid) {
        c.text(layout::kMargin, kFooterBase, format_date(s.local).view(), medium, kInk);
    }
    TextBuf<16> steps;
    steps.put(format_steps(s.steps.today).view());
    steps.put(" st");
    c.text_aligned(0,
                   kFooterBase,
                   static_cast<std::int16_t>(gfx::kWidth - layout::kMargin),
                   steps.view(),
                   medium,
                   Align::kRight,
                   kInk);
}

} // namespace qz::faces
