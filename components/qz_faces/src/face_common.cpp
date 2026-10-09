// Shared formatting and icon drawing for the watch faces. Integer-only (no float/double), no heap.
// Icons are drawn from primitives (no bitmaps): geometry constants below are pixel coordinates in
// each icon's own box.
#include "face_common.hpp"

#include "qz/time/civil.hpp"

namespace qz::faces {

using gfx::Canvas;
using gfx::Color;
using gfx::FontId;
using gfx::Rect;
using model::SyncIndicator;
using model::WeatherCondition;

namespace {

constexpr Color kInk = Color::kBlack;
constexpr Color kPaper = Color::kWhite;

constexpr std::int32_t kDeciPerDegree = 10;
constexpr std::int32_t kFahrenheitNum = 9;
constexpr std::int32_t kFahrenheitDen = 5;
constexpr std::int32_t kFahrenheitOffsetDeci = 320;
constexpr std::uint32_t kSecondsPerHour = 3600;
constexpr std::uint32_t kMaxAgeHours = 99;
constexpr char kDegreeSign[] = "°"; ///< UTF-8, Latin-1 range of every font

[[nodiscard]] std::int32_t floor_div(std::int32_t a, std::int32_t b) noexcept {
    std::int32_t q = a / b;
    if ((a % b) < 0) {
        --q;
    }
    return q;
}

} // namespace

std::int32_t round_div(std::int32_t num, std::int32_t den) noexcept {
    return floor_div((2 * num) + den, 2 * den);
}

std::int32_t temp_whole(std::int16_t temp_dc, model::TempUnit unit) noexcept {
    const auto dc = static_cast<std::int32_t>(temp_dc);
    if (unit == model::TempUnit::kFahrenheit) {
        // F = C * 9/5 + 32; in deci-degrees: dF = dC * 9/5 + 320; whole = round(dF / 10).
        return round_div((dc * kFahrenheitNum) + (kFahrenheitOffsetDeci * kFahrenheitDen),
                         kDeciPerDegree * kFahrenheitDen);
    }
    return round_div(dc, kDeciPerDegree);
}

TextBuf<16> format_temp(std::int16_t temp_dc, model::TempUnit unit) noexcept {
    TextBuf<16> t;
    t.put_int(temp_whole(temp_dc, unit));
    t.put(kDegreeSign);
    t.put_char(unit == model::TempUnit::kFahrenheit ? 'F' : 'C');
    return t;
}

TextBuf<24> format_high_low(const model::WeatherReport& wx, model::TempUnit unit) noexcept {
    TextBuf<24> t;
    t.put_char('H');
    t.put_int(temp_whole(wx.high_dc, unit));
    t.put(" L");
    t.put_int(temp_whole(wx.low_dc, unit));
    return t;
}

TextBuf<16> format_age(std::uint32_t age_s) noexcept {
    TextBuf<16> t;
    const std::uint32_t hours = age_s / kSecondsPerHour;
    if (hours == 0) {
        t.put("<1h old");
    } else if (hours > kMaxAgeHours) {
        t.put("99h+ old");
    } else {
        t.put_uint(hours);
        t.put("h old");
    }
    return t;
}

TextBuf<24> format_date(const time::LocalDateTime& local) noexcept {
    TextBuf<24> t;
    const std::string_view wd = time::weekday_name(local.weekday, true);
    const std::string_view mon = time::month_name(local.date.month, true);
    t.put(wd.empty() ? std::string_view("?") : wd);
    t.put_char(' ');
    t.put_uint(local.date.day);
    t.put_char(' ');
    t.put(mon.empty() ? std::string_view("?") : mon);
    return t;
}

TimeText format_time(const ui::WatchState& s) noexcept {
    TimeText out;
    if (s.time_valid) {
        std::array<char, 8> raw{};
        bool pm = false;
        const std::size_t n = time::format_hhmm(raw, s.local.time, s.hour_format, &pm);
        if (n > 0) {
            out.text.put(std::string_view(raw.data(), n));
            out.show_meridiem = (s.hour_format == time::HourFormat::k12h);
            out.pm = pm;
            return out;
        }
    }
    out.text.put("--:--");
    return out;
}

TextBuf<8> format_battery(const model::BatteryStatus& b) noexcept {
    TextBuf<8> t;
    if (b.usb_present) {
        t.put(b.charging ? "CHG" : "USB");
    } else if (!b.valid) {
        t.put("--%");
    } else {
        t.put_uint(b.percent > 100U ? 100U : b.percent);
        t.put_char('%');
    }
    return t;
}

std::string_view power_tag(model::PowerLevel level) noexcept {
    switch (level) {
        case model::PowerLevel::kNormal:
            return {};
        case model::PowerLevel::kLow:
            return "LOW";
        case model::PowerLevel::kSaver:
            return "SAVER";
        case model::PowerLevel::kCritical:
            return "CRIT";
    }
    return {};
}

std::string_view sync_word(SyncIndicator sync) noexcept {
    switch (sync) {
        case SyncIndicator::kNone:
            return {};
        case SyncIndicator::kNeverSynced:
            return "no sync";
        case SyncIndicator::kLastFailed:
            return "sync fail";
        case SyncIndicator::kStale:
            return "sync old";
        case SyncIndicator::kOk:
            return "synced";
    }
    return {};
}

bool weather_visible(const ui::WatchState& s) noexcept {
    return s.weather.valid != 0 && s.weather_freshness != model::WeatherFreshness::kHidden;
}

// ---- icons ---------------------------------------------------------------------------------

namespace {

/// Draws in an icon's local coordinate system (origin = the icon's top-left corner). Takes plain
/// ints so the geometry below stays readable; each call narrows once, here.
class Brush {
public:
    Brush(Canvas& c, std::int16_t x, std::int16_t y) noexcept : c_(c), ox_(x), oy_(y) {}
    void line(std::int32_t x0,
              std::int32_t y0,
              std::int32_t x1,
              std::int32_t y1,
              Color col = kInk) const noexcept {
        c_.line(gx(x0), gy(y0), gx(x1), gy(y1), col);
    }
    void hline(std::int32_t x, std::int32_t y, std::int32_t w, Color col = kInk) const noexcept {
        c_.hline(gx(x), gy(y), narrow(w), col);
    }
    void vline(std::int32_t x, std::int32_t y, std::int32_t h, Color col = kInk) const noexcept {
        c_.vline(gx(x), gy(y), narrow(h), col);
    }
    void rect(std::int32_t x,
              std::int32_t y,
              std::int32_t w,
              std::int32_t h,
              Color col = kInk) const noexcept {
        c_.rect(Rect{gx(x), gy(y), narrow(w), narrow(h)}, col);
    }
    void fill(std::int32_t x,
              std::int32_t y,
              std::int32_t w,
              std::int32_t h,
              Color col = kInk) const noexcept {
        c_.fill_rect(Rect{gx(x), gy(y), narrow(w), narrow(h)}, col);
    }
    void circle(std::int32_t cx,
                std::int32_t cy,
                std::int32_t r,
                bool filled,
                Color col = kInk) const noexcept {
        c_.circle(gx(cx), gy(cy), narrow(r), filled, col);
    }
    void
    text(std::int32_t x, std::int32_t baseline, std::string_view t, FontId font) const noexcept {
        c_.text(gx(x), gy(baseline), t, gfx::font(font), kInk);
    }

private:
    static std::int16_t narrow(std::int32_t v) noexcept { return static_cast<std::int16_t>(v); }
    [[nodiscard]] std::int16_t gx(std::int32_t x) const noexcept { return narrow(ox_ + x); }
    [[nodiscard]] std::int16_t gy(std::int32_t y) const noexcept { return narrow(oy_ + y); }

