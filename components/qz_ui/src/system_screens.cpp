// System screens: steps history, weather detail, sync, provisioning, diagnostics, about, factory
// reset, charge-me and the status overlay (ARCHITECTURE.md section 15). Pure drawing; integer-only.
//
// Layout rules: content stays inside a 6 px side margin; every string passes through fit_text()
// (code-point-safe truncation with "...") so nothing is ever clipped mid-glyph at the panel edge.
#include "system_screens.hpp"

#include "qz/time/tz.hpp"

#include <algorithm>
#include <array>

namespace qz::ui {
namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::Font;
using gfx::FontId;
using model::SyncIndicator;
using model::WeatherCondition;

/// Layout table (200 x 200 panel). [TUNE] values are judged on the PNG review images.
namespace layout {
constexpr std::int16_t kW = gfx::kWidth;
constexpr std::int16_t kPad = 6; ///< side margin
constexpr std::int16_t kContentW = kW - (2 * kPad);
constexpr std::int16_t kRowsTop = tuning::kTitleBarH + 2;
constexpr std::int16_t kBodyTop = tuning::kTitleBarH + 4;
constexpr std::int16_t kLabelGap = 8;    ///< min gap between a row's label and value
constexpr std::int16_t kLabelMaxW = 104; ///< widest label in a key/value row
constexpr std::int16_t kMinValueW = 24;

// steps chart
constexpr std::int16_t kBarW = 20;
constexpr std::int16_t kBarPitch = 28; ///< 7 * 28 - 8 = 188
constexpr std::int16_t kAxisY = 162;   ///< bars end on the row above this line
constexpr std::int16_t kDayLabelGap = 3;
constexpr std::int16_t kChartGap = 6; ///< between the caption block and the tallest bar

// provisioning
constexpr std::size_t kPasswordLineChars = 22; ///< medium font: 22 * 8 = 176 px <= box inner width
constexpr std::int16_t kBoxH = 22;
constexpr std::int16_t kBoxLineH = 16;
constexpr std::int16_t kBoxPad = 6;

constexpr std::int16_t kBigIconScale = 3;    ///< 16 px sync glyph -> 48 px
constexpr std::int16_t kStatusIconScale = 2; ///< battery 26 x 12 -> 52 x 24
constexpr std::int16_t kChargeIconScale = 4; ///< battery 26 x 12 -> 104 x 48

constexpr std::string_view kProvisioningUrl = "http://192.168.4.1"; ///< SoftAP gateway [ASSUMED]
constexpr std::uint32_t kMaxAgeDays = 99;
constexpr std::uint32_t kPercentCap = 999;
constexpr std::size_t kDiagMaxWakeRows = 6;
} // namespace layout

constexpr std::int32_t kSecondsPerHour = 3600;
constexpr std::int32_t kDeciPerDegree = 10;
constexpr std::int64_t kYearLimitUtc = 7'258'118'400; ///< 2200-01-01: civil_from_days range end
constexpr std::string_view kDegreeSign = "\xC2\xB0";  ///< UTF-8 degree sign (Latin-1 in fonts)

std::int16_t i16(std::int32_t v) noexcept {
    return static_cast<std::int16_t>(v);
}

std::int32_t floor_div(std::int32_t a, std::int32_t b) noexcept {
    std::int32_t q = a / b;
    if ((a % b) < 0) {
        --q;
    }
    return q;
}

std::int32_t round_div(std::int32_t num, std::int32_t den) noexcept {
    return floor_div((2 * num) + den, 2 * den);
}

/// Magnitude of a signed value as unsigned (right for INT32_MIN too).
std::uint32_t magnitude(std::int32_t v) noexcept {
    const auto u = static_cast<std::uint32_t>(v);
    return v < 0 ? 0U - u : u;
}

template<std::size_t N>
void put_int(TextBuilder<N>& b, std::int32_t v) noexcept {
    if (v < 0) {
        b.put_char('-');
    }
    b.put_uint(magnitude(v));
}

std::string_view yes_no(bool v) noexcept {
    return v ? "yes" : "no";
}

std::string_view or_dash(std::string_view s) noexcept {
    return s.empty() ? std::string_view("-") : s;
}

/// Longest prefix of at most `n` bytes that ends on a code-point boundary.
std::string_view utf8_prefix(std::string_view s, std::size_t n) noexcept {
    if (s.size() <= n) {
        return s;
    }
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0U) == 0x80U) {
        --n; // s[n] would start in the middle of a sequence: cut before it
    }
    return s.substr(0, n);
}

// ---- text and shape drawing ----------------------------------------------------------------
std::int16_t baseline_at(std::int32_t top, const Font& f) noexcept {
    return i16(top + f.ascent);
}

/// Fitted text aligned inside [x, x + width) on `baseline`.
void text_in(Canvas& c,
             std::int32_t x,
             std::int32_t width,
             std::int32_t baseline,
             std::string_view s,
             const Font& f,
             Align align,
             Color color = Color::kBlack) noexcept {
    const auto t = fit_text(s, f, i16(width));
    c.text_aligned(i16(x), i16(baseline), i16(width), t.view(), f, align, color);
}

void center_text(Canvas& c,
                 std::int32_t baseline,
                 std::string_view s,
                 const Font& f,
                 Color color = Color::kBlack) noexcept {
    text_in(c, layout::kPad, layout::kContentW, baseline, s, f, Align::kCenter, color);
}

