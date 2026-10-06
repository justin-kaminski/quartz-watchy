// Face rendering tests (WP-16): every WatchState variant renders, readability rule, determinism,
// variant-specific visual differences, formatting helpers, and optional PNG export for review
// (set QZ_FACE_PNG_DIR to a directory to have the main variants written there).
#include "../src/face_common.hpp"
#include "qz/faces/registry.hpp"
#include "qz/gfx/framebuffer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace qz::faces {
namespace {

using gfx::Color;
using gfx::Framebuffer;
using model::PowerLevel;
using model::SyncIndicator;
using model::WeatherCondition;
using model::WeatherFreshness;

// gtest macros inflate the cognitive-complexity score of table-driven checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)

constexpr std::array<SyncIndicator, 5> kAllSync{SyncIndicator::kNone,
                                                SyncIndicator::kNeverSynced,
                                                SyncIndicator::kLastFailed,
                                                SyncIndicator::kStale,
                                                SyncIndicator::kOk};
constexpr std::array<WeatherCondition, 10> kAllConditions{WeatherCondition::kUnknown,
                                                          WeatherCondition::kClear,
                                                          WeatherCondition::kPartlyCloudy,
                                                          WeatherCondition::kCloudy,
                                                          WeatherCondition::kFog,
                                                          WeatherCondition::kDrizzle,
                                                          WeatherCondition::kRain,
                                                          WeatherCondition::kSnow,
                                                          WeatherCondition::kShowers,
                                                          WeatherCondition::kThunder};
constexpr std::array<PowerLevel, 4> kAllPower{
    PowerLevel::kNormal, PowerLevel::kLow, PowerLevel::kSaver, PowerLevel::kCritical};

ui::WatchState typical_state() {
    ui::WatchState s;
    s.time_valid = true;
    s.local.date = {2026, 10, 6};
    s.local.time = {14, 32, 0};
    s.local.weekday = time::Weekday::kTuesday;
    s.hour_format = model::HourFormat::k24h;
    s.steps.today = 6240;
    s.steps.goal = 10000;
    s.battery.valid = true;
    s.battery.percent = 85;
    s.battery.mv = 3950;
    s.weather.valid = 1;
    s.weather.has_high_low = 1;
    s.weather.temp_dc = 123;
    s.weather.high_dc = 151;
    s.weather.low_dc = 32;
    s.weather.condition = WeatherCondition::kPartlyCloudy;
    s.weather_freshness = WeatherFreshness::kFresh;
    s.weather_age_s = 1800;
    s.sync = SyncIndicator::kOk;
    s.conn_mode = model::ConnectivityMode::kTimeWeather;
    return s;
}

Framebuffer render(const FaceDescriptor& face, const ui::WatchState& s) {
    Framebuffer fb;
    gfx::Canvas canvas(fb);
    face.render(s, canvas);
    return fb;
}

struct Ink {
    int count = 0;
    int min_y = gfx::kHeight;
    int max_y = -1;
    [[nodiscard]] int height() const { return max_y < min_y ? 0 : max_y - min_y + 1; }
};

/// Ink statistics for rows [y0, y1).
Ink ink_in_rows(const Framebuffer& fb, int y0, int y1) {
    Ink ink;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < gfx::kWidth; ++x) {
            if (fb.get(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)) ==
                Color::kBlack) {
                ++ink.count;
                ink.min_y = std::min(ink.min_y, y);
                ink.max_y = std::max(ink.max_y, y);
            }
        }
    }
    return ink;
}

// ---- every variant renders ------------------------------------------------------------------

void render_all_faces(const ui::WatchState& s, const std::string& what) {
    for (const FaceDescriptor& d : descriptors()) {
        const Framebuffer fb = render(d, s);
        EXPECT_GT(ink_in_rows(fb, 0, gfx::kHeight).count, 100) << d.name << ": " << what;
    }
}

TEST(FacesTest, RendersTimeValidityAndHourFormats) {
    for (const bool valid : {true, false}) {
        for (const auto fmt : {model::HourFormat::k24h, model::HourFormat::k12h}) {
            for (const std::uint8_t hour : std::array<std::uint8_t, 7>{0, 1, 9, 11, 12, 13, 23}) {
                ui::WatchState s = typical_state();
                s.time_valid = valid;
                s.hour_format = fmt;
                s.local.time.hour = hour;
                render_all_faces(s, "time/hour");
            }
        }
    }
}

