// Default watch face (id 0): status bar, large time, date, steps with goal bar, weather row.
//
//   y   0..15  battery icon + % (+ power tag)            [AM/PM]  sync icon
//   y  22..82  time, kHuge digits (60 px tall, >= 48 px readability rule)
//   y  88..    date line, kLarge
//   y 124      rule
//   y 130..    steps (kLarge) and "/goal" (kMedium), goal progress bar
//   y 176..    weather icon + temperature (+ "Nh old" when stale) + H/L, only when not hidden
#include "face_common.hpp"

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;

constexpr Color kInk = Color::kBlack;

constexpr std::int16_t kDateTopY = 90;
constexpr std::int16_t kRuleY = 126;
constexpr std::int16_t kStepsTopY = 132;
constexpr std::int16_t kGoalBarY = 166;
constexpr std::int16_t kGoalBarH = 7;
constexpr std::int16_t kWeatherY = 177;
constexpr std::int16_t kGap = 4;
constexpr std::uint32_t kMaxShownCount = 99999; ///< larger step counts show "99999+"

void put_capped(TextBuf<12>& t, std::uint32_t v) noexcept {
    t.put_uint(v > kMaxShownCount ? kMaxShownCount : v);
    if (v > kMaxShownCount) {
        t.put_char('+');
    }
}

void draw_status_bar(const ui::WatchState& s, Canvas& c, const TimeText& tt) noexcept {
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const auto base = static_cast<std::int16_t>(layout::kBarY + small.ascent);

    // Left cluster: battery icon, percentage, power tag.
    auto x = layout::kMargin;
    draw_battery_icon(c, x, layout::kBarY, s.battery);
    x = static_cast<std::int16_t>(x + layout::kBatteryIconW + kGap);
    x = static_cast<std::int16_t>(
        x + c.text(x, base, format_battery(s.battery).view(), small, kInk) + kGap);
    const std::string_view tag = power_tag(s.power);
    if (!tag.empty()) {
        draw_tag(c, x, base, tag);
    }

    // Right cluster: sync glyph, AM/PM to its left.
    const auto sync_x =
        static_cast<std::int16_t>(gfx::kWidth - layout::kMargin - layout::kSyncIconSize);
    draw_sync_icon(c, sync_x, layout::kBarY, s.sync);
    if (tt.show_meridiem) {
        c.text_aligned(0,
                       base,
                       static_cast<std::int16_t>(sync_x - kGap),
                       tt.pm ? "PM" : "AM",
                       small,
                       Align::kRight,
                       kInk);
    }
}

void draw_steps(const ui::WatchState& s, Canvas& c) noexcept {
    const gfx::Font& large = gfx::font(FontId::kLarge);
    const gfx::Font& medium = gfx::font(FontId::kMedium);
    const auto base = static_cast<std::int16_t>(kStepsTopY + large.ascent);
    constexpr auto kRight = static_cast<std::int16_t>(gfx::kWidth - (2 * layout::kMargin));

    TextBuf<12> today;
    put_capped(today, s.steps.today);
    c.text(layout::kMargin, base, today.view(), large, kInk);

    if (s.steps.goal == 0) {
        c.text_aligned(layout::kMargin, base, kRight, "steps", medium, Align::kRight, kInk);
        return;
    }
    TextBuf<12> goal;
    goal.put_char('/');
    put_capped(goal, s.steps.goal);
    c.text_aligned(layout::kMargin, base, kRight, goal.view(), medium, Align::kRight, kInk);

    // Progress bar: outline + proportional fill (integer math; clamped at the goal).
    const auto bar_w = static_cast<std::int16_t>(gfx::kWidth - (2 * layout::kMargin));
    c.rect(Rect{layout::kMargin, kGoalBarY, bar_w, kGoalBarH}, kInk);
    const std::uint64_t done = s.steps.today < s.steps.goal ? s.steps.today : s.steps.goal;
    const auto inner_w = static_cast<std::uint64_t>(bar_w - 4);
    const auto fill = static_cast<std::int16_t>((done * inner_w) / s.steps.goal);
    c.fill_rect(Rect{static_cast<std::int16_t>(layout::kMargin + 2),
                     static_cast<std::int16_t>(kGoalBarY + 2),
                     fill,
                     static_cast<std::int16_t>(kGoalBarH - 4)},
                kInk);
}

void draw_weather(const ui::WatchState& s, Canvas& c) noexcept {
    if (!weather_visible(s)) {
        return;
    }
    const gfx::Font& medium = gfx::font(FontId::kMedium);
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const bool stale = s.weather_freshness == model::WeatherFreshness::kStale;
    const auto base =
        static_cast<std::int16_t>(kWeatherY + ((layout::kWeatherIconSize + medium.ascent) / 2));

    draw_weather_icon(c, layout::kMargin, kWeatherY, s.weather.condition);
    auto x = static_cast<std::int16_t>(layout::kMargin + layout::kWeatherIconSize + (2 * kGap));
    const std::int16_t adv =
        c.text(x, base, format_temp(s.weather.temp_dc, s.temp_unit).view(), medium, kInk);
    if (stale) {
        // Stale mark: dotted underline under the temperature plus its age.
        for (std::int16_t dx = 0; dx < adv; dx = static_cast<std::int16_t>(dx + 2)) {
            c.pixel(static_cast<std::int16_t>(x + dx), static_cast<std::int16_t>(base + 3), kInk);
        }
        x = static_cast<std::int16_t>(x + adv + kGap);
        c.text(x, base, format_age(s.weather_age_s).view(), small, kInk);
    }
    if (s.weather_high_low && s.weather.has_high_low != 0 && !stale) {
        c.text_aligned(0,
                       base,
                       static_cast<std::int16_t>(gfx::kWidth - layout::kMargin),
                       format_high_low(s.weather, s.temp_unit).view(),
                       small,
                       Align::kRight,
                       kInk);
    }
}

} // namespace

void render_default_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);

    const TimeText tt = format_time(s);
    draw_status_bar(s, c, tt);

    c.text_aligned(0,
                   layout::kTimeBaselineY,
                   gfx::kWidth,
                   tt.text.view(),
                   gfx::font(FontId::kHuge),
                   Align::kCenter,
                   kInk);

    const gfx::Font& large = gfx::font(FontId::kLarge);
    const auto date_base = static_cast<std::int16_t>(kDateTopY + large.ascent);
    if (s.time_valid) {
        c.text_aligned(
            0, date_base, gfx::kWidth, format_date(s.local).view(), large, Align::kCenter, kInk);
    } else {
        c.text_aligned(0, date_base, gfx::kWidth, "No time set", large, Align::kCenter, kInk);
    }
    c.hline(layout::kMargin,
            kRuleY,
            static_cast<std::int16_t>(gfx::kWidth - (2 * layout::kMargin)),
            kInk);

    draw_steps(s, c);
    draw_weather(s, c);
}

} // namespace qz::faces