/// "label ........ value" in row `row` below the title bar (medium font, one row = kRowH).
void kv_row(Canvas& c, std::int32_t row, std::string_view label, std::string_view value) noexcept {
    const Font& f = gfx::font(FontId::kMedium);
    const std::int32_t top = layout::kRowsTop + (row * tuning::kRowH);
    const std::int32_t base = top + ((tuning::kRowH - f.line_height) / 2) + f.ascent;
    const auto lab = fit_text(label, f, layout::kLabelMaxW);
    c.text(layout::kPad, i16(base), lab.view(), f, Color::kBlack);
    const std::int32_t lw = Canvas::text_width(lab.view(), f);
    const std::int32_t width =
        std::max<std::int32_t>(layout::kContentW - lw - layout::kLabelGap, layout::kMinValueW);
    text_in(c, layout::kPad + layout::kContentW - width, width, base, value, f, Align::kRight);
}

void draw_icon_centered(Canvas& c,
                        std::int32_t center_x,
                        std::int32_t top,
                        const gfx::Bitmap& bmp,
                        std::int16_t scale) noexcept {
    icons::draw(c, i16(center_x - ((bmp.width * scale) / 2)), i16(top), bmp, scale, Color::kBlack);
}

/// White-on-black bar with centered text (call-to-action lines).
void draw_banner(Canvas& c, std::int32_t top, std::int32_t h, std::string_view text) noexcept {
    const Font& f = gfx::font(FontId::kMedium);
    c.fill_rect({layout::kPad, i16(top), layout::kContentW, i16(h)}, Color::kBlack);
    const std::int32_t base = top + ((h - f.line_height) / 2) + f.ascent;
    text_in(
        c, layout::kPad + 4, layout::kContentW - 8, base, text, f, Align::kCenter, Color::kWhite);
}

/// Outline with a checkerboard fill: a light grey that stays legible on 1-bpp e-paper.
void draw_hatched_bar(
    Canvas& c, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) noexcept {
    c.rect({i16(x), i16(y), i16(w), i16(h)}, Color::kBlack);
    for (std::int32_t j = 1; j < h - 1; ++j) {
        for (std::int32_t i = 1; i < w - 1; ++i) {
            if (((i + j) % 2) == 0) {
                c.pixel(i16(x + i), i16(y + j), Color::kBlack);
            }
        }
    }
}

void draw_dashed_hline(Canvas& c, std::int32_t x, std::int32_t y, std::int32_t w) noexcept {
    constexpr std::int32_t kDash = 3;
    for (std::int32_t i = 0; i < w; i += 2 * kDash) {
        c.hline(i16(x + i), i16(y), i16(std::min(kDash, w - i)), Color::kBlack);
    }
}

bool weather_shown(const WatchState& s) noexcept {
    return s.weather.valid != 0 && s.weather_freshness != model::WeatherFreshness::kHidden;
}

} // namespace

// ---- formatting ----------------------------------------------------------------------------
namespace fmt {
namespace {
/// Whole degrees in the display unit from deci-degrees Celsius.
std::int32_t whole_degrees(std::int16_t temp_dc, model::TempUnit unit) noexcept {
    const auto dc = static_cast<std::int32_t>(temp_dc);
    if (unit == model::TempUnit::kFahrenheit) {
        // F = C * 9/5 + 32; in deci-degrees: dF = dC * 9/5 + 320; whole = round(dF / 10).
        return round_div((dc * 9) + (320 * 5), kDeciPerDegree * 5);
    }
    return round_div(dc, kDeciPerDegree);
}
} // namespace

TextBuilder<16> temperature(std::int16_t temp_dc, model::TempUnit unit) noexcept {
    TextBuilder<16> t;
    put_int(t, whole_degrees(temp_dc, unit));
    t.put(kDegreeSign);
    t.put_char(unit == model::TempUnit::kFahrenheit ? 'F' : 'C');
    return t;
}

TextBuilder<24> high_low(const model::WeatherReport& wx, model::TempUnit unit) noexcept {
    TextBuilder<24> t;
    t.put("H");
    put_int(t, whole_degrees(wx.high_dc, unit));
    t.put(" L");
    put_int(t, whole_degrees(wx.low_dc, unit));
    return t;
}

TextBuilder<20> age(std::uint32_t age_s) noexcept {
    constexpr std::uint32_t kMinute = 60;
    constexpr std::uint32_t kHour = 3600;
    constexpr std::uint32_t kDay = 86'400;
    constexpr std::uint32_t kDaysFromHours = 48;
    TextBuilder<20> t;
    if (age_s < kMinute) {
        t.put("just now");
    } else if (age_s < kHour) {
        t.put_uint(age_s / kMinute).put(" min ago");
    } else if (age_s < kDaysFromHours * kHour) {
        t.put_uint(age_s / kHour).put(" h ago");
    } else {
        const std::uint32_t days = age_s / kDay;
        if (days > layout::kMaxAgeDays) {
            t.put_uint(layout::kMaxAgeDays).put("+ d ago");
        } else {
            t.put_uint(days).put(" d ago");
        }
    }
    return t;
}

TextBuilder<16> count(std::uint32_t value) noexcept {
    constexpr std::size_t kGroup = 3;
    std::array<char, 10> digits{};
    std::size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0 && n < digits.size());
    TextBuilder<16> t;
    while (n > 0) {
        t.put_char(digits[--n]);
        if (n > 0 && (n % kGroup) == 0) {
            t.put_char(',');
        }
    }
    return t;
}

TextBuilder<12> clock(const time::CivilTime& time, time::HourFormat fmt) noexcept {
    TextBuilder<12> t;
    if (time.hour > 23 || time.minute > 59) {
        t.put("--:--");
        return t;
    }
    std::array<char, 8> raw{};
    bool pm = false;
    const std::size_t n = time::format_hhmm(raw, time, fmt, &pm);
    t.put(std::string_view(raw.data(), std::min(n, raw.size())));
    if (fmt == time::HourFormat::k12h) {
        t.put(pm ? " PM" : " AM");
    }
    return t;
}