TEST(FacesTest, RendersEveryWeatherVariant) {
    for (const WeatherFreshness fresh :
         {WeatherFreshness::kFresh, WeatherFreshness::kStale, WeatherFreshness::kHidden}) {
        for (const WeatherCondition cond : kAllConditions) {
            for (const auto unit : {model::TempUnit::kCelsius, model::TempUnit::kFahrenheit}) {
                for (const bool hi_lo : {true, false}) {
                    ui::WatchState s = typical_state();
                    s.weather_freshness = fresh;
                    s.weather.condition = cond;
                    s.temp_unit = unit;
                    s.weather_high_low = hi_lo;
                    s.weather_age_s = fresh == WeatherFreshness::kStale ? 3U * 3600U : 60U;
                    render_all_faces(s, "weather");
                }
            }
        }
    }
    ui::WatchState none = typical_state();
    none.weather = {};
    render_all_faces(none, "no weather report");
}

TEST(FacesTest, RendersEverySyncIndicator) {
    for (const SyncIndicator sync : kAllSync) {
        ui::WatchState s = typical_state();
        s.sync = sync;
        render_all_faces(s, "sync");
    }
}

TEST(FacesTest, RendersPowerChargingAndBatteryVariants) {
    for (const PowerLevel level : kAllPower) {
        for (const bool usb : {false, true}) {
            for (const bool charging : {false, true}) {
                for (const bool valid : {false, true}) {
                    for (const std::uint8_t pct : std::array<std::uint8_t, 5>{0, 5, 50, 100, 200}) {
                        ui::WatchState s = typical_state();
                        s.power = level;
                        s.battery.level = level;
                        s.battery.usb_present = usb;
                        s.battery.charging = charging;
                        s.battery.valid = valid;
                        s.battery.percent = pct;
                        render_all_faces(s, "power");
                    }
                }
            }
        }
    }
}

TEST(FacesTest, RendersGoalVariantsAndExtremeValues) {
    constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t goal : {0U, 1U, 10000U, 50000U, kMax}) {
        for (const std::uint32_t steps : {0U, 1U, 9999U, 10000U, 123456U, kMax}) {
            ui::WatchState s = typical_state();
            s.steps.goal = goal;
            s.steps.today = steps;
            render_all_faces(s, "steps");
        }
    }
    ui::WatchState s = typical_state();
    s.weather.temp_dc = std::numeric_limits<std::int16_t>::min();
    s.weather.high_dc = std::numeric_limits<std::int16_t>::max();
    s.weather.low_dc = std::numeric_limits<std::int16_t>::min();
    s.weather_age_s = kMax;
    s.weather_freshness = WeatherFreshness::kStale;
    s.battery.percent = 255;
    s.local.date = {2199, 12, 31};
    s.local.weekday = time::Weekday::kSaturday;
    render_all_faces(s, "extreme weather/battery");
}

TEST(FacesTest, RendersLongStringsAndGarbageFields) {
    ui::WatchState s = typical_state();
    s.tz_label = "America/Argentina/ComodRivadavia-with-an-extremely-long-label";
    s.fw_version = "999.999.999-very-long-version-string";
    render_all_faces(s, "long strings");
    // Corrupted civil fields must degrade, not crash.
    s.local.time = {99, 99, 99};
    s.local.date = {1, 0, 0};
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): corrupted-input test
    s.local.weekday = static_cast<time::Weekday>(200);
    render_all_faces(s, "garbage time fields");
    s.local.date = {2026, 13, 255};
    render_all_faces(s, "garbage date fields");
}

TEST(FacesTest, DrawingOutsideTheNormalCanvasStateIsHarmless) {
    // Faces reset the clip and clear: a dirty framebuffer and a shrunk clip change nothing.
    const ui::WatchState s = typical_state();
    for (const FaceDescriptor& d : descriptors()) {
        const Framebuffer clean = render(d, s);
        Framebuffer dirty;
        dirty.clear(Color::kBlack);
        gfx::Canvas canvas(dirty);
        canvas.set_clip({10, 10, 5, 5});
        d.render(s, canvas);
        EXPECT_EQ(dirty.bits, clean.bits) << d.name;
    }
}