    Canvas& c_;
    std::int16_t ox_;
    std::int16_t oy_;
};

} // namespace

void draw_battery_icon(Canvas& c,
                       std::int16_t x,
                       std::int16_t y,
                       const model::BatteryStatus& b) noexcept {
    constexpr std::int32_t kBodyW = 24;
    constexpr std::int32_t kBodyH = 12;
    constexpr std::int32_t kInnerW = 20;
    constexpr std::int32_t kInnerH = 8;
    const Brush p(c, x, y);
    p.rect(0, 0, kBodyW, kBodyH);
    p.fill(kBodyW, 3, 2, 6); // terminal nub
    if (b.usb_present) {
        // Lightning bolt: the percentage is meaningless while the charger is attached.
        p.line(13, 2, 9, 6);
        p.line(14, 2, 10, 6);
        p.line(9, 6, 15, 6);
        p.line(15, 6, 11, 10);
        return;
    }
    if (!b.valid) {
        return; // empty body: no sample yet
    }
    const std::int32_t pct = b.percent > 100U ? 100 : b.percent;
    std::int32_t fill = (pct * kInnerW) / 100;
    if (pct > 0 && fill == 0) {
        fill = 1;
    }
    p.fill(2, 2, fill, kInnerH);
}

void draw_sync_icon(Canvas& c, std::int16_t x, std::int16_t y, SyncIndicator sync) noexcept {
    constexpr std::int32_t kRadius = 7;
    if (sync == SyncIndicator::kNone) {
        return;
    }
    const Brush p(c, x, y);
    switch (sync) {
        case SyncIndicator::kOk: // check mark in a ring
            p.circle(kRadius, kRadius, kRadius, false);
            p.line(4, 7, 6, 10);
            p.line(4, 8, 6, 11);
            p.line(6, 10, 11, 4);
            p.line(6, 11, 11, 5);
            break;
        case SyncIndicator::kStale: // clock face: the data is old
            p.circle(kRadius, kRadius, kRadius, false);
            p.vline(kRadius, 3, 5);
            p.hline(kRadius, kRadius, 4);
            break;
        case SyncIndicator::kLastFailed: // white cross on a solid disc
            p.circle(kRadius, kRadius, kRadius, true);
            p.line(4, 4, 10, 10, kPaper);
            p.line(10, 4, 4, 10, kPaper);
            p.line(5, 4, 10, 9, kPaper);
            p.line(9, 4, 4, 9, kPaper);
            break;
        case SyncIndicator::kNeverSynced: // question mark in a ring
            p.circle(kRadius, kRadius, kRadius, false);
            p.text(4, 11, "?", FontId::kSmall);
            break;
        case SyncIndicator::kNone:
            break;
    }
}

namespace {

// Cloud silhouette in a 20 x 11 box at (x, y) of the brush; `grow` widens it (white halo).
void draw_cloud(
    const Brush& p, std::int32_t x, std::int32_t y, std::int32_t grow, Color col) noexcept {
    p.circle(x + 5, y + 7, 4 + grow, true, col);
    p.circle(x + 10, y + 5, 5 + grow, true, col);
    p.circle(x + 15, y + 7, 4 + grow, true, col);
    p.fill(x + 5 - grow, y + 7 - grow, 11 + (2 * grow), 5 + (2 * grow), col);
}

void draw_sun(const Brush& p, std::int32_t cx, std::int32_t cy, std::int32_t r) noexcept {
    p.circle(cx, cy, r, true);
    const std::int32_t in = r + 2;
    const std::int32_t out = r + 5;
    const std::int32_t d0 = ((r * 3) / 4) + 2;
    const std::int32_t d1 = d0 + 2;
    p.line(cx, cy - in, cx, cy - out);
    p.line(cx, cy + in, cx, cy + out);
    p.line(cx - in, cy, cx - out, cy);
    p.line(cx + in, cy, cx + out, cy);
    p.line(cx + d0, cy + d0, cx + d1, cy + d1);
    p.line(cx - d0, cy + d0, cx - d1, cy + d1);
    p.line(cx + d0, cy - d0, cx + d1, cy - d1);
    p.line(cx - d0, cy - d0, cx - d1, cy - d1);
}

void draw_drops(const Brush& p, std::int32_t shift, std::int32_t len) noexcept {
    for (std::int32_t i = 0; i < 4; ++i) {
        const std::int32_t dx = 4 + (i * 4) + shift;
        const std::int32_t dy = 14 + ((i % 2) * 2);
        p.line(dx + 1, dy, dx - 1, dy + len);
    }
}

} // namespace

void draw_weather_icon(Canvas& c, std::int16_t x, std::int16_t y, WeatherCondition cond) noexcept {
    const Brush p(c, x, y);
    switch (cond) {
        case WeatherCondition::kClear:
            draw_sun(p, 10, 10, 4);
            break;
        case WeatherCondition::kPartlyCloudy:
            draw_sun(p, 7, 7, 3);
            draw_cloud(p, 0, 8, 1, kPaper);
            draw_cloud(p, 0, 8, 0, kInk);
            break;
        case WeatherCondition::kCloudy:
            draw_cloud(p, 0, 4, 0, kInk);
            draw_cloud(p, 1, 9, 0, kInk);
            break;
        case WeatherCondition::kFog:
            p.fill(2, 4, 16, 2);
            p.fill(5, 9, 13, 2);
            p.fill(2, 14, 16, 2);
            break;
        case WeatherCondition::kDrizzle:
            draw_cloud(p, 0, 1, 0, kInk);
            for (std::int32_t i = 0; i < 3; ++i) {
                p.fill(4 + (i * 5), 15 + ((i % 2) * 2), 2, 2);
            }
            break;
        case WeatherCondition::kRain:
            draw_cloud(p, 0, 1, 0, kInk);
            draw_drops(p, 0, 3);
            break;
        case WeatherCondition::kShowers:
            draw_cloud(p, 0, 1, 0, kInk);
            draw_drops(p, 0, 5);
            draw_drops(p, 1, 5);
            break;
        case WeatherCondition::kSnow:
            draw_cloud(p, 0, 1, 0, kInk);
            for (std::int32_t i = 0; i < 3; ++i) {
                const std::int32_t sx = 5 + (i * 5);
                const std::int32_t sy = 16 + ((i % 2) * 2);
                p.hline(sx - 1, sy, 3);
                p.vline(sx, sy - 1, 3);
            }
            break;
        case WeatherCondition::kThunder:
            draw_cloud(p, 0, 1, 0, kInk);
            p.line(11, 13, 7, 17);
            p.line(12, 13, 8, 17);
            p.line(7, 17, 12, 17);
            p.line(12, 17, 8, 19);
            break;
        case WeatherCondition::kUnknown:
            p.circle(10, 10, 8, false);
            p.text(7, 15, "?", FontId::kMedium);
            break;
    }
}

std::int16_t
draw_tag(Canvas& c, std::int16_t x, std::int16_t baseline_y, std::string_view text) noexcept {
    constexpr std::int16_t kPad = 2;
    const gfx::Font& f = gfx::font(FontId::kSmall);
    const auto w = static_cast<std::int16_t>(Canvas::text_width(text, f) + (2 * kPad));
    const auto top = static_cast<std::int16_t>(baseline_y - f.ascent - 1);
    c.fill_rect(Rect{x, top, w, static_cast<std::int16_t>(f.line_height)}, kInk);
    c.text(static_cast<std::int16_t>(x + kPad), baseline_y, text, f, kPaper);
    return w;
}

// ---- integer geometry -------------------------------------------------------------------------

namespace {

/// sin(d degrees) * 10000 for d = 0..90 (rounded; generated once, checked by faces_test).
constexpr std::array<std::int16_t, 91> kSinDeg{
    {0,    175,  349,  523,  698,  872,  1045, 1219, 1392, 1564, 1736, 1908, 2079, 2250, 2419, 2588,
     2756, 2924, 3090, 3256, 3420, 3584, 3746, 3907, 4067, 4226, 4384, 4540, 4695, 4848, 5000, 5150,
     5299, 5446, 5592, 5736, 5878, 6018, 6157, 6293, 6428, 6561, 6691, 6820, 6947, 7071, 7193, 7314,
     7431, 7547, 7660, 7771, 7880, 7986, 8090, 8192, 8290, 8387, 8480, 8572, 8660, 8746, 8829, 8910,
     8988, 9063, 9135, 9205, 9272, 9336, 9397, 9455, 9511, 9563, 9613, 9659, 9703, 9744, 9781, 9816,
     9848, 9877, 9903, 9925, 9945, 9962, 9976, 9986, 9994, 9998, 10000}};

constexpr std::int32_t kTenthsPerTurn = 3600;
constexpr std::int32_t kTenthsQuarter = 900;

/// sin for 0..900 tenths (first quadrant) with linear interpolation between whole degrees.
std::int32_t sin_quadrant(std::int32_t t) noexcept {
    const std::int32_t d = t / 10;
    const std::int32_t frac = t % 10;
    if (d >= 90) {
        return kSinDeg[90];
    }
    const auto a = static_cast<std::int32_t>(kSinDeg[static_cast<std::size_t>(d)]);
    const auto b = static_cast<std::int32_t>(kSinDeg[static_cast<std::size_t>(d + 1)]);
    return a + round_div((b - a) * frac, 10);
}

} // namespace

std::int32_t sin_e4(std::int32_t deg_tenths) noexcept {
    std::int32_t t = deg_tenths % kTenthsPerTurn;
    if (t < 0) {
        t += kTenthsPerTurn;
    }
    if (t <= kTenthsQuarter) {
        return sin_quadrant(t);
    }
    if (t <= 2 * kTenthsQuarter) {
        return sin_quadrant((2 * kTenthsQuarter) - t);
    }
    if (t <= 3 * kTenthsQuarter) {
        return -sin_quadrant(t - (2 * kTenthsQuarter));
    }
    return -sin_quadrant(kTenthsPerTurn - t);
}

std::int32_t cos_e4(std::int32_t deg_tenths) noexcept {
    return sin_e4(deg_tenths + kTenthsQuarter);
}

void thick_line(Canvas& c,
                std::int16_t x0,
                std::int16_t y0,
                std::int16_t x1,
                std::int16_t y1,
                std::int16_t width,
                Color color) noexcept {
    const auto r = static_cast<std::int16_t>(width / 2);
    if (r <= 0) {
        c.line(x0, y0, x1, y1, color);
        return;
    }
    // Bresenham walk; a disc at every step. Faces draw a handful of short lines per minute.
    std::int32_t x = x0;
    std::int32_t y = y0;
    const std::int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const std::int32_t dy = y1 > y0 ? -(y1 - y0) : -(y0 - y1);
    const std::int32_t sx = x0 < x1 ? 1 : -1;
    const std::int32_t sy = y0 < y1 ? 1 : -1;
    std::int32_t err = dx + dy;
    for (;;) {
        c.circle(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), r, true, color);
        if (x == x1 && y == y1) {
            break;
        }
        const std::int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }
}