TextBuilder<24> last_sync(const WatchState& state) noexcept {
    TextBuilder<24> t;
    if (state.last_sync_utc <= 0) {
        t.put("never");
        return t;
    }
    const std::int64_t local = state.last_sync_utc + state.local.utc_offset_s;
    if (!state.time_valid || local < 0 || local >= kYearLimitUtc) {
        t.put("?");
        return t;
    }
    const time::DayNumber day = time::day_of(local);
    const auto secs =
        static_cast<std::int32_t>(local - (static_cast<std::int64_t>(day) * time::kSecondsPerDay));
    time::CivilTime tod;
    tod.hour = static_cast<std::uint8_t>(secs / kSecondsPerHour);
    tod.minute = static_cast<std::uint8_t>((secs % kSecondsPerHour) / 60);
    tod.second = static_cast<std::uint8_t>(secs % 60);
    const time::CivilDate date = time::civil_from_days(day);
    const std::string_view month = time::month_name(date.month, true);
    t.put_uint(date.day).put_char(' ').put(month.empty() ? std::string_view("?") : month);
    t.put_char(' ').put(clock(tod, state.hour_format).view());
    return t;
}

TextBuilder<12> minutes_seconds(std::uint32_t seconds) noexcept {
    TextBuilder<12> t;
    t.put_uint(seconds / 60U).put_char(':').put_uint(seconds % 60U, 2);
    return t;
}

TextBuilder<16> duration_ms(std::uint32_t ms) noexcept {
    constexpr std::uint32_t kMsPerMin = 60'000;
    constexpr std::uint32_t kMsPerS = 1000;
    constexpr std::uint32_t kTenth = 10;
    TextBuilder<16> t;
    if (ms >= kMsPerMin) {
        t.put_uint(ms / kMsPerMin).put_char('.').put_uint((ms / (kMsPerMin / kTenth)) % kTenth);
        t.put(" min");
    } else {
        t.put_uint(ms / kMsPerS).put_char('.').put_uint((ms / (kMsPerS / kTenth)) % kTenth);
        t.put(" s");
    }
    return t;
}

TextBuilder<16> drift(std::int32_t ppb) noexcept {
    constexpr std::uint32_t kPpbPerPpm = 1000;
    constexpr std::uint32_t kPpbPerTenth = 100;
    TextBuilder<16> t;
    const std::uint32_t mag = magnitude(ppb);
    t.put_char(ppb < 0 ? '-' : '+');
    t.put_uint(mag / kPpbPerPpm).put_char('.').put_uint((mag % kPpbPerPpm) / kPpbPerTenth);
    t.put(" ppm");
    return t;
}

TextBuilder<12> utc_offset(std::int32_t offset_s) noexcept {
    constexpr std::uint32_t kHour = 3600;
    constexpr std::uint32_t kMinute = 60;
    TextBuilder<12> t;
    const std::uint32_t mag = magnitude(offset_s);
    t.put_char(offset_s < 0 ? '-' : '+');
    t.put_uint(mag / kHour, 2).put_char(':').put_uint((mag % kHour) / kMinute, 2);
    return t;
}

std::string_view condition_name(WeatherCondition cond) noexcept {
    switch (cond) {
        case WeatherCondition::kClear:
            return "Clear";
        case WeatherCondition::kPartlyCloudy:
            return "Partly cloudy";
        case WeatherCondition::kCloudy:
            return "Cloudy";
        case WeatherCondition::kFog:
            return "Fog";
        case WeatherCondition::kDrizzle:
            return "Drizzle";
        case WeatherCondition::kRain:
            return "Rain";
        case WeatherCondition::kSnow:
            return "Snow";
        case WeatherCondition::kShowers:
            return "Showers";
        case WeatherCondition::kThunder:
            return "Thunderstorm";
        case WeatherCondition::kUnknown:
            break;
    }
    return "Unknown";
}

std::string_view power_name(model::PowerLevel level) noexcept {
    switch (level) {
        case model::PowerLevel::kNormal:
            return "Normal";
        case model::PowerLevel::kLow:
            return "Low";
        case model::PowerLevel::kSaver:
            return "Saver";
        case model::PowerLevel::kCritical:
            return "Critical";
    }
    return "?";
}

std::string_view wake_cause_name(model::WakeCause cause) noexcept {
    switch (cause) {
        case model::WakeCause::kColdBoot:
            return "Cold boot";
        case model::WakeCause::kReset:
            return "Reset";
        case model::WakeCause::kTimer:
            return "Timer";
        case model::WakeCause::kButton:
            return "Button";
        case model::WakeCause::kAccel:
            return "Accel";
        case model::WakeCause::kUsb:
            return "USB";
        case model::WakeCause::kTetheredTick:
            return "Tether";
        case model::WakeCause::kUnknown:
            break;
    }
    return "Unknown";
}

std::string_view connectivity_name(model::ConnectivityMode mode) noexcept {
    switch (mode) {
        case model::ConnectivityMode::kOff:
            return "Off";
        case model::ConnectivityMode::kTimeOnly:
            return "Time only";
        case model::ConnectivityMode::kTimeWeather:
            return "Time+weather";
    }
    return "?";
}

std::string_view sync_word(SyncIndicator sync) noexcept {
    switch (sync) {
        case SyncIndicator::kNone:
            return "off";
        case SyncIndicator::kNeverSynced:
            return "never";
        case SyncIndicator::kLastFailed:
            return "failed";
        case SyncIndicator::kStale:
            return "stale";
        case SyncIndicator::kOk:
            return "ok";
    }
    return "?";
}