// ---- readability and determinism -----------------------------------------------------------

TEST(FacesTest, DefaultFaceTimeDigitsAreAtLeast48PixelsTall) {
    const Framebuffer fb = render(descriptors()[0], typical_state());
    // Rows between the status bar and the date line hold only the time.
    const Ink ink = ink_in_rows(fb, 18, 88);
    EXPECT_GE(ink.height(), layout::kMinTimeDigitPx);
    EXPECT_GT(ink.count, 500);
}

TEST(FacesTest, MinimalFaceTimeDigitsAreAtLeast48PixelsTall) {
    const Framebuffer fb = render(descriptors()[1], typical_state());
    const Ink ink = ink_in_rows(fb, 20, 160);
    EXPECT_GE(ink.height(), layout::kMinTimeDigitPx);
}

TEST(FacesTest, InvalidTimeDrawsAVisiblePlaceholder) {
    ui::WatchState s = typical_state();
    s.time_valid = false;
    // "--:--" uses dashes (short by design); it must still be large and unmistakable.
    EXPECT_GT(ink_in_rows(render(descriptors()[0], s), 18, 88).count, 300);
    EXPECT_GT(ink_in_rows(render(descriptors()[1], s), 20, 160).count, 300);
}

TEST(FacesTest, RenderingTwiceGivesIdenticalBytes) {
    ui::WatchState s = typical_state();
    s.weather_freshness = WeatherFreshness::kStale;
    s.power = PowerLevel::kSaver;
    s.battery.usb_present = true;
    s.battery.charging = true;
    for (const FaceDescriptor& d : descriptors()) {
        const Framebuffer a = render(d, s);
        const Framebuffer b = render(d, s);
        EXPECT_EQ(a.bits, b.bits) << d.name;
        EXPECT_EQ(a.crc32(), b.crc32()) << d.name;
    }
}

TEST(FacesTest, EveryNonDefaultSyncIndicatorLooksDistinct) {
    for (const FaceDescriptor& d : descriptors()) {
        std::vector<std::uint32_t> crcs;
        for (const SyncIndicator sync : kAllSync) {
            ui::WatchState s = typical_state();
            s.sync = sync;
            crcs.push_back(render(d, s).crc32());
        }
        for (std::size_t i = 0; i < crcs.size(); ++i) {
            for (std::size_t j = i + 1; j < crcs.size(); ++j) {
                EXPECT_NE(crcs[i], crcs[j]) << d.name << " sync " << i << " vs " << j;
            }
        }
    }
}

// ---- variant semantics ---------------------------------------------------------------------

TEST(FacesTest, WeatherFreshStaleHiddenAreVisiblyDifferent) {
    const FaceDescriptor& face = descriptors()[0];
    ui::WatchState s = typical_state();
    s.weather_freshness = WeatherFreshness::kFresh;
    const Framebuffer fresh = render(face, s);
    s.weather_freshness = WeatherFreshness::kStale;
    s.weather_age_s = 4U * 3600U;
    const Framebuffer stale = render(face, s);
    s.weather_freshness = WeatherFreshness::kHidden;
    const Framebuffer hidden = render(face, s);
    EXPECT_NE(fresh.bits, stale.bits) << "stale must carry a mark";
    EXPECT_NE(fresh.bits, hidden.bits);
    EXPECT_NE(stale.bits, hidden.bits);
    // Hidden weather leaves the weather row empty.
    EXPECT_EQ(ink_in_rows(hidden, 174, gfx::kHeight).count, 0);
    EXPECT_GT(ink_in_rows(fresh, 174, gfx::kHeight).count, 0);
}

TEST(FacesTest, HiddenWeatherEqualsNoReport) {
    ui::WatchState hidden = typical_state();
    hidden.weather_freshness = WeatherFreshness::kHidden;
    ui::WatchState none = typical_state();
    none.weather = {};
    none.weather_freshness = WeatherFreshness::kFresh; // no report: nothing to show either way
    for (const FaceDescriptor& d : descriptors()) {
        EXPECT_EQ(render(d, hidden).bits, render(d, none).bits) << d.name;
    }
}