TextBuf<8> format_steps(std::uint32_t steps) noexcept {
    constexpr std::uint32_t kMaxShown = 99999;
    TextBuf<8> out;
    out.put_uint(steps > kMaxShown ? kMaxShown : steps);
    if (steps > kMaxShown) {
        out.put_char('+');
    }
    return out;
}

std::int16_t draw_weather_compact(Canvas& c,
                                  std::int16_t x,
                                  std::int16_t y_top,
                                  const ui::WatchState& s,
                                  const gfx::Font& f) noexcept {
    if (!weather_visible(s)) {
        return 0;
    }
    constexpr std::int16_t kGap = 3;
    draw_weather_icon(c, x, y_top, s.weather.condition);
    const auto base =
        static_cast<std::int16_t>(y_top + ((layout::kWeatherIconSize + f.ascent) / 2));
    auto tx = static_cast<std::int16_t>(x + layout::kWeatherIconSize + kGap);
    const std::int16_t adv =
        c.text(tx, base, format_temp(s.weather.temp_dc, s.temp_unit).view(), f, Color::kBlack);
    tx = static_cast<std::int16_t>(tx + adv);
    if (s.weather_freshness == model::WeatherFreshness::kStale) {
        for (std::int16_t dx = 0; dx < adv; dx = static_cast<std::int16_t>(dx + 2)) {
            c.pixel(static_cast<std::int16_t>(tx - adv + dx),
                    static_cast<std::int16_t>(base + 2),
                    Color::kBlack);
        }
        tx = static_cast<std::int16_t>(tx + kGap);
        tx = static_cast<std::int16_t>(tx + c.text(tx,
                                                   base,
                                                   format_age(s.weather_age_s).view(),
                                                   gfx::font(FontId::kSmall),
                                                   Color::kBlack));
    }
    return static_cast<std::int16_t>(tx - x);
}