std::string_view error_text(Errc code) noexcept {
    switch (code) {
        case Errc::kTimeout:
            return "Timed out";
        case Errc::kNoCredentials:
            return "No Wi-Fi set up";
        case Errc::kBatteryLow:
            return "Battery too low";
        case Errc::kUnsupported:
            return "Radio unavailable";
        case Errc::kBusy:
            return "Busy, try again";
        case Errc::kIo:
            return "Radio error";
        case Errc::kNotFound:
            return "Network not found";
        case Errc::kNoTime:
            return "Clock not set";
        case Errc::kInvalidState:
            return "Not allowed now";
        case Errc::kCorrupt:
            return "Bad server reply";
        case Errc::kBadArgs:
        case Errc::kUnknownCommand:
        case Errc::kNoSpace:
        case Errc::kInternal:
            break;
    }
    return "Sync failed";
}

} // namespace fmt

// ---- screens -------------------------------------------------------------------------------
namespace {

// ---- StepsHistory ----
void draw_steps_chart(const WatchState& s, Canvas& c, std::int32_t chart_top) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const std::size_t days = std::min<std::size_t>(s.steps.history_count, model::kStepHistoryDays);
    std::uint32_t max_steps = std::max<std::uint32_t>(s.steps.goal, 1U);
    for (std::size_t i = 0; i < days; ++i) {
        max_steps = std::max(max_steps, s.steps.history[i].steps);
    }
    const std::int32_t plot_h = layout::kAxisY - chart_top;
    const auto bar_height = [&](std::uint32_t steps) {
        const auto h = static_cast<std::int32_t>(
            (static_cast<std::uint64_t>(steps) * static_cast<std::uint64_t>(plot_h)) / max_steps);
        return steps > 0 ? std::max<std::int32_t>(h, 1) : 0;
    };
    constexpr std::size_t kSlots = model::kStepHistoryDays;
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const std::size_t idx = kSlots - 1 - slot; // oldest on the left, newest on the right
        const std::int32_t x = layout::kPad + (static_cast<std::int32_t>(slot) * layout::kBarPitch);
        std::string_view label = "-";
        if (idx < days) {
            const model::StepDay& d = s.steps.history[idx];
            const std::int32_t h = bar_height(d.steps);
            const bool met = s.steps.goal == 0 || d.steps >= s.steps.goal;
            if (h > 0 && (met || h < 4)) {
                c.fill_rect({i16(x), i16(layout::kAxisY - h), layout::kBarW, i16(h)},
                            Color::kBlack);
            } else if (h > 0) {
                draw_hatched_bar(c, x, layout::kAxisY - h, layout::kBarW, h);
            }
            label = time::weekday_name(time::weekday_from_days(d.day), true);
        }
        const std::int32_t label_top = layout::kAxisY + layout::kDayLabelGap;
        const bool newest = idx == 0 && days > 0;
        if (newest) {
            c.fill_rect({i16(x - 1), i16(label_top), i16(layout::kBarW + 2), small.line_height},
                        Color::kBlack);
        }
        text_in(c,
                x - 1,
                layout::kBarW + 2,
                baseline_at(label_top, small),
                label,
                small,
                Align::kCenter,
                newest ? Color::kWhite : Color::kBlack);
    }
    c.hline(layout::kPad, layout::kAxisY, layout::kContentW, Color::kBlack);
    if (s.steps.goal > 0) {
        const auto h = static_cast<std::int32_t>(
            (static_cast<std::uint64_t>(s.steps.goal) * static_cast<std::uint64_t>(plot_h)) /
            max_steps);
        draw_dashed_hline(c, layout::kPad, layout::kAxisY - h, layout::kContentW);
    }
}

void render_steps(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& large = gfx::font(FontId::kLarge);
    draw_title_bar(c, "Steps");

    std::int32_t y = layout::kBodyTop;
    center_text(c, baseline_at(y, large), fmt::count(s.steps.today).view(), large);
    y += large.line_height + 2;

    TextBuilder<40> goal_line;
    if (s.steps.goal > 0) {
        const std::uint64_t pct = (static_cast<std::uint64_t>(s.steps.today) * 100U) / s.steps.goal;
        goal_line.put("goal ").put(fmt::count(s.steps.goal).view()).put(" (");
        goal_line.put_uint(
            static_cast<std::uint32_t>(std::min<std::uint64_t>(pct, layout::kPercentCap)));
        goal_line.put("%)");
    } else {
        goal_line.put("no step goal");
    }
    center_text(c, baseline_at(y, small), goal_line.view(), small);
    y += small.line_height + 1;

    const std::size_t days = std::min<std::size_t>(s.steps.history_count, model::kStepHistoryDays);
    if (days > 0) {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < days; ++i) {
            sum += s.steps.history[i].steps;
        }
        TextBuilder<40> line;
        line.put_uint(static_cast<std::uint32_t>(days)).put("-day avg ");
        line.put(fmt::count(static_cast<std::uint32_t>(sum / days)).view());
        center_text(c, baseline_at(y, small), line.view(), small);
    }
    y += small.line_height + 1;

    draw_steps_chart(s, c, y + layout::kChartGap);
    draw_hint(c, "BACK close");
}

