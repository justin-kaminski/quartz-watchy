// Day-progress watch face (id 6): a ring along the screen edge fills clockwise from midnight at the
// top centre (noon at the bottom), with hour notches and, when a location is set, inward markers at
// sunrise and sunset. Centre: status, time (Huge digits), date, sun times, steps, weather.
#include "face_common.hpp"

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;

constexpr Color kInk = Color::kBlack;
constexpr std::int16_t kRingInset = 2;                                  ///< outer edge of the ring
constexpr std::int16_t kRingW = 5;                                      ///< filled ring thickness
constexpr std::int16_t kSide = gfx::kWidth - (2 * kRingInset) - kRingW; ///< centre-line square side
constexpr std::int32_t kPerimeter = 4 * kSide;
constexpr std::int32_t kMinutesPerDay = 1440;
constexpr std::int16_t kMajorNotch = 5;
constexpr std::int16_t kSunMark = 9;
constexpr std::int16_t kStatusY = 12;
constexpr std::int16_t kInnerInset = 12; ///< status row clear of the ring band
constexpr std::int16_t kTimeBase = 92;
constexpr std::int16_t kDateBase = 116;
constexpr std::int16_t kSunBase = 132;
constexpr std::int16_t kStepsBase = 148;
constexpr std::int16_t kWeatherTop = 156;

struct Pt {
    std::int16_t x;
    std::int16_t y;
    std::int8_t nx; ///< inward normal
    std::int8_t ny;
};

/// Position on the ring's centre line at distance `d` from the top centre, clockwise.
Pt ring_point(std::int32_t d) noexcept {
    constexpr auto lo = static_cast<std::int32_t>(kRingInset + (kRingW / 2));
    constexpr std::int32_t hi = lo + kSide;
    constexpr std::int32_t half = kSide / 2;
    d %= kPerimeter;
    if (d < 0) {
        d += kPerimeter;
    }
    if (d < half) {
        return {static_cast<std::int16_t>(lo + half + d), static_cast<std::int16_t>(lo), 0, 1};
    }
    d -= half;
    if (d < kSide) {
        return {static_cast<std::int16_t>(hi), static_cast<std::int16_t>(lo + d), -1, 0};
    }
    d -= kSide;
    if (d < kSide) {
        return {static_cast<std::int16_t>(hi - d), static_cast<std::int16_t>(hi), 0, -1};
    }
    d -= kSide;
    if (d < kSide) {
        return {static_cast<std::int16_t>(lo), static_cast<std::int16_t>(hi - d), 1, 0};
    }
    d -= kSide;
    return {static_cast<std::int16_t>(lo + d), static_cast<std::int16_t>(lo), 0, 1};
}

std::int32_t minutes_to_distance(std::int32_t minutes) noexcept {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(minutes) * kPerimeter) /
                                     kMinutesPerDay);
}

/// Hour tick inside the band only, in the colour that contrasts with the fill at that point.
void band_tick(Canvas& c, std::int32_t minutes, bool filled) noexcept {
    const Pt p = ring_point(minutes_to_distance(minutes));
    const auto r = static_cast<std::int16_t>(kRingW / 2);
    const Color col = filled ? Color::kWhite : kInk;
    if (p.nx != 0) {
        c.hline(static_cast<std::int16_t>(p.x - r), p.y, kRingW, col);
    } else {
        c.vline(p.x, static_cast<std::int16_t>(p.y - r), kRingW, col);
    }
}

/// Mark reaching `len` px inward from the ring's centre line.
void notch(Canvas& c, std::int32_t minutes, std::int16_t len, std::int16_t width) noexcept {
    const Pt p = ring_point(minutes_to_distance(minutes));
    const auto x1 = static_cast<std::int16_t>(p.x + (p.nx * (len + (kRingW / 2))));
    const auto y1 = static_cast<std::int16_t>(p.y + (p.ny * (len + (kRingW / 2))));
    thick_line(c, p.x, p.y, x1, y1, width);
}

