// Analog watch face (id 2): dial with 60 minute dots and 12 hour ticks, hour and minute hands.
// Corners: power tag (top left), battery (top right), sync glyph (bottom left), AM/PM (bottom
// right, 12 h only). Inside the dial, spread out so a hand covers at most one item: weather under
// 12, weekday / day window / month at 3, steps and the goal bar at 9; 6 stays clear. Hands carry a
// white halo. Invalid time: no hands, "--:--" in the middle.
#include "face_common.hpp"

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;

constexpr Color kInk = Color::kBlack;
constexpr std::int16_t kCx = 100;
constexpr std::int16_t kCy = 100;
constexpr std::int16_t kDialR = 96;
constexpr std::int16_t kDotR = 91;
constexpr std::int16_t kTickOuter = 93;
constexpr std::int16_t kTickInner = 84;
constexpr std::int16_t kMajorTickInner = 79;
constexpr std::int16_t kMinuteLen = 78;
constexpr std::int16_t kMinuteW = 4;
constexpr std::int16_t kHourLen = 52;
constexpr std::int16_t kHourW = 7;
constexpr std::int16_t kTailLen = 12;
constexpr std::int16_t kHaloW = 4;
constexpr std::int16_t kHubR = 5;
constexpr std::int16_t kWeatherTop = 26;
constexpr Rect kDayWindow{131, 91, 30, 19};
constexpr std::int16_t kWeekdayBase = 87;
constexpr std::int16_t kMonthBase = 122;
constexpr std::int16_t kStepsCx = 50;
constexpr std::int16_t kStepsBase = 99;
constexpr std::int16_t kGoalBarY = 103;
constexpr std::int16_t kGoalBarW = 40;
constexpr std::int16_t kGoalBarH = 5;
constexpr std::int32_t kTenthsPerMinute = 60; ///< 6 degrees per minute
constexpr std::int32_t kTenthsPerHour = 300;  ///< 30 degrees per hour

/// Point at `len` px from the centre along `deg_tenths` (0 = 12 o'clock, clockwise).
void polar(std::int32_t deg_tenths, std::int32_t len, std::int16_t& x, std::int16_t& y) noexcept {
    x = static_cast<std::int16_t>(kCx + round_div(len * sin_e4(deg_tenths), kTrigOne));
    y = static_cast<std::int16_t>(kCy - round_div(len * cos_e4(deg_tenths), kTrigOne));
}

void draw_dial(Canvas& c) noexcept {
    c.circle(kCx, kCy, kDialR, false, kInk);
    for (std::int32_t m = 0; m < 60; ++m) {
        const std::int32_t a = m * kTenthsPerMinute;
        std::int16_t x0 = 0;
        std::int16_t y0 = 0;
        std::int16_t x1 = 0;
        std::int16_t y1 = 0;
        if (m % 5 != 0) {
            polar(a, kDotR, x0, y0);
            c.pixel(x0, y0, kInk);
            continue;
        }
        const bool major = m % 15 == 0;
        polar(a, major ? kMajorTickInner : kTickInner, x0, y0);
        polar(a, kTickOuter, x1, y1);
        thick_line(c, x0, y0, x1, y1, major ? 4 : 2);
    }
}

void draw_hand(Canvas& c, std::int32_t deg_tenths, std::int16_t len, std::int16_t width) noexcept {
    std::int16_t x0 = 0;
    std::int16_t y0 = 0;
    std::int16_t x1 = 0;
    std::int16_t y1 = 0;
    polar(deg_tenths + 1800, kTailLen, x0, y0);
    polar(deg_tenths, len, x1, y1);
    // White halo first: text under the hand stays readable on both sides of it.
    thick_line(c, x0, y0, x1, y1, static_cast<std::int16_t>(width + kHaloW), Color::kWhite);
    thick_line(c, x0, y0, x1, y1, width);
}

void draw_corners(const ui::WatchState& s, Canvas& c, const TimeText& tt) noexcept {
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const std::string_view tag = power_tag(s.power);
    if (!tag.empty()) {
        (void)draw_tag(c, 2, 11, tag);
    }
    c.text_aligned(
        0, 11, gfx::kWidth - 2, format_battery(s.battery).view(), small, Align::kRight, kInk);
    draw_sync_icon(
        c, 2, static_cast<std::int16_t>(gfx::kHeight - 2 - layout::kSyncIconSize), s.sync);
    if (tt.show_meridiem) {
        c.text_aligned(
            0, gfx::kHeight - 4, gfx::kWidth - 2, tt.pm ? "PM" : "AM", small, Align::kRight, kInk);
    }
}