// ---- WeatherDetail ----
void render_weather(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    const Font& big = gfx::font(FontId::kLarge); // kHuge only has digits and clock punctuation
    draw_title_bar(c, "Weather");

    if (!weather_shown(s)) {
        draw_icon_centered(c, gfx::kWidth / 2, 40, icons::weather(WeatherCondition::kUnknown), 2);
        const bool old_data = s.weather.valid != 0;
        center_text(
            c, baseline_at(104, medium), old_data ? "Data too old" : "No weather data", medium);
        std::string_view why = "Sync to fetch weather";
        if (!s.radio_available) {
            why = "This build has no radio";
        } else if (s.conn_mode != model::ConnectivityMode::kTimeWeather) {
            why = "Needs Time+weather mode";
        } else if (old_data) {
            why = "Sync to refresh";
        }
        center_text(c, baseline_at(126, small), why, small);
        draw_hint(c, "BACK close");
        return;
    }

    constexpr std::int32_t kIconX = 14;
    constexpr std::int32_t kIconTop = 32;
    constexpr std::int32_t kColumnX = 96;
    constexpr std::int32_t kColumnW = layout::kW - layout::kPad - kColumnX;
    icons::draw(c,
                i16(kIconX),
                i16(kIconTop),
                icons::weather(s.weather.condition),
                layout::kBigIconScale,
                Color::kBlack);
    text_in(c,
            kColumnX,
            kColumnW,
            baseline_at(kIconTop + 8, big),
            fmt::temperature(s.weather.temp_dc, s.temp_unit).view(),
            big,
            Align::kLeft);
    if (s.weather.has_high_low != 0) {
        text_in(c,
                kColumnX,
                kColumnW,
                baseline_at(kIconTop + big.line_height + 16, medium),
                fmt::high_low(s.weather, s.temp_unit).view(),
                medium,
                Align::kLeft);
    }
    center_text(c, baseline_at(116, medium), fmt::condition_name(s.weather.condition), medium);
    TextBuilder<32> age_line;
    age_line.put("Updated ").put(fmt::age(s.weather_age_s).view());
    center_text(c, baseline_at(138, small), age_line.view(), small);
    if (s.weather_freshness == model::WeatherFreshness::kStale) {
        draw_banner(c, 152, 22, "Old data");
    }
    draw_hint(c, "BACK close");
}

// ---- SyncNow ----
void render_sync(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    draw_title_bar(c, "Sync now");

    gfx::Bitmap icon = icons::sync_state(s.sync);
    std::string_view status = "Ready";
    std::string_view detail = "MENU starts a sync";
    switch (s.op_phase) {
        case OpPhase::kRunning:
            icon = icons::sync_running();
            status = "Syncing...";
            detail = "Please wait";
            break;
        case OpPhase::kSucceeded:
            icon = icons::sync_state(SyncIndicator::kOk);
            status = "Sync complete";
            detail = "Clock and weather updated";
            break;
        case OpPhase::kFailed:
            icon = icons::sync_state(SyncIndicator::kLastFailed);
            status = "Sync failed";
            detail = fmt::error_text(s.op_error);
            break;
        case OpPhase::kIdle:
            if (!s.radio_available) {
                status = "Radio unavailable";
                detail = "This build has no radio";
            } else if (!s.has_credentials) {
                status = "No Wi-Fi set up";
                detail = "Menu > Weather > Setup Wi-Fi";
            } else if (s.conn_mode == model::ConnectivityMode::kOff) {
                status = "Connectivity is off";
                detail = "Turn on in Menu > Connectivity";
            }
            break;
    }
    draw_icon_centered(c, gfx::kWidth / 2, 34, icon, layout::kBigIconScale);
    center_text(c, baseline_at(98, medium), status, medium);
    center_text(c, baseline_at(118, small), detail, small);

    center_text(c, baseline_at(140, small), "Last sync", small);
    center_text(c, baseline_at(154, medium), fmt::last_sync(s).view(), medium);
    draw_hint(c, s.op_phase == OpPhase::kRunning ? "BACK exit" : "MENU retry  BACK exit");
}

// ---- Provisioning ----
/// Outlined box `h` tall at `top` spanning the content width; returns the inner text top.
std::int32_t draw_box(Canvas& c, std::int32_t top, std::int32_t h) noexcept {
    c.rect({layout::kPad, i16(top), layout::kContentW, i16(h)}, Color::kBlack);
    return top + ((h - layout::kBoxLineH) / 2);
}

void render_provisioning(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    const Font& large = gfx::font(FontId::kLarge);
    draw_title_bar(c, "Wi-Fi setup");
    if (s.prov_ssid.empty()) {
        center_text(c, baseline_at(92, medium), "Starting...", medium);
        draw_hint(c, "BACK cancel");
        return;
    }
    constexpr std::int32_t kInner = layout::kContentW - 8;
    std::int32_t y = layout::kBodyTop;
    center_text(c, baseline_at(y, small), "1. Join this network", small);
    y += small.line_height + 1;
    std::int32_t inner_top = draw_box(c, y, layout::kBoxH);
    text_in(c,
            layout::kPad + 4,
            kInner,
            baseline_at(inner_top, medium),
            s.prov_ssid,
            medium,
            Align::kCenter);
    y += layout::kBoxH + 4;

    center_text(c, baseline_at(y, small), "2. Enter this password", small);
    y += small.line_height + 1;
    // The password is drawn, never logged. Long values wrap to a second line, then truncate.
    const std::string_view first = utf8_prefix(s.prov_password, layout::kPasswordLineChars);
    const std::string_view rest = s.prov_password.substr(first.size());
    const std::int32_t lines = rest.empty() ? 1 : 2;
    const std::int32_t box_h = layout::kBoxPad + (lines * layout::kBoxLineH);
    (void)draw_box(c, y, box_h);
    const std::int32_t text_top = y + (layout::kBoxPad / 2);
    text_in(
        c, layout::kPad + 4, kInner, baseline_at(text_top, medium), first, medium, Align::kCenter);
    if (lines == 2) {
        text_in(c,
                layout::kPad + 4,
                kInner,
                baseline_at(text_top + layout::kBoxLineH, medium),
                rest,
                medium,
                Align::kCenter);
    }
    y += box_h + 4;

    center_text(c, baseline_at(y, small), "3. Open in a browser", small);
    y += small.line_height + 1;
    center_text(c, baseline_at(y, medium), layout::kProvisioningUrl, medium);
    y += medium.line_height + 3;

    TextBuilder<24> left;
    left.put(fmt::minutes_seconds(s.prov_seconds_left).view()).put(" left");
    center_text(c, baseline_at(y, large), left.view(), large);
    draw_hint(c, "BACK stop");
}