void draw_ring(const ui::WatchState& s, Canvas& c) noexcept {
    // Track: 1 px outline of the full ring.
    c.rect({kRingInset,
            kRingInset,
            static_cast<std::int16_t>(gfx::kWidth - (2 * kRingInset)),
            static_cast<std::int16_t>(gfx::kHeight - (2 * kRingInset))},
           kInk);
    c.rect({static_cast<std::int16_t>(kRingInset + kRingW - 1),
            static_cast<std::int16_t>(kRingInset + kRingW - 1),
            static_cast<std::int16_t>(gfx::kWidth - (2 * (kRingInset + kRingW - 1))),
            static_cast<std::int16_t>(gfx::kHeight - (2 * (kRingInset + kRingW - 1)))},
           kInk);
    const std::int32_t now = s.time_valid ? (s.local.time.hour * 60) + s.local.time.minute : 0;
    if (s.time_valid) {
        const std::int32_t filled = minutes_to_distance(now);
        for (std::int32_t d = 0; d < filled; ++d) {
            const Pt p = ring_point(d);
            c.fill_rect({static_cast<std::int16_t>(p.x - (kRingW / 2)),
                         static_cast<std::int16_t>(p.y - (kRingW / 2)),
                         kRingW,
                         kRingW},
                        kInk);
        }
    }
    for (std::int32_t h = 0; h < 24; ++h) {
        if (h % 6 == 0) {
            notch(c, h * 60, kMajorNotch, 2); // quarter-day marks reach into the face
        } else {
            band_tick(c, h * 60, h * 60 < now);
        }
    }
}

void draw_sun(const ui::WatchState& s, Canvas& c) noexcept {
    if (!s.time_valid || s.settings == nullptr || !s.settings->location_set) {
        return;
    }
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const SunTimes sun = sun_times(s.local, s.settings->location);
    TextBuf<32> line;
    switch (sun.kind) {
        case SunTimes::Kind::kPolarDay:
            line.put("sun up all day");
            break;
        case SunTimes::Kind::kPolarNight:
            line.put("sun down all day");
            break;
        case SunTimes::Kind::kNormal:
            notch(c, sun.rise_min, kSunMark, 4);
            notch(c, sun.set_min, kSunMark, 4);
            line.put("rise ");
            line.put(format_clock_minutes(sun.rise_min, s.hour_format).view());
            line.put("  set ");
            line.put(format_clock_minutes(sun.set_min, s.hour_format).view());
            break;
    }
    c.text_aligned(0, kSunBase, gfx::kWidth, line.view(), small, Align::kCenter, kInk);
}

} // namespace

void render_progress_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const gfx::Font& medium = gfx::font(FontId::kMedium);

    draw_ring(s, c);
    c.set_clip({8, 8, 184, 184}); // inside the ring band
    // Status row narrowed to the inner area: draw it, then nothing else uses y 14..29.
    {
        const TimeText tt = format_time(s);
        (void)draw_status_row(c, s, kStatusY, kInnerInset);
        c.text_aligned(0,
                       kTimeBase,
                       gfx::kWidth,
                       tt.text.view(),
                       gfx::font(FontId::kHuge),
                       Align::kCenter,
                       kInk);
        if (tt.show_meridiem) {
            c.text_aligned(0, kDateBase, 184, tt.pm ? "PM" : "AM", small, Align::kRight, kInk);
        }
    }
    if (s.time_valid) {
        c.text_aligned(
            0, kDateBase, gfx::kWidth, format_date(s.local).view(), medium, Align::kCenter, kInk);
    }
    draw_sun(s, c);
    TextBuf<32> steps;
    steps.put(format_steps(s.steps.today).view());
    if (s.steps.goal != 0) {
        steps.put(" / ");
        steps.put_uint(s.steps.goal);
    }
    steps.put(" steps");
    if (s.steps.goal != 0 && s.steps.today >= s.steps.goal) {
        steps.put(" *");
    }
    c.text_aligned(0, kStepsBase, gfx::kWidth, steps.view(), small, Align::kCenter, kInk);
    (void)draw_weather_compact(c, 60, kWeatherTop, s, small);
    c.reset_clip();
}

} // namespace qz::faces