void draw_weather(const ui::WatchState& s, Canvas& c) noexcept {
    if (!weather_visible(s)) {
        return;
    }
    const gfx::Font& small = gfx::font(FontId::kSmall);
    // Centre the block under 12: icon + gap + temperature (+ gap + age when stale).
    std::int32_t w = layout::kWeatherIconSize + 3 +
                     Canvas::text_width(format_temp(s.weather.temp_dc, s.temp_unit).view(), small);
    if (s.weather_freshness == model::WeatherFreshness::kStale) {
        w += 3 + Canvas::text_width(format_age(s.weather_age_s).view(), small);
    }
    (void)draw_weather_compact(c, static_cast<std::int16_t>(kCx - (w / 2)), kWeatherTop, s, small);
}

void draw_info(const ui::WatchState& s, Canvas& c) noexcept {
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const gfx::Font& medium = gfx::font(FontId::kMedium);
    c.rect(kDayWindow, kInk);
    if (s.time_valid) {
        TextBuf<4> day;
        day.put_uint(s.local.date.day);
        c.text_aligned(kDayWindow.x,
                       static_cast<std::int16_t>(kDayWindow.y + 15),
                       kDayWindow.w,
                       day.view(),
                       medium,
                       Align::kCenter,
                       kInk);
        // format_date() = "Tue 6 Oct": weekday above the window, month below it.
        const TextBuf<24> date = format_date(s.local);
        const std::string_view text = date.view();
        const std::size_t first = text.find(' ');
        const std::size_t last = text.rfind(' ');
        if (first != std::string_view::npos && last != std::string_view::npos && last > first) {
            c.text_aligned(kDayWindow.x,
                           kWeekdayBase,
                           kDayWindow.w,
                           text.substr(0, first),
                           small,
                           Align::kCenter,
                           kInk);
            c.text_aligned(kDayWindow.x,
                           kMonthBase,
                           kDayWindow.w,
                           text.substr(last + 1),
                           small,
                           Align::kCenter,
                           kInk);
        }
    }
    c.text_aligned(static_cast<std::int16_t>(kStepsCx - 30),
                   kStepsBase,
                   60,
                   format_steps(s.steps.today).view(),
                   small,
                   Align::kCenter,
                   kInk);
    if (s.steps.goal == 0) {
        return;
    }
    const Rect bar{
        static_cast<std::int16_t>(kStepsCx - (kGoalBarW / 2)), kGoalBarY, kGoalBarW, kGoalBarH};
    c.rect(bar, kInk);
    const std::uint32_t done = s.steps.today >= s.steps.goal ? s.steps.goal : s.steps.today;
    const auto fill = static_cast<std::int16_t>(
        (static_cast<std::uint64_t>(done) * static_cast<std::uint64_t>(kGoalBarW - 2)) /
        s.steps.goal);
    c.fill_rect({static_cast<std::int16_t>(bar.x + 1),
                 static_cast<std::int16_t>(bar.y + 1),
                 fill,
                 static_cast<std::int16_t>(kGoalBarH - 2)},
                kInk);
}

} // namespace

void render_analog_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);
    const TimeText tt = format_time(s);
    draw_dial(c);
    draw_corners(s, c, tt);
    draw_weather(s, c);
    draw_info(s, c);
    if (!s.time_valid || tt.text.view() == "--:--") {
        c.fill_rect({58, 88, 84, 24}, Color::kWhite);
        c.text_aligned(
            0, 106, gfx::kWidth, "--:--", gfx::font(FontId::kLarge), Align::kCenter, kInk);
        return;
    }
    const std::int32_t minute = s.local.time.minute % 60;
    const std::int32_t hour = s.local.time.hour % 12;
    draw_hand(c, (hour * kTenthsPerHour) + (minute * kTenthsPerHour / 60), kHourLen, kHourW);
    draw_hand(c, minute * kTenthsPerMinute, kMinuteLen, kMinuteW);
    c.circle(kCx, kCy, kHubR, true, kInk);
    c.circle(kCx, kCy, 1, true, Color::kWhite);
}

} // namespace qz::faces