// ---- Diagnostics ----
constexpr std::array<std::string_view, tuning::kDiagPageCount> kDiagNames{
    "Battery", "Time", "Sync", "Wakes", "Sensors", "Self-test"};

void diag_battery(const WatchState& s, Canvas& c) noexcept {
    const model::BatteryStatus& b = s.battery;
    TextBuilder<16> v;
    TextBuilder<16> p;
    if (b.valid) {
        v.put_uint(b.mv).put(" mV");
        p.put_uint(std::min<std::uint8_t>(b.percent, 100)).put("%");
    } else {
        v.put("no sample");
        p.put("--");
    }
    if (b.usb_present) {
        p.put(" (USB)");
    }
    kv_row(c, 0, "Voltage", v.view());
    kv_row(c, 1, "Percent", p.view());
    kv_row(c, 2, "Level", fmt::power_name(b.level));
    kv_row(c, 3, "Power mode", fmt::power_name(s.power));
    kv_row(c, 4, "USB", yes_no(b.usb_present));
    kv_row(c, 5, "Charging", yes_no(b.charging));
    kv_row(c, 6, "Tethered", yes_no(s.tethered));
    kv_row(c, 7, "Source", b.faked ? "faked" : "sensor");
}

void diag_time(const WatchState& s, Canvas& c) noexcept {
    const bool ok = s.time_valid && time::is_valid(s.local.date, s.local.time);
    TextBuilder<16> date;
    TextBuilder<16> tod;
    if (ok) {
        date.put_uint(static_cast<std::uint32_t>(s.local.date.year), 4).put_char('-');
        date.put_uint(s.local.date.month, 2).put_char('-').put_uint(s.local.date.day, 2);
        tod.put_uint(s.local.time.hour, 2).put_char(':').put_uint(s.local.time.minute, 2);
        tod.put_char(':').put_uint(s.local.time.second, 2);
    } else {
        date.put("--");
        tod.put("--");
    }
    kv_row(c, 0, "Valid", yes_no(s.time_valid));
    kv_row(c, 1, "Date", date.view());
    kv_row(c, 2, "Time", tod.view());
    kv_row(c, 3, "Zone", or_dash(s.tz_label));
    kv_row(c, 4, "UTC offset", fmt::utc_offset(s.local.utc_offset_s).view());
    kv_row(c, 5, "DST", yes_no(s.local.is_dst));
    kv_row(c, 6, "Drift", fmt::drift(s.drift_ppb).view());
    kv_row(c, 7, "Clock", s.clock_degraded ? "degraded" : "ok");
}

void diag_sync(const WatchState& s, Canvas& c) noexcept {
    std::string_view phase = "idle";
    switch (s.op_phase) {
        case OpPhase::kRunning:
            phase = "running";
            break;
        case OpPhase::kSucceeded:
            phase = "ok";
            break;
        case OpPhase::kFailed:
            phase = "failed";
            break;
        case OpPhase::kIdle:
            break;
    }
    kv_row(c, 0, "Mode", fmt::connectivity_name(s.conn_mode));
    kv_row(c, 1, "Radio", yes_no(s.radio_available));
    kv_row(c, 2, "Wi-Fi saved", yes_no(s.has_credentials));
    kv_row(c, 3, "Indicator", fmt::sync_word(s.sync));
    kv_row(c, 4, "Last sync", fmt::last_sync(s).view());
    kv_row(c, 5, "Operation", phase);
    kv_row(c, 6, "Error", s.op_phase == OpPhase::kFailed ? to_token(s.op_error) : "-");
    kv_row(c, 7, "Weather", s.weather.valid != 0 ? fmt::age(s.weather_age_s).view() : "none");
}

void diag_wakes(const WatchState& s, Canvas& c) noexcept {
    kv_row(c, 0, "Wakes today", fmt::count(s.wakes_today).view());
    kv_row(c, 1, "Awake today", fmt::duration_ms(s.awake_ms_today).view());
    if (s.recent_wakes.empty()) {
        kv_row(c, 2, "Recent", "none");
        return;
    }
    // recent_wakes is chronological (oldest first) [ASSUMED]; show the newest rows at the bottom.
    const std::size_t shown = std::min(s.recent_wakes.size(), layout::kDiagMaxWakeRows);
    const std::size_t first = s.recent_wakes.size() - shown;
    for (std::size_t i = 0; i < shown; ++i) {
        const model::WakeRecord& w = s.recent_wakes[first + i];
        TextBuilder<24> label;
        label.put(fmt::wake_cause_name(w.cause));
        if (w.error != 0) {
            label.put(" !");
        }
        TextBuilder<16> value;
        if (w.awake_ms < 1000U) { // compact: "123ms" / "5.0s", at most 11 characters with volts
            value.put_uint(w.awake_ms).put("ms ");
        } else {
            value.put_uint(w.awake_ms / 1000U).put_char('.').put_uint((w.awake_ms % 1000U) / 100U);
            value.put("s ");
        }
        value.put_uint(w.battery_mv / 1000U).put_char('.');
        value.put_uint((w.battery_mv % 1000U) / 10U, 2).put_char('V');
        kv_row(c, static_cast<std::int32_t>(2 + i), label.view(), value.view());
    }
}