TEST(FacesTest, EveryConditionGlyphIsDistinct) {
    std::vector<std::uint32_t> crcs;
    for (const WeatherCondition cond : kAllConditions) {
        ui::WatchState s = typical_state();
        s.weather.condition = cond;
        crcs.push_back(render(descriptors()[0], s).crc32());
    }
    for (std::size_t i = 0; i < crcs.size(); ++i) {
        for (std::size_t j = i + 1; j < crcs.size(); ++j) {
            EXPECT_NE(crcs[i], crcs[j]) << "condition " << i << " vs " << j;
        }
    }
}

TEST(FacesTest, InvalidTimeShowsPlaceholderNotTheOldTime) {
    for (const FaceDescriptor& d : descriptors()) {
        const ui::WatchState valid = typical_state();
        ui::WatchState invalid = typical_state();
        invalid.time_valid = false;
        EXPECT_NE(render(d, valid).bits, render(d, invalid).bits) << d.name;
        // The stale civil time inside the state must not leak when time_valid is false.
        ui::WatchState other = invalid;
        other.local.time = {3, 7, 0};
        other.local.date = {2030, 1, 1};
        EXPECT_EQ(render(d, invalid).bits, render(d, other).bits) << d.name;
    }
}

TEST(FacesTest, HourFormatAndMeridiemChangeTheImage) {
    for (const FaceDescriptor& d : descriptors()) {
        ui::WatchState s = typical_state();
        s.local.time.hour = 15;
        const Framebuffer h24 = render(d, s);
        s.hour_format = model::HourFormat::k12h;
        const Framebuffer pm = render(d, s);
        s.local.time.hour = 3;
        const Framebuffer am = render(d, s);
        EXPECT_NE(h24.bits, pm.bits) << d.name;
        EXPECT_NE(am.bits, pm.bits) << d.name;
    }
}

TEST(FacesTest, PowerStatesAreMarked) {
    for (const FaceDescriptor& d : descriptors()) {
        ui::WatchState s = typical_state();
        const Framebuffer normal = render(d, s);
        for (const PowerLevel level :
             {PowerLevel::kLow, PowerLevel::kSaver, PowerLevel::kCritical}) {
            s.power = level;
            EXPECT_NE(render(d, s).bits, normal.bits)
                << d.name << " level " << int{static_cast<std::uint8_t>(level)};
        }
        s.power = PowerLevel::kNormal;
        s.battery.usb_present = true;
        const Framebuffer usb = render(d, s);
        s.battery.charging = true;
        const Framebuffer chg = render(d, s);
        EXPECT_NE(usb.bits, normal.bits) << d.name;
        EXPECT_NE(chg.bits, usb.bits) << d.name;
    }
}

TEST(FacesTest, StepsGoalBarFillsProportionally) {
    const FaceDescriptor& face = descriptors()[0];
    ui::WatchState s = typical_state();
    s.steps.goal = 10000;
    int previous = -1;
    for (const std::uint32_t steps : {0U, 2500U, 5000U, 10000U, 40000U}) {
        s.steps.today = steps;
        const Framebuffer fb = render(face, s);
        int fill = 0;
        for (int x = 0; x < gfx::kWidth; ++x) {
            // Middle row of the bar's inner area: ink there is fill only.
            if (fb.get(static_cast<std::int16_t>(x), 169) == Color::kBlack) {
                ++fill;
            }
        }
        EXPECT_GE(fill, previous) << steps;
        previous = fill;
    }
    EXPECT_GT(previous, 150) << "goal reached: bar is full";
    s.steps.today = 100;
    s.steps.goal = 0; // no goal: no bar
    EXPECT_EQ(ink_in_rows(render(face, s), 165, 174).count, 0);
}

// ---- formatting helpers --------------------------------------------------------------------

TEST(FacesFormatTest, RoundDivRoundsHalfUpAndNeverNegativeZero) {
    EXPECT_EQ(round_div(14, 10), 1);
    EXPECT_EQ(round_div(15, 10), 2);
    EXPECT_EQ(round_div(-14, 10), -1);
    EXPECT_EQ(round_div(-15, 10), -1);
    EXPECT_EQ(round_div(-16, 10), -2);
    EXPECT_EQ(round_div(-4, 10), 0);
    EXPECT_EQ(round_div(0, 10), 0);
}