// ---- status row / sun -------------------------------------------------------------------------

std::int16_t
draw_status_row(Canvas& c, const ui::WatchState& s, std::int16_t y, std::int16_t inset) noexcept {
    constexpr std::int16_t kGap = 4;
    const gfx::Font& small = gfx::font(FontId::kSmall);
    const auto base = static_cast<std::int16_t>(y + 11);
    auto x = inset;
    const std::string_view tag = power_tag(s.power);
    if (!tag.empty()) {
        x = static_cast<std::int16_t>(x + draw_tag(c, x, base, tag) + kGap);
    }
    draw_battery_icon(c, x, static_cast<std::int16_t>(y + 1), s.battery);
    x = static_cast<std::int16_t>(x + layout::kBatteryIconW + kGap);
    x = static_cast<std::int16_t>(
        x + c.text(x, base, format_battery(s.battery).view(), small, Color::kBlack) + kGap);
    draw_sync_icon(
        c, static_cast<std::int16_t>(gfx::kWidth - inset - layout::kSyncIconSize), y, s.sync);
    return x;
}

namespace {

constexpr std::int32_t kSecondsPerDay = 86'400;
constexpr std::int32_t kMinutesPerDay = 1440;
constexpr std::int32_t kE5PerTenth = 10'000; ///< 1e-5 degrees per 0.1 degree

/// Largest t in [0, 1800] tenths with cos_e4(t) >= value (cos falls monotonically there).
std::int32_t acos_tenths(std::int32_t value_e4) noexcept {
    std::int32_t lo = 0;
    std::int32_t hi = 1800;
    while (lo < hi) {
        const std::int32_t mid = (lo + hi + 1) / 2;
        if (cos_e4(mid) >= value_e4) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

std::int32_t wrap_minutes(std::int32_t seconds) noexcept {
    std::int32_t s = seconds % kSecondsPerDay;
    if (s < 0) {
        s += kSecondsPerDay;
    }
    return (s + 30) / 60 % kMinutesPerDay;
}

} // namespace

SunTimes sun_times(const time::LocalDateTime& local, model::Location where) noexcept {
    const time::DayNumber jan1 = time::days_from_civil({local.date.year, 1, 1});
    const auto n = static_cast<std::int32_t>(time::days_from_civil(local.date) - jan1) + 1;
    // B = 360/365 * (N - 81) degrees, in tenths.
    const std::int32_t b = 3600 * (n - 81) / 365;
    const std::int32_t sin_b = sin_e4(b);
    const std::int32_t cos_b = cos_e4(b);
    const std::int32_t sin_2b = sin_e4(2 * b);
    // Equation of time (minutes) = 9.87 sin 2B - 7.53 cos B - 1.5 sin B; here in seconds.
    const std::int64_t eot_s =
        ((987LL * sin_2b) - (753LL * cos_b) - (150LL * sin_b)) * 60 / (100LL * kTrigOne);
    // Declination = 23.44 deg * sin B, in tenths.
    const auto decl = static_cast<std::int32_t>((2344LL * sin_b) / (10LL * kTrigOne));
    const std::int32_t lat = where.lat_e5 / kE5PerTenth;
    // cos w0 = (sin(-0.833 deg) - sin(lat) sin(decl)) / (cos(lat) cos(decl))
    const std::int64_t num = (static_cast<std::int64_t>(sin_e4(-8)) * kTrigOne) -
                             (static_cast<std::int64_t>(sin_e4(lat)) * sin_e4(decl));
    const std::int64_t den = static_cast<std::int64_t>(cos_e4(lat)) * cos_e4(decl);
    SunTimes out;
    if (den <= 0) {
        out.kind = SunTimes::Kind::kPolarNight; // a pole: never a normal day
        return out;
    }
    const std::int64_t cos_w0 = num * kTrigOne / den;
    if (cos_w0 >= kTrigOne) {
        out.kind = SunTimes::Kind::kPolarNight;
        return out;
    }
    if (cos_w0 <= -kTrigOne) {
        out.kind = SunTimes::Kind::kPolarDay;
        return out;
    }
    const std::int32_t w0 = acos_tenths(static_cast<std::int32_t>(cos_w0)); // tenths of a degree
    // Solar noon (UTC s) = 12 h - longitude * 240 s/deg - EoT; one degree of hour angle = 240 s.
    const std::int64_t noon_utc =
        43'200 - (static_cast<std::int64_t>(where.lon_e5) * 240 / 100'000) - eot_s;
    const std::int64_t half_day = static_cast<std::int64_t>(w0) * 24;
    out.rise_min =
        wrap_minutes(static_cast<std::int32_t>(noon_utc - half_day + local.utc_offset_s));
    out.set_min = wrap_minutes(static_cast<std::int32_t>(noon_utc + half_day + local.utc_offset_s));
    return out;
}

TextBuf<8> format_clock_minutes(std::int32_t minutes, model::HourFormat fmt) noexcept {
    const std::int32_t m = ((minutes % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay;
    std::int32_t h = m / 60;
    const std::int32_t mm = m % 60;
    TextBuf<8> out;
    const bool twelve = fmt == model::HourFormat::k12h;
    const bool pm = h >= 12;
    if (twelve) {
        h %= 12;
        h = h == 0 ? 12 : h;
    }
    out.put_uint(static_cast<std::uint64_t>(h));
    out.put_char(':');
    out.put_char(static_cast<char>('0' + (mm / 10)));
    out.put_char(static_cast<char>('0' + (mm % 10)));
    if (twelve) {
        out.put_char(pm ? 'p' : 'a');
    }
    return out;
}

} // namespace qz::faces