void diag_sensors(const WatchState& s, Canvas& c) noexcept {
    kv_row(c, 0, "Steps today", fmt::count(s.steps.today).view());
    kv_row(c, 1, "Step goal", s.steps.goal > 0 ? fmt::count(s.steps.goal).view() : "off");
    TextBuilder<16> days;
    days.put_uint(std::min<std::uint8_t>(s.steps.history_count, model::kStepHistoryDays));
    kv_row(c, 2, "History days", days.view());
    TextBuilder<16> adc;
    if (s.battery.valid) {
        adc.put_uint(s.battery.mv).put(" mV");
    } else {
        adc.put("no sample");
    }
    kv_row(c, 3, "Battery ADC", adc.view());
    kv_row(c, 4, "Weather data", yes_no(s.weather.valid != 0));
    kv_row(c, 5, "Weather shown", yes_no(weather_shown(s)));
    kv_row(c, 6, "Faked values", yes_no(s.battery.faked || s.weather.faked != 0));
    kv_row(c, 7, "Power mode", fmt::power_name(s.power));
}

void diag_selftest(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    const Font& large = gfx::font(FontId::kLarge);
    center_text(c, baseline_at(48, small), "Last self-test", small);
    if (s.selftest_summary.empty()) {
        center_text(c, baseline_at(70, large), "not run", large);
    } else {
        center_text(c, baseline_at(70, large), s.selftest_summary, large);
    }
    center_text(c, baseline_at(112, medium), "Press MENU to run", medium);
    center_text(c, baseline_at(132, small), "Takes a few seconds", small);
}

void render_diagnostics(std::uint8_t page, const WatchState& s, Canvas& c) noexcept {
    const std::size_t pg = std::min<std::size_t>(page, tuning::kDiagPageCount - 1U);
    TextBuilder<32> title;
    title.put(kDiagNames[pg]).put("  ").put_uint(static_cast<std::uint32_t>(pg + 1)).put("/");
    title.put_uint(tuning::kDiagPageCount);
    draw_title_bar(c, title.view());
    switch (pg) {
        case 0:
            diag_battery(s, c);
            break;
        case 1:
            diag_time(s, c);
            break;
        case 2:
            diag_sync(s, c);
            break;
        case 3:
            diag_wakes(s, c);
            break;
        case 4:
            diag_sensors(s, c);
            break;
        default:
            diag_selftest(s, c);
            break;
    }
    draw_hint(c,
              pg + 1U == tuning::kDiagPageCount ? "MENU run test  BACK exit"
                                                : "UP/DOWN page  BACK exit");
}

// ---- About ----
void render_about(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& large = gfx::font(FontId::kLarge);
    draw_title_bar(c, "About");
    center_text(c, baseline_at(layout::kBodyTop, large), "Quartz", large);
    center_text(c,
                baseline_at(layout::kBodyTop + large.line_height + 2, small),
                "Watchy v3 firmware",
                small);
    kv_row(c, 3, "Version", or_dash(s.fw_version));
    kv_row(c, 4, "Git", or_dash(s.git_hash));
    kv_row(c, 5, "IDF", or_dash(s.idf_version));
    kv_row(c, 6, "Radio", s.radio_available ? "built in" : "not built");
    kv_row(c, 7, "tzdata", or_dash(time::tzdata_version()));
    draw_hint(c, "BACK close");
}

// ---- FactoryReset ----
void render_factory_reset(const WatchState& /*s*/, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    draw_title_bar(c, "Factory reset");
    draw_icon_centered(c, gfx::kWidth / 2, 30, icons::warning(), 2);
    center_text(c, baseline_at(88, medium), "Erase everything?", medium);
    center_text(c, baseline_at(110, small), "Settings, Wi-Fi and step", small);
    center_text(c, baseline_at(123, small), "history are lost for good.", small);
    draw_banner(c, 142, 24, "Hold MENU 3 s to erase");
    draw_hint(c, "BACK cancel");
}

// ---- ChargeMe ----
void render_charge_me(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    const Font& large = gfx::font(FontId::kLarge);
    draw_icon_centered(c,
                       gfx::kWidth / 2,
                       26,
                       icons::battery(icons::BatteryLevel::kEmpty),
                       layout::kChargeIconScale);
    center_text(c, baseline_at(92, large), "Charge me", large);
    center_text(c, baseline_at(130, small), "Battery is empty", small);
    if (s.time_valid) {
        TextBuilder<24> t;
        t.put("Last time ").put(fmt::clock(s.local.time, s.hour_format).view());
        center_text(c, baseline_at(150, medium), t.view(), medium);
    }
    if (s.battery.valid) {
        TextBuilder<16> mv;
        mv.put_uint(s.battery.mv).put(" mV");
        center_text(c, baseline_at(170, small), mv.view(), small);
    }
    draw_hint(c, "Plug in USB to charge");
}