TEST(FacesFormatTest, TemperaturesRoundAndConvert) {
    EXPECT_EQ(temp_whole(123, model::TempUnit::kCelsius), 12);
    EXPECT_EQ(temp_whole(-123, model::TempUnit::kCelsius), -12);
    EXPECT_EQ(temp_whole(0, model::TempUnit::kFahrenheit), 32);
    EXPECT_EQ(temp_whole(100, model::TempUnit::kFahrenheit), 50);
    EXPECT_EQ(temp_whole(-400, model::TempUnit::kFahrenheit), -40);
    EXPECT_EQ(temp_whole(370, model::TempUnit::kFahrenheit), 99); // 98.6 F
    EXPECT_EQ(format_temp(123, model::TempUnit::kCelsius).view(), "12°C");
    EXPECT_EQ(format_temp(-5, model::TempUnit::kFahrenheit).view(), "31\u00B0F");
    EXPECT_EQ(format_temp(-250, model::TempUnit::kCelsius).view(), "-25°C");

    model::WeatherReport wx;
    wx.high_dc = 151;
    wx.low_dc = -32;
    EXPECT_EQ(format_high_low(wx, model::TempUnit::kCelsius).view(), "H15 L-3");
}

TEST(FacesFormatTest, AgeDateTimeAndBatteryText) {
    EXPECT_EQ(format_age(0).view(), "<1h old");
    EXPECT_EQ(format_age(3599).view(), "<1h old");
    EXPECT_EQ(format_age((3U * 3600U) + 5U).view(), "3h old");
    EXPECT_EQ(format_age(std::numeric_limits<std::uint32_t>::max()).view(), "99h+ old");

    time::LocalDateTime local;
    local.date = {2026, 10, 6};
    local.weekday = time::Weekday::kTuesday;
    EXPECT_EQ(format_date(local).view(), "Tue 6 Oct");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): corrupted-input test
    local.weekday = static_cast<time::Weekday>(99);
    local.date.month = 0;
    EXPECT_EQ(format_date(local).view(), "? 6 ?");

    ui::WatchState s = typical_state();
    s.local.time = {0, 5, 0};
    s.hour_format = model::HourFormat::k12h;
    TimeText t = format_time(s);
    EXPECT_EQ(t.text.view(), "12:05");
    EXPECT_TRUE(t.show_meridiem);
    EXPECT_FALSE(t.pm);
    s.local.time = {13, 5, 0};
    t = format_time(s);
    EXPECT_EQ(t.text.view(), "1:05");
    EXPECT_TRUE(t.pm);
    s.hour_format = model::HourFormat::k24h;
    t = format_time(s);
    EXPECT_EQ(t.text.view(), "13:05");
    EXPECT_FALSE(t.show_meridiem);
    s.time_valid = false;
    EXPECT_EQ(format_time(s).text.view(), "--:--");
    s.time_valid = true;
    s.local.time = {40, 0, 0};
    EXPECT_EQ(format_time(s).text.view(), "--:--");

    model::BatteryStatus b;
    EXPECT_EQ(format_battery(b).view(), "--%");
    b.valid = true;
    b.percent = 85;
    EXPECT_EQ(format_battery(b).view(), "85%");
    b.percent = 250;
    EXPECT_EQ(format_battery(b).view(), "100%");
    b.usb_present = true;
    EXPECT_EQ(format_battery(b).view(), "USB");
    b.charging = true;
    EXPECT_EQ(format_battery(b).view(), "CHG");

    EXPECT_TRUE(power_tag(PowerLevel::kNormal).empty());
    EXPECT_FALSE(power_tag(PowerLevel::kSaver).empty());
    EXPECT_TRUE(sync_word(SyncIndicator::kNone).empty());
    EXPECT_FALSE(sync_word(SyncIndicator::kOk).empty());
}

