// Dashboard watch face (id 5): status row, the time (Huge digits), date + weather, a seven-day step
// chart (six finished days plus today, goal as a dotted line) and today's steps against the goal.
#include "face_common.hpp"

#include <algorithm>

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;

constexpr Color kInk = Color::kBlack;
constexpr std::int16_t kTimeBase = 80;
constexpr std::int16_t kInfoTop = 86;
constexpr std::int16_t kChartTop = 112;
constexpr std::int16_t kChartH = 50;
constexpr std::int16_t kBarW = 18;
constexpr std::int16_t kBarGap = 8;
constexpr std::int16_t kChartX = 13;
constexpr std::int16_t kLabelBase = 174;
constexpr std::int16_t kFooterBase = 192;
constexpr std::size_t kDays = 7;

/// Checkerboard fill: finished days read lighter than today on a 1-bit panel.
void dither_rect(Canvas& c, const Rect& r) noexcept {
    for (std::int16_t y = r.y; y < r.y + r.h; ++y) {
        for (std::int16_t x = r.x; x < r.x + r.w; ++x) {
            if (((x + y) & 1) == 0) {
                c.pixel(x, y, kInk);
            }
        }
    }
}

void draw_chart(const ui::WatchState& s, Canvas& c) noexcept {
    const gfx::Font& small = gfx::font(FontId::kSmall);
    std::array<std::uint32_t, kDays> steps{};
    std::array<bool, kDays> known{};
    // Slot 6 = today, slot 5 = yesterday (history[0]), ... slot 0 = six days ago.
    steps[kDays - 1] = s.steps.today;
    known[kDays - 1] = true;
    const std::size_t count = std::min<std::size_t>(s.steps.history_count, kDays - 1);
    for (std::size_t i = 0; i < count; ++i) {
        steps[kDays - 2 - i] = s.steps.history[i].steps;
        known[kDays - 2 - i] = true;
    }
    std::uint32_t top = s.steps.goal;
    for (const std::uint32_t v : steps) {
        top = std::max(top, v);
    }
    top = std::max<std::uint32_t>(top, 1);
    const auto height_of = [&](std::uint32_t v) {
        return static_cast<std::int16_t>((static_cast<std::uint64_t>(v) * kChartH) / top);
    };
    const auto base_y = static_cast<std::int16_t>(kChartTop + kChartH);
    c.hline(layout::kMargin,
            base_y,
            static_cast<std::int16_t>(gfx::kWidth - (2 * layout::kMargin)),
            kInk);
    const time::DayNumber today = s.time_valid ? time::days_from_civil(s.local.date) : 0;
    for (std::size_t i = 0; i < kDays; ++i) {
        const auto x =
            static_cast<std::int16_t>(kChartX + (static_cast<std::int16_t>(i) * (kBarW + kBarGap)));
        const std::int16_t h = height_of(steps[i]);
        const Rect bar{x, static_cast<std::int16_t>(base_y - h), kBarW, h};
        if (i == kDays - 1) {
            c.fill_rect(bar, kInk);
        } else if (known[i] && h > 0) {
            c.rect(bar, kInk);
            dither_rect(c,
                        {static_cast<std::int16_t>(x + 2),
                         static_cast<std::int16_t>(bar.y + 2),
                         static_cast<std::int16_t>(kBarW - 4),
                         static_cast<std::int16_t>(h - 2)});
        }
        if (s.time_valid) {
            const auto day = today - static_cast<time::DayNumber>(kDays - 1 - i);
            const std::string_view name = time::weekday_name(time::weekday_from_days(day), true);
            c.text_aligned(x, kLabelBase, kBarW, name.substr(0, 2), small, Align::kCenter, kInk);
        }
    }
    if (s.steps.goal != 0) {
        const auto gy = static_cast<std::int16_t>(base_y - height_of(s.steps.goal));
        for (std::int16_t x = layout::kMargin; x < gfx::kWidth - layout::kMargin;
             x = static_cast<std::int16_t>(x + 4)) {
            c.hline(x, gy, 2, kInk);
        }
    }
}

} // namespace

void render_dashboard_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const gfx::Font& medium = gfx::font(FontId::kMedium);

    (void)draw_status_row(c, s, layout::kBarY);
    const TimeText tt = format_time(s);
    c.text_aligned(
        0, kTimeBase, gfx::kWidth, tt.text.view(), gfx::font(FontId::kHuge), Align::kCenter, kInk);
    if (tt.show_meridiem) {
        c.text_aligned(0,
                       30,
                       static_cast<std::int16_t>(gfx::kWidth - layout::kMargin),
                       tt.pm ? "PM" : "AM",
                       small,
                       Align::kRight,
                       kInk);
    }
    if (s.time_valid) {
        c.text(layout::kMargin,
               static_cast<std::int16_t>(kInfoTop + 15),
               format_date(s.local).view(),
               medium,
               kInk);
    }
    (void)draw_weather_compact(c, 112, kInfoTop, s, small);
    draw_chart(s, c);

    TextBuf<32> line;
    line.put(format_steps(s.steps.today).view());
    if (s.steps.goal != 0) {
        line.put(" / ");
        line.put_uint(s.steps.goal);
    }
    line.put(" steps");
    c.text_aligned(0, kFooterBase, gfx::kWidth, line.view(), medium, Align::kCenter, kInk);
}

} // namespace qz::faces