// ---- StatusOverlay ----
void render_status(const WatchState& s, Canvas& c) noexcept {
    const Font& small = gfx::font(FontId::kSmall);
    const Font& medium = gfx::font(FontId::kMedium);
    const Font& large = gfx::font(FontId::kLarge);
    draw_title_bar(c, "Status");
    const model::BatteryStatus& b = s.battery;

    // battery
    constexpr std::int32_t kBatteryTop = 30;
    icons::draw(c,
                layout::kPad + 4,
                i16(kBatteryTop),
                icons::battery(icons::battery_level_of(b)),
                layout::kStatusIconScale,
                Color::kBlack);
    TextBuilder<16> pct;
    if (b.usb_present) {
        pct.put(b.charging ? "Charging" : "USB power");
    } else if (b.valid) {
        pct.put_uint(std::min<std::uint8_t>(b.percent, 100)).put("%");
    } else {
        pct.put("--%");
    }
    // "Charging" does not fit the large font next to the battery glyph: medium for words.
    const Font& pct_font = b.usb_present ? medium : large;
    text_in(c,
            70,
            104,
            baseline_at(kBatteryTop + ((24 - pct_font.line_height) / 2), pct_font),
            pct.view(),
            pct_font,
            Align::kLeft);
    if (s.power >= model::PowerLevel::kSaver) {
        icons::draw(c,
                    i16(layout::kW - layout::kPad - icons::kSaverSize),
                    i16(kBatteryTop + 4),
                    icons::saver(),
                    1,
                    Color::kBlack);
    }
    std::string_view note;
    if (s.power == model::PowerLevel::kCritical) {
        note = "Battery critical";
    } else if (s.power == model::PowerLevel::kSaver) {
        note = "Battery saver on";
    } else if (s.power == model::PowerLevel::kLow) {
        note = "Battery low";
    }
    if (!note.empty()) {
        center_text(c, baseline_at(60, small), note, small);
    }

    // steps
    TextBuilder<40> steps;
    steps.put(fmt::count(s.steps.today).view());
    if (s.steps.goal > 0) {
        steps.put(" / ").put(fmt::count(s.steps.goal).view());
    }
    steps.put(" steps");
    center_text(c, baseline_at(76, medium), steps.view(), medium);
    constexpr std::int32_t kBarTop = 98;
    constexpr std::int32_t kBarH = 10;
    c.rect({layout::kPad, i16(kBarTop), layout::kContentW, i16(kBarH)}, Color::kBlack);
    if (s.steps.goal > 0) {
        const std::uint64_t inner = layout::kContentW - 4;
        const auto fill = static_cast<std::int32_t>(std::min<std::uint64_t>(
            (static_cast<std::uint64_t>(s.steps.today) * inner) / s.steps.goal, inner));
        c.fill_rect({layout::kPad + 2, i16(kBarTop + 2), i16(fill), i16(kBarH - 4)}, Color::kBlack);
    }

    // sync
    constexpr std::int32_t kSyncTop = 120;
    icons::draw(c, layout::kPad, i16(kSyncTop), icons::sync_state(s.sync), 1, Color::kBlack);
    TextBuilder<40> sync;
    switch (s.sync) {
        case SyncIndicator::kOk:
            sync.put("Synced ").put(fmt::last_sync(s).view());
            break;
        case SyncIndicator::kStale:
            sync.put("Stale, last ").put(fmt::last_sync(s).view());
            break;
        case SyncIndicator::kLastFailed:
            sync.put("Last sync failed");
            break;
        case SyncIndicator::kNeverSynced:
            sync.put("Never synced");
            break;
        case SyncIndicator::kNone:
            sync.put("Sync off");
            break;
    }
    text_in(c,
            layout::kPad + icons::kSyncSize + 6,
            layout::kContentW - icons::kSyncSize - 6,
            baseline_at(kSyncTop, medium),
            sync.view(),
            medium,
            Align::kLeft);

    // weather
    if (weather_shown(s)) {
        constexpr std::int32_t kWeatherTop = 144;
        icons::draw(c,
                    layout::kPad,
                    i16(kWeatherTop),
                    icons::weather(s.weather.condition),
                    1,
                    Color::kBlack);
        TextBuilder<40> wx;
        wx.put(fmt::temperature(s.weather.temp_dc, s.temp_unit).view()).put("  ");
        wx.put(fmt::condition_name(s.weather.condition));
        text_in(c,
                layout::kPad + icons::kWeatherSize + 6,
                layout::kContentW - icons::kWeatherSize - 6,
                baseline_at(kWeatherTop + 4, medium),
                wx.view(),
                medium,
                Align::kLeft);
    }
    draw_hint(c, "BACK close");
}

} // namespace

void render_system_screen(ScreenId id,
                          std::uint8_t page,
                          const WatchState& state,
                          Canvas& canvas) noexcept {
    switch (id) {
        case ScreenId::kStepsHistory:
            render_steps(state, canvas);
            break;
        case ScreenId::kWeatherDetail:
            render_weather(state, canvas);
            break;
        case ScreenId::kSyncNow:
            render_sync(state, canvas);
            break;
        case ScreenId::kProvisioning:
            render_provisioning(state, canvas);
            break;
        case ScreenId::kDiagnostics:
            render_diagnostics(page, state, canvas);
            break;
        case ScreenId::kAbout:
            render_about(state, canvas);
            break;
        case ScreenId::kFactoryReset:
            render_factory_reset(state, canvas);
            break;
        case ScreenId::kChargeMe:
            render_charge_me(state, canvas);
            break;
        case ScreenId::kStatusOverlay:
            render_status(state, canvas);
            break;
        case ScreenId::kFace:
        case ScreenId::kMenu:
        case ScreenId::kTimeDateEditor:
        case ScreenId::kTimezonePicker:
        case ScreenId::kChoice:
        case ScreenId::kWeatherSettings:
        case ScreenId::kLocationEditor:
        case ScreenId::kStepGoalEditor:
        case ScreenId::kCount:
            break;
    }
}

} // namespace qz::ui