TEST(FacesFormatTest, TextBufTruncatesAndFormatsExtremes) {
    TextBuf<4> buf;
    buf.put("abcdef");
    EXPECT_EQ(buf.view(), "abcd");
    TextBuf<24> n;
    n.put_int(std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(n.view(), "-9223372036854775808");
    TextBuf<24> z;
    z.put_uint(0);
    EXPECT_EQ(z.view(), "0");
}

TEST(FacesFormatTest, WeatherVisibility) {
    ui::WatchState s = typical_state();
    EXPECT_TRUE(weather_visible(s));
    s.weather_freshness = WeatherFreshness::kStale;
    EXPECT_TRUE(weather_visible(s));
    s.weather_freshness = WeatherFreshness::kHidden;
    EXPECT_FALSE(weather_visible(s));
    s.weather_freshness = WeatherFreshness::kFresh;
    s.weather.valid = 0;
    EXPECT_FALSE(weather_visible(s));
}

// ---- PNG export for human review -----------------------------------------------------------

class FileSink final : public gfx::ByteSink {
public:
    explicit FileSink(const std::string& path) : out_(path, std::ios::binary) {}
    Status write(std::span<const std::uint8_t> bytes) override {
        out_.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return out_.good() ? Status{} : Status{Error{Errc::kInternal, 0}};
    }
    [[nodiscard]] bool ok() const { return out_.good(); }

private:
    std::ofstream out_;
};

/// Renders `face` for `s` and writes `<dir>/<name>.png`. Returns false on any failure.
bool write_face_png(const std::string& dir,
                    const std::string& name,
                    const FaceDescriptor& face,
                    const ui::WatchState& s) {
    const Framebuffer fb = render(face, s);
    FileSink sink(dir + "/" + name + ".png");
    return sink.ok() && gfx::encode_png(fb, sink).has_value();
}

TEST(FacesPngTest, WritesReviewImagesWhenQzFacePngDirIsSet) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, getenv is safe here.
    const char* dir = std::getenv("QZ_FACE_PNG_DIR");
    if (dir == nullptr || *dir == '\0') {
        GTEST_SKIP() << "set QZ_FACE_PNG_DIR to write face PNGs";
    }
    struct Variant {
        const char* name;
        ui::WatchState state;
    };
    std::vector<Variant> variants;
    variants.push_back({"normal", typical_state()});
    {
        ui::WatchState s = typical_state();
        s.hour_format = model::HourFormat::k12h;
        s.local.time = {9, 41, 0};
        s.weather.condition = WeatherCondition::kRain;
        s.temp_unit = model::TempUnit::kFahrenheit;
        s.steps.today = 11234;
        variants.push_back({"12h_rain_goal_met", s});
    }
    {
        ui::WatchState s = typical_state();
        s.weather_freshness = WeatherFreshness::kStale;
        s.weather_age_s = 4U * 3600U;
        s.sync = SyncIndicator::kStale;
        s.battery.percent = 15;
        s.power = PowerLevel::kLow;
        variants.push_back({"weather_stale_low_battery", s});
    }
    {
        ui::WatchState s = typical_state();
        s.time_valid = false;
        s.sync = SyncIndicator::kNeverSynced;
        s.weather_freshness = WeatherFreshness::kHidden;
        s.steps.goal = 0;
        variants.push_back({"time_invalid_never_synced", s});
    }
    {
        ui::WatchState s = typical_state();
        s.battery.usb_present = true;
        s.battery.charging = true;
        s.power = PowerLevel::kSaver;
        s.sync = SyncIndicator::kLastFailed;
        s.weather.condition = WeatherCondition::kThunder;
        variants.push_back({"charging_saver_sync_failed", s});
    }
    for (const Variant& v : variants) {
        for (const FaceDescriptor& d : descriptors()) {
            const std::string name = std::string(d.name) + "_" + v.name;
            EXPECT_TRUE(write_face_png(dir, name, d, v.state)) << name;
        }
    }
    // One sheet per condition for icon review.
    for (const WeatherCondition cond : kAllConditions) {
        ui::WatchState s = typical_state();
        s.weather.condition = cond;
        const std::string name = "default_wx_" + std::to_string(static_cast<int>(cond));
        EXPECT_TRUE(write_face_png(dir, name, descriptors()[0], s)) << name;
    }
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::faces
