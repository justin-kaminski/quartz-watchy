// WP-15: system screens, icon set and the text formatting they share. Covers the fuzz-ish
// state matrix (every screen x time/battery/weather/sync/string variants), the edge-margin rule
// (long strings are truncated, never clipped mid-glyph at the panel edge), determinism, icons,
// and optional PNG export for review (set QZ_SCREEN_PNG_DIR to a directory).
#include "../src/icons.hpp"
#include "../src/system_screens.hpp"
#include "qz/time/civil.hpp"
#include "ui_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace qz::ui {
namespace {

using gfx::Color;
using gfx::Framebuffer;
using model::SyncIndicator;
using model::WeatherCondition;

// NOLINTBEGIN(readability-function-cognitive-complexity)

// ---- fixtures ------------------------------------------------------------------------------
template<std::size_t N>
constexpr std::array<char, N> repeat(char c) {
    std::array<char, N> a{};
    for (char& x : a) {
        x = c;
    }
    return a;
}
constexpr auto kW32 = repeat<32>('W');
constexpr auto kW63 = repeat<63>('W');
constexpr auto kM40 = repeat<40>('M');
constexpr auto kX90 = repeat<90>('X');
constexpr std::string_view kUtf8 =
    "Z\xC3\xBCrich \xE2\x80\x93 \xE6\x9D\xB1\xE4\xBA\xAC \xF0\x9F\x95\x90 caf\xC3\xA9";
constexpr int kDiagPages = tuning::kDiagPageCount;
constexpr std::string_view kBrokenUtf8 = "ab\xE2\x82"; ///< sequence cut in the middle

std::string_view sv(const auto& arr) {
    return {arr.data(), arr.size()};
}

const std::array<model::WakeRecord, 9>& wake_log() {
    static const std::array<model::WakeRecord, 9> kLog = [] {
        std::array<model::WakeRecord, 9> a{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i].cause = static_cast<model::WakeCause>(i % model::kWakeCauseCount);
            a[i].awake_ms = static_cast<std::uint16_t>(40U + (i * 7000U));
            a[i].battery_mv = static_cast<std::uint16_t>(3300U + (i * 111U));
            a[i].error = static_cast<std::uint8_t>(i % 3U);
        }
        return a;
    }();
    return kLog;
}

const std::array<ScreenId, 9> kSystemScreens{ScreenId::kStepsHistory,
                                             ScreenId::kWeatherDetail,
                                             ScreenId::kSyncNow,
                                             ScreenId::kProvisioning,
                                             ScreenId::kDiagnostics,
                                             ScreenId::kAbout,
                                             ScreenId::kFactoryReset,
                                             ScreenId::kChargeMe,
                                             ScreenId::kStatusOverlay};

/// A believable state: valid time, mid battery, fresh weather, synced.
WatchState nominal_state(const settings::Settings& settings) {
    WatchState s;
    s.settings = &settings;
    s.time_valid = true;
    s.local.date = {2026, 10, 6};
    s.local.time = {14, 32, 42};
    s.local.weekday = time::Weekday::kTuesday;
    s.tz_label = "America/Chicago";
    s.local.utc_offset_s = -5 * 3600;
    s.local.is_dst = true;
    s.steps.today = 8421;
    s.steps.goal = 10000;
    const std::array<std::uint32_t, 7> hist{8421, 12034, 9950, 4210, 10320, 7777, 15002};
    for (std::size_t i = 0; i < hist.size(); ++i) {
        s.steps.history[i] = {20000 - static_cast<std::int32_t>(i), hist[i]};
    }
    s.steps.history_count = 7;
    s.battery = {3920, 70, model::PowerLevel::kNormal, false, false, true, false};
    s.weather.valid = 1;
    s.weather.has_high_low = 1;
    s.weather.temp_dc = 215;
    s.weather.high_dc = 260;
    s.weather.low_dc = 120;
    s.weather.condition = WeatherCondition::kPartlyCloudy;
    s.weather_freshness = model::WeatherFreshness::kFresh;
    s.weather_age_s = 3 * 3600;
    s.sync = SyncIndicator::kOk;
    s.conn_mode = model::ConnectivityMode::kTimeWeather;
    s.has_credentials = true;
    s.last_sync_utc = 1'791'297'120; // 2026-10-06 14:32 UTC range
    s.prov_ssid = "Quartz-4F2A";
    s.prov_password = "k7Qm2xW9pRtB";
    s.prov_seconds_left = 273;
    s.fw_version = "1.2.3";
    s.git_hash = "abc1234";
    s.idf_version = "v6.1";
    s.drift_ppb = 12'345;
    s.awake_ms_today = 38'400;
    s.wakes_today = 1440;
    s.recent_wakes = wake_log();
    s.selftest_summary = "12/12 pass";
    return s;
}

class ScreensTest : public ::testing::Test {
protected:
    settings::Settings settings_ = settings::defaults();
    test::FakeFaces faces_;
    Ui ui_{faces_};

    Framebuffer render(ScreenId id, const WatchState& s, int page = 0) {
        EXPECT_TRUE(ui_.show(id, s).has_value());
        for (int i = 0; i < page; ++i) {
            (void)ui_.handle(test::click(model::Button::kDown), s);
        }
        Framebuffer fb;
        gfx::Canvas canvas(fb);
        ui_.render(s, canvas);
        return fb;
    }
};

bool black(const Framebuffer& fb, int x, int y) {
    return fb.get(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)) == Color::kBlack;
}

/// Ink in the 3 px side margins (rows below the title bar) would mean a glyph or shape was cut
/// by the panel edge. (The bottom edge is covered by HintFitsAboveTheBottomEdge.)
std::string edge_violation(const Framebuffer& fb, int first_row) {
    constexpr std::array<int, 6> kColumns{0, 1, 2, 197, 198, 199};
    for (int y = first_row; y < gfx::kHeight; ++y) {
        for (const int x : kColumns) {
            if (black(fb, x, y)) {
                return "ink at x=" + std::to_string(x) + " y=" + std::to_string(y);
            }
        }
    }
    return {};
}

int first_content_row(ScreenId id) {
    return id == ScreenId::kChargeMe ? 0 : tuning::kTitleBarH;
}

int ink_below_title(const Framebuffer& fb) {
    int n = 0;
    for (int y = tuning::kTitleBarH; y < gfx::kHeight; ++y) {
        for (int x = 0; x < gfx::kWidth; ++x) {
            n += black(fb, x, y) ? 1 : 0;
        }
    }
    return n;
}

// ---- state matrix --------------------------------------------------------------------------
std::vector<model::BatteryStatus> battery_variants() {
    std::vector<model::BatteryStatus> v;
    v.push_back({}); // no sample
    for (const std::uint8_t pct :
         std::array<std::uint8_t, 8>{0, 5, 10, 30, 55, 80, 100, 255}) { // 255: out of range
        v.push_back({static_cast<std::uint16_t>(3300 + (pct * 3)),
                     pct,
                     model::PowerLevel::kNormal,
                     false,
                     false,
                     true,
                     false});
    }
    v.push_back({4200, 100, model::PowerLevel::kNormal, true, true, true, false}); // charging
    v.push_back({4200, 100, model::PowerLevel::kNormal, true, false, true, true}); // USB, full
    v.push_back({0xFFFF, 100, model::PowerLevel::kCritical, false, false, true, true});
    return v;
}

template<class Fn>
void for_each_state(const settings::Settings& settings, const Fn& fn) {
    const WatchState base = nominal_state(settings);
    const auto batteries = battery_variants();

    // time validity x hour format x battery x power level
    for (int tv = 0; tv < 3; ++tv) {
        for (const auto hf : {time::HourFormat::k24h, time::HourFormat::k12h}) {
            for (const auto& b : batteries) {
                for (const auto pw : {model::PowerLevel::kNormal,
                                      model::PowerLevel::kLow,
                                      model::PowerLevel::kSaver,
                                      model::PowerLevel::kCritical}) {
                    WatchState s = base;
                    s.time_valid = tv != 0;
                    if (tv == 2) {
                        s.local.date = {2026, 13, 0};
                        s.local.time = {99, 99, 99};
                        s.last_sync_utc = INT64_MAX;
                    }
                    if (tv == 0) {
                        s.local.date = {};
                        s.local.time = {};
                    }
                    s.hour_format = hf;
                    s.battery = b;
                    s.power = pw;
                    s.tethered = pw == model::PowerLevel::kLow;
                    fn(s);
                }
            }
        }
    }
    // weather: condition (incl. out of range) x freshness x unit x validity x age
    for (int cond = 0; cond < 12; ++cond) {
        for (const auto fresh : {model::WeatherFreshness::kFresh,
                                 model::WeatherFreshness::kStale,
                                 model::WeatherFreshness::kHidden}) {
            for (const auto unit : {model::TempUnit::kCelsius, model::TempUnit::kFahrenheit}) {
                for (const std::uint8_t valid : std::array<std::uint8_t, 2>{0, 1}) {
                    for (const std::uint32_t age : {0U, 7200U, 4'000'000'000U}) {
                        WatchState s = base;
                        s.weather.condition = cond == 11 ? static_cast<WeatherCondition>(200)
                                                         : static_cast<WeatherCondition>(cond);
                        s.weather_freshness = fresh;
                        s.temp_unit = unit;
                        s.weather.valid = valid;
                        s.weather.has_high_low = static_cast<std::uint8_t>(age == 0 ? 0 : 1);
                        s.weather.temp_dc = static_cast<std::int16_t>(cond % 2 == 0 ? -400 : 32767);
                        s.weather.high_dc = INT16_MAX;
                        s.weather.low_dc = INT16_MIN;
                        s.weather_age_s = age;
                        fn(s);
                    }
                }
            }
        }
    }
    // sync / connectivity / operation
    for (int sync = 0; sync < 7; ++sync) { // 5, 6: out of range
        for (const auto mode : {model::ConnectivityMode::kOff,
                                model::ConnectivityMode::kTimeOnly,
                                model::ConnectivityMode::kTimeWeather}) {
            for (const bool radio : {false, true}) {
                for (int op = 0; op < 4 + 13; ++op) {
                    WatchState s = base;
                    s.sync = static_cast<SyncIndicator>(sync);
                    s.conn_mode = mode;
                    s.radio_available = radio;
                    s.has_credentials = op % 2 == 0;
                    s.op_phase = static_cast<OpPhase>(op < 3 ? op : 3);
                    s.op_error = static_cast<Errc>(op < 3 ? 0 : op - 3);
                    s.last_sync_utc = op % 5 == 0 ? 0 : base.last_sync_utc;
                    fn(s);
                }
            }
        }
    }
    // steps: goal / history / extremes
    for (const std::uint32_t goal : {0U, 500U, 10000U, 4'000'000'000U}) {
        for (const std::uint32_t today : {0U, 1U, 10000U, 4'294'967'295U}) {
            for (const std::uint8_t count : std::array<std::uint8_t, 5>{0, 1, 3, 7, 9}) {
                WatchState s = base;
                s.steps.goal = goal;
                s.steps.today = today;
                s.steps.history_count = count;
                s.steps.history[0].steps = today;
                s.steps.history[3].steps = 4'294'967'295U;
                fn(s);
            }
        }
    }
    // strings: empty, long, multi-byte, broken UTF-8, wake-log extremes
    const std::array<std::string_view, 6> strings{
        std::string_view{}, sv(kW32), sv(kW63), sv(kM40), sv(kX90), kUtf8};
    for (const auto str : strings) {
        for (const auto pw : {strings[0], strings[2], strings[5], kBrokenUtf8}) {
            WatchState s = base;
            s.prov_ssid = str;
            s.prov_password = pw;
            s.tz_label = str;
            s.fw_version = str;
            s.git_hash = str;
            s.idf_version = str;
            s.selftest_summary = str;
            s.prov_seconds_left = 0xFFFF;
            s.drift_ppb = INT32_MIN;
            s.awake_ms_today = 0xFFFFFFFFU;
            s.wakes_today = 0xFFFF;
            s.local.utc_offset_s = INT32_MIN;
            s.recent_wakes = {};
            fn(s);
            s.recent_wakes = std::span<const model::WakeRecord>(wake_log()).first(1);
            s.prov_ssid = kBrokenUtf8;
            fn(s);
        }
    }
}

// ---- tests: screens ------------------------------------------------------------------------
TEST_F(ScreensTest, EveryScreenRendersEveryStateWithoutTouchingTheEdges) {
    std::size_t states = 0;
    for_each_state(settings_, [&](const WatchState& s) {
        ++states;
        for (std::size_t i = 0; i < static_cast<std::size_t>(ScreenId::kCount); ++i) {
            const auto id = static_cast<ScreenId>(i);
            const bool system = std::ranges::find(kSystemScreens, id) != kSystemScreens.end();
            // The WP-14 list screens depend little on the snapshot: sample them.
            if (!system && (states % 10) != 1) {
                continue;
            }
            if (id == ScreenId::kChoice) {
                for (std::size_t k = 0; k < static_cast<std::size_t>(ChoiceKind::kCount); ++k) {
                    ASSERT_TRUE(ui_.show_choice(static_cast<ChoiceKind>(k), s).has_value());
                    Framebuffer fb;
                    gfx::Canvas canvas(fb);
                    ui_.render(s, canvas);
                }
                continue;
            }
            const int pages = id == ScreenId::kDiagnostics ? tuning::kDiagPageCount : 1;
            for (int page = 0; page < pages; ++page) {
                const Framebuffer fb = render(id, s, page);
                if (system) {
                    const std::string why = edge_violation(fb, first_content_row(id));
                    ASSERT_TRUE(why.empty()) << screen_name(id) << " page " << page << ": " << why;
                }
            }
        }
    });
    EXPECT_GT(states, 1000U);
}

TEST(ScreensLayoutTest, HintFitsAboveTheBottomEdge) {
    // The hint line is the lowest text on every screen: its descenders must end on the panel.
    const gfx::Font& f = gfx::font(gfx::FontId::kSmall);
    EXPECT_LE(tuning::kHintBaseline + (f.line_height - f.ascent), gfx::kHeight);
}

TEST_F(ScreensTest, RenderingIsDeterministic) {
    for_each_state(settings_, [&](const WatchState& s) {
        static int n = 0;
        if (++n % 37 != 0) {
            return;
        }
        for (const ScreenId id : kSystemScreens) {
            const Framebuffer a = render(id, s);
            const Framebuffer b = render(id, s);
            ASSERT_EQ(a.crc32(), b.crc32()) << screen_name(id);
            ASSERT_EQ(a.bits, b.bits) << screen_name(id);
        }
    });
}

TEST_F(ScreensTest, EverySystemScreenDrawsContentAndPagesDiffer) {
    const WatchState s = nominal_state(settings_);
    std::set<std::uint32_t> crcs;
    for (const ScreenId id : kSystemScreens) {
        const Framebuffer fb = render(id, s);
        EXPECT_GT(ink_below_title(fb), 150) << screen_name(id);
        crcs.insert(fb.crc32());
    }
    EXPECT_EQ(crcs.size(), kSystemScreens.size());
    std::set<std::uint32_t> pages;
    for (int p = 0; p < kDiagPages; ++p) {
        pages.insert(render(ScreenId::kDiagnostics, s, p).crc32());
    }
    EXPECT_EQ(pages.size(), static_cast<std::size_t>(tuning::kDiagPageCount));
}

TEST_F(ScreensTest, DiagnosticsPageWrapsAndClamps) {
    const WatchState s = nominal_state(settings_);
    const auto first = render(ScreenId::kDiagnostics, s, 0).crc32();
    // Six DOWN clicks wrap around to page one.
    EXPECT_EQ(render(ScreenId::kDiagnostics, s, tuning::kDiagPageCount).crc32(), first);
    // An out-of-range page index (never produced by SystemScreen) draws the last page.
    Framebuffer fb;
    gfx::Canvas canvas(fb);
    render_system_screen(ScreenId::kDiagnostics, 200, s, canvas);
    EXPECT_EQ(fb.crc32(), render(ScreenId::kDiagnostics, s, tuning::kDiagPageCount - 1).crc32());
}

TEST_F(ScreensTest, NonSystemIdsDrawNothingThroughTheDispatcher) {
    const WatchState s = nominal_state(settings_);
    Framebuffer fb;
    gfx::Canvas canvas(fb);
    for (const ScreenId id : {ScreenId::kFace,
                              ScreenId::kMenu,
                              ScreenId::kTimezonePicker,
                              ScreenId::kWeatherSettings,
                              ScreenId::kCount}) {
        render_system_screen(id, 0, s, canvas);
    }
    EXPECT_EQ(test::black_pixels(fb), 0);
}

TEST_F(ScreensTest, ProvisioningShowsSsidPasswordAndCountdown) {
    const WatchState s = nominal_state(settings_);
    const auto base = render(ScreenId::kProvisioning, s).crc32();
    WatchState t = s;
    t.prov_password = "different-pw1";
    EXPECT_NE(render(ScreenId::kProvisioning, t).crc32(), base);
    t = s;
    t.prov_ssid = "Quartz-0000";
    EXPECT_NE(render(ScreenId::kProvisioning, t).crc32(), base);
    t = s;
    t.prov_seconds_left = 59;
    EXPECT_NE(render(ScreenId::kProvisioning, t).crc32(), base);
    t = s;
    t.prov_ssid = {};
    const Framebuffer starting = render(ScreenId::kProvisioning, t);
    EXPECT_NE(starting.crc32(), base);
    EXPECT_LT(ink_below_title(starting), ink_below_title(render(ScreenId::kProvisioning, s)));
}

TEST_F(ScreensTest, LongPasswordWrapsToTwoLinesAndStaysInside) {
    WatchState s = nominal_state(settings_);
    s.prov_password = sv(kW32); // 32 chars > 23 per line
    const Framebuffer wrapped = render(ScreenId::kProvisioning, s);
    EXPECT_TRUE(edge_violation(wrapped, tuning::kTitleBarH).empty());
    s.prov_password = "k7Qm2xW9pRtB";
    EXPECT_GT(ink_below_title(wrapped), ink_below_title(render(ScreenId::kProvisioning, s)));
}

TEST_F(ScreensTest, StepsChartReflectsGoalAndHistory) {
    WatchState s = nominal_state(settings_);
    const Framebuffer with_goal = render(ScreenId::kStepsHistory, s);
    s.steps.goal = 0;
    const Framebuffer no_goal = render(ScreenId::kStepsHistory, s);
    EXPECT_NE(with_goal.crc32(), no_goal.crc32());
    s.steps.history_count = 0;
    const Framebuffer empty = render(ScreenId::kStepsHistory, s);
    EXPECT_LT(ink_below_title(empty), ink_below_title(no_goal));
    // A longer bar for a bigger day: compare two histories differing in one day.
    s = nominal_state(settings_);
    s.steps.history[2].steps = 1;
    EXPECT_NE(render(ScreenId::kStepsHistory, s).crc32(), with_goal.crc32());
}

TEST_F(ScreensTest, WeatherDetailVariantsAreDistinct) {
    WatchState s = nominal_state(settings_);
    const auto fresh = render(ScreenId::kWeatherDetail, s).crc32();
    s.weather_freshness = model::WeatherFreshness::kStale;
    const auto stale = render(ScreenId::kWeatherDetail, s).crc32();
    s.weather_freshness = model::WeatherFreshness::kHidden;
    const auto hidden = render(ScreenId::kWeatherDetail, s).crc32();
    s.weather.valid = 0;
    const auto none = render(ScreenId::kWeatherDetail, s).crc32();
    EXPECT_EQ(std::set<std::uint32_t>({fresh, stale, hidden, none}).size(), 4U);
    s = nominal_state(settings_);
    s.temp_unit = model::TempUnit::kFahrenheit;
    EXPECT_NE(render(ScreenId::kWeatherDetail, s).crc32(), fresh);
    s = nominal_state(settings_);
    s.weather.condition = WeatherCondition::kThunder;
    EXPECT_NE(render(ScreenId::kWeatherDetail, s).crc32(), fresh);
}

TEST_F(ScreensTest, SyncNowShowsEachPhase) {
    WatchState s = nominal_state(settings_);
    std::set<std::uint32_t> crcs;
    for (const OpPhase phase :
         {OpPhase::kIdle, OpPhase::kRunning, OpPhase::kSucceeded, OpPhase::kFailed}) {
        s.op_phase = phase;
        crcs.insert(render(ScreenId::kSyncNow, s).crc32());
    }
    EXPECT_EQ(crcs.size(), 4U);
    s.op_phase = OpPhase::kFailed;
    s.op_error = Errc::kTimeout;
    const auto timeout = render(ScreenId::kSyncNow, s).crc32();
    s.op_error = Errc::kNoCredentials;
    EXPECT_NE(render(ScreenId::kSyncNow, s).crc32(), timeout);
}

TEST_F(ScreensTest, StatusOverlayReflectsBatterySavingAndSteps) {
    WatchState s = nominal_state(settings_);
    const auto base = render(ScreenId::kStatusOverlay, s).crc32();
    s.power = model::PowerLevel::kSaver;
    EXPECT_NE(render(ScreenId::kStatusOverlay, s).crc32(), base);
    s = nominal_state(settings_);
    s.steps.today = 100;
    EXPECT_NE(render(ScreenId::kStatusOverlay, s).crc32(), base);
    s = nominal_state(settings_);
    s.battery.usb_present = true;
    s.battery.charging = true;
    EXPECT_NE(render(ScreenId::kStatusOverlay, s).crc32(), base);
}

TEST_F(ScreensTest, ChargeMeAndFactoryResetKeepInputBehaviour) {
    const WatchState s = nominal_state(settings_);
    ASSERT_TRUE(ui_.show(ScreenId::kChargeMe, s).has_value());
    EXPECT_TRUE(ui_.handle(test::click(model::Button::kBack), s).empty());
    EXPECT_EQ(ui_.current(), ScreenId::kChargeMe);
    ASSERT_TRUE(ui_.show(ScreenId::kFactoryReset, s).has_value());
    const auto actions = ui_.handle(test::repeat(model::Button::kMenu, 3000), s);
    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions[0].kind, ActionKind::kFactoryReset);
}

// ---- tests: icons --------------------------------------------------------------------------
int ink(const gfx::Bitmap& b) {
    int n = 0;
    for (const std::uint8_t byte : b.bits) {
        n += std::popcount(byte);
    }
    return n;
}

bool well_formed(const gfx::Bitmap& b, int w, int h) {
    return b.width == w && b.height == h &&
           b.bits.size() == static_cast<std::size_t>((w + 7) / 8) * static_cast<std::size_t>(h);
}

bool same(const gfx::Bitmap& a, const gfx::Bitmap& b) {
    return a.width == b.width && a.height == b.height && std::ranges::equal(a.bits, b.bits);
}

TEST(IconsTest, WeatherIconsAreWellFormedAndDistinct) {
    std::vector<gfx::Bitmap> all;
    for (int i = 0; i <= static_cast<int>(WeatherCondition::kThunder); ++i) {
        const auto b = icons::weather(static_cast<WeatherCondition>(i));
        EXPECT_TRUE(well_formed(b, icons::kWeatherSize, icons::kWeatherSize)) << i;
        EXPECT_GT(ink(b), 30) << i;
        all.push_back(b);
    }
    for (std::size_t a = 0; a < all.size(); ++a) {
        for (std::size_t b = a + 1; b < all.size(); ++b) {
            EXPECT_FALSE(same(all[a], all[b])) << a << " vs " << b;
        }
    }
    // Out-of-range condition falls back to the "unknown" glyph.
    EXPECT_TRUE(same(icons::weather(static_cast<WeatherCondition>(200)),
                     icons::weather(WeatherCondition::kUnknown)));
}

TEST(IconsTest, SyncIconsAreWellFormedAndDistinct) {
    std::vector<gfx::Bitmap> all;
    for (int i = 0; i <= static_cast<int>(SyncIndicator::kOk); ++i) {
        const auto b = icons::sync_state(static_cast<SyncIndicator>(i));
        EXPECT_TRUE(well_formed(b, icons::kSyncSize, icons::kSyncSize)) << i;
        all.push_back(b);
    }
    EXPECT_EQ(ink(all[0]), 0); // kNone: blank
    for (std::size_t i = 1; i < all.size(); ++i) {
        EXPECT_GT(ink(all[i]), 20) << i;
    }
    all.push_back(icons::sync_running());
    EXPECT_TRUE(well_formed(all.back(), icons::kSyncSize, icons::kSyncSize));
    for (std::size_t a = 0; a < all.size(); ++a) {
        for (std::size_t b = a + 1; b < all.size(); ++b) {
            EXPECT_FALSE(same(all[a], all[b])) << a << " vs " << b;
        }
    }
    EXPECT_EQ(ink(icons::sync_state(static_cast<SyncIndicator>(77))), 0);
}

TEST(IconsTest, BatteryLevelsFillMonotonically) {
    using L = icons::BatteryLevel;
    int previous = -1;
    for (const L level : {L::kEmpty, L::kLow, L::kHalf, L::kHigh, L::kFull}) {
        const auto b = icons::battery(level);
        EXPECT_TRUE(well_formed(b, icons::kBatteryW, icons::kBatteryH));
        EXPECT_GT(ink(b), previous);
        previous = ink(b);
    }
    EXPECT_FALSE(same(icons::battery(L::kCharging), icons::battery(L::kEmpty)));
    EXPECT_FALSE(same(icons::battery(L::kUnknown), icons::battery(L::kEmpty)));
    EXPECT_FALSE(same(icons::battery(L::kUnknown), icons::battery(L::kCharging)));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): out-of-range input is the point
    EXPECT_TRUE(same(icons::battery(static_cast<L>(99)), icons::battery(L::kCharging)));
}

TEST(IconsTest, BatteryLevelMapping) {
    using L = icons::BatteryLevel;
    model::BatteryStatus b;
    EXPECT_EQ(icons::battery_level_of(b), L::kUnknown);
    b.valid = true;
    const std::array<std::pair<std::uint8_t, L>, 9> cases{{{0, L::kEmpty},
                                                           {5, L::kEmpty},
                                                           {10, L::kLow},
                                                           {30, L::kLow},
                                                           {35, L::kHalf},
                                                           {55, L::kHalf},
                                                           {60, L::kHigh},
                                                           {80, L::kHigh},
                                                           {85, L::kFull}}};
    for (const auto& [pct, level] : cases) {
        b.percent = pct;
        EXPECT_EQ(icons::battery_level_of(b), level) << int{pct};
    }
    b.percent = 100;
    EXPECT_EQ(icons::battery_level_of(b), L::kFull);
    b.usb_present = true;
    EXPECT_EQ(icons::battery_level_of(b), L::kCharging);
    b.valid = false;
    EXPECT_EQ(icons::battery_level_of(b), L::kCharging); // USB wins over "no sample"
}

TEST(IconsTest, SaverAndWarningGlyphs) {
    EXPECT_TRUE(well_formed(icons::saver(), icons::kSaverSize, icons::kSaverSize));
    EXPECT_TRUE(well_formed(icons::warning(), icons::kWarningSize, icons::kWarningSize));
    EXPECT_GT(ink(icons::saver()), 20);
    EXPECT_GT(ink(icons::warning()), 40);
}

TEST(IconsTest, DrawScalesPixels) {
    const auto bmp = icons::sync_state(SyncIndicator::kOk);
    Framebuffer one;
    Framebuffer ref;
    gfx::Canvas c1(one);
    gfx::Canvas cr(ref);
    icons::draw(c1, 10, 10, bmp, 1);
    cr.bitmap(10, 10, bmp, Color::kBlack);
    EXPECT_EQ(one.bits, ref.bits);

    Framebuffer two;
    gfx::Canvas c2(two);
    icons::draw(c2, 10, 10, bmp, 2);
    EXPECT_EQ(test::black_pixels(two), 4 * test::black_pixels(one));
    Framebuffer three;
    gfx::Canvas c3(three);
    icons::draw(c3, 10, 10, bmp, 3);
    EXPECT_EQ(test::black_pixels(three), 9 * test::black_pixels(one));

    // Malformed bitmaps are ignored rather than read out of bounds.
    const std::array<std::uint8_t, 1> tiny{0xFF};
    Framebuffer bad;
    gfx::Canvas cb(bad);
    icons::draw(cb, 0, 0, gfx::Bitmap{16, 16, tiny}, 2);
    icons::draw(cb, 0, 0, gfx::Bitmap{0, 0, {}}, 2);
    EXPECT_EQ(test::black_pixels(bad), 0);
    // Off-screen placement is clipped by the canvas.
    icons::draw(cb, 190, 190, bmp, 3);
    icons::draw(cb, -20, -20, bmp, 3);
}

// ---- tests: formatting ---------------------------------------------------------------------
constexpr std::string_view kDeg = "\xC2\xB0";

std::string str(std::string_view v) {
    return std::string(v);
}

TEST(FormatTest, Temperature) {
    using model::TempUnit;
    EXPECT_EQ(str(fmt::temperature(215, TempUnit::kCelsius).view()), "22" + str(kDeg) + "C");
    EXPECT_EQ(str(fmt::temperature(-4, TempUnit::kCelsius).view()), "0" + str(kDeg) + "C");
    EXPECT_EQ(str(fmt::temperature(-6, TempUnit::kCelsius).view()), "-1" + str(kDeg) + "C");
    EXPECT_EQ(str(fmt::temperature(0, TempUnit::kFahrenheit).view()), "32" + str(kDeg) + "F");
    EXPECT_EQ(str(fmt::temperature(100, TempUnit::kFahrenheit).view()), "50" + str(kDeg) + "F");
    EXPECT_EQ(str(fmt::temperature(-400, TempUnit::kFahrenheit).view()), "-40" + str(kDeg) + "F");
    EXPECT_FALSE(fmt::temperature(INT16_MIN, TempUnit::kFahrenheit).view().empty());
    EXPECT_FALSE(fmt::temperature(INT16_MAX, TempUnit::kCelsius).view().empty());
}

TEST(FormatTest, HighLow) {
    model::WeatherReport wx;
    wx.high_dc = 150;
    wx.low_dc = 30;
    EXPECT_EQ(str(fmt::high_low(wx, model::TempUnit::kCelsius).view()), "H15 L3");
    wx.low_dc = -50;
    EXPECT_EQ(str(fmt::high_low(wx, model::TempUnit::kCelsius).view()), "H15 L-5");
    wx.high_dc = 100;
    wx.low_dc = 0;
    EXPECT_EQ(str(fmt::high_low(wx, model::TempUnit::kFahrenheit).view()), "H50 L32");
}

TEST(FormatTest, Age) {
    EXPECT_EQ(str(fmt::age(0).view()), "just now");
    EXPECT_EQ(str(fmt::age(59).view()), "just now");
    EXPECT_EQ(str(fmt::age(60).view()), "1 min ago");
    EXPECT_EQ(str(fmt::age(3599).view()), "59 min ago");
    EXPECT_EQ(str(fmt::age(3600).view()), "1 h ago");
    EXPECT_EQ(str(fmt::age(47U * 3600U).view()), "47 h ago");
    EXPECT_EQ(str(fmt::age(48U * 3600U).view()), "2 d ago");
    EXPECT_EQ(str(fmt::age(99U * 86400U).view()), "99 d ago");
    EXPECT_EQ(str(fmt::age(100U * 86400U).view()), "99+ d ago");
    EXPECT_EQ(str(fmt::age(UINT32_MAX).view()), "99+ d ago");
}

TEST(FormatTest, Count) {
    EXPECT_EQ(str(fmt::count(0).view()), "0");
    EXPECT_EQ(str(fmt::count(999).view()), "999");
    EXPECT_EQ(str(fmt::count(1000).view()), "1,000");
    EXPECT_EQ(str(fmt::count(12345).view()), "12,345");
    EXPECT_EQ(str(fmt::count(1234567).view()), "1,234,567");
    EXPECT_EQ(str(fmt::count(UINT32_MAX).view()), "4,294,967,295");
}

TEST(FormatTest, Clock) {
    using time::HourFormat;
    EXPECT_EQ(str(fmt::clock({14, 32, 0}, HourFormat::k24h).view()), "14:32");
    EXPECT_EQ(str(fmt::clock({14, 32, 0}, HourFormat::k12h).view()), "2:32 PM");
    EXPECT_EQ(str(fmt::clock({0, 5, 0}, HourFormat::k12h).view()), "12:05 AM");
    EXPECT_EQ(str(fmt::clock({12, 0, 0}, HourFormat::k12h).view()), "12:00 PM");
    EXPECT_EQ(str(fmt::clock({0, 5, 0}, HourFormat::k24h).view()), "00:05");
    EXPECT_EQ(str(fmt::clock({24, 0, 0}, HourFormat::k24h).view()), "--:--");
    EXPECT_EQ(str(fmt::clock({1, 60, 0}, HourFormat::k12h).view()), "--:--");
}

TEST(FormatTest, LastSync) {
    WatchState s;
    s.time_valid = true;
    const std::int64_t noon =
        static_cast<std::int64_t>(time::days_from_civil({2026, 10, 6})) * 86400;
    constexpr std::int64_t kHour = 3600;
    constexpr std::int64_t kMinute = 60;
    s.last_sync_utc = noon + (14 * kHour) + (32 * kMinute);
    EXPECT_EQ(str(fmt::last_sync(s).view()), "6 Oct 14:32");
    s.hour_format = time::HourFormat::k12h;
    EXPECT_EQ(str(fmt::last_sync(s).view()), "6 Oct 2:32 PM");
    s.hour_format = time::HourFormat::k24h;
    s.local.utc_offset_s = 3600;
    EXPECT_EQ(str(fmt::last_sync(s).view()), "6 Oct 15:32");
    s.last_sync_utc = noon + (23 * kHour) + (30 * kMinute);
    EXPECT_EQ(str(fmt::last_sync(s).view()), "7 Oct 00:30"); // local date rolls over
    s.local.utc_offset_s = -(5 * 3600);
    s.last_sync_utc = noon + kHour;
    EXPECT_EQ(str(fmt::last_sync(s).view()), "5 Oct 20:00");
    s.last_sync_utc = 0;
    EXPECT_EQ(str(fmt::last_sync(s).view()), "never");
    s.last_sync_utc = noon;
    s.time_valid = false;
    EXPECT_EQ(str(fmt::last_sync(s).view()), "?");
    s.time_valid = true;
    s.last_sync_utc = INT64_MAX;
    EXPECT_EQ(str(fmt::last_sync(s).view()), "?");
    s.last_sync_utc = 1;
    s.local.utc_offset_s = -86400; // local time before 1970
    EXPECT_EQ(str(fmt::last_sync(s).view()), "?");
}

TEST(FormatTest, DurationsAndOffsets) {
    EXPECT_EQ(str(fmt::minutes_seconds(299).view()), "4:59");
    EXPECT_EQ(str(fmt::minutes_seconds(0).view()), "0:00");
    EXPECT_EQ(str(fmt::minutes_seconds(3600).view()), "60:00");
    EXPECT_EQ(str(fmt::duration_ms(0).view()), "0.0 s");
    EXPECT_EQ(str(fmt::duration_ms(12345).view()), "12.3 s");
    EXPECT_EQ(str(fmt::duration_ms(60000).view()), "1.0 min");
    EXPECT_EQ(str(fmt::duration_ms(150000).view()), "2.5 min");
    EXPECT_EQ(str(fmt::drift(12345).view()), "+12.3 ppm");
    EXPECT_EQ(str(fmt::drift(-12345).view()), "-12.3 ppm");
    EXPECT_EQ(str(fmt::drift(0).view()), "+0.0 ppm");
    EXPECT_FALSE(fmt::drift(INT32_MIN).view().empty());
    EXPECT_EQ(str(fmt::utc_offset(19800).view()), "+05:30");
    EXPECT_EQ(str(fmt::utc_offset(-28800).view()), "-08:00");
    EXPECT_EQ(str(fmt::utc_offset(0).view()), "+00:00");
    EXPECT_FALSE(fmt::utc_offset(INT32_MIN).view().empty());
}

TEST(FormatTest, NamesAreNonEmptyAndDistinct) {
    std::set<std::string> seen;
    for (int i = 0; i <= static_cast<int>(WeatherCondition::kThunder); ++i) {
        const auto n = fmt::condition_name(static_cast<WeatherCondition>(i));
        EXPECT_FALSE(n.empty());
        seen.insert(str(n));
    }
    EXPECT_EQ(seen.size(), 10U);
    seen.clear();
    for (int i = 0; i < 4; ++i) {
        seen.insert(str(fmt::power_name(static_cast<model::PowerLevel>(i))));
    }
    EXPECT_EQ(seen.size(), 4U);
    seen.clear();
    for (std::size_t i = 0; i < model::kWakeCauseCount; ++i) {
        seen.insert(str(fmt::wake_cause_name(static_cast<model::WakeCause>(i))));
    }
    EXPECT_EQ(seen.size(), model::kWakeCauseCount);
    seen.clear();
    for (int i = 0; i < 3; ++i) {
        seen.insert(str(fmt::connectivity_name(static_cast<model::ConnectivityMode>(i))));
    }
    EXPECT_EQ(seen.size(), 3U);
    seen.clear();
    for (int i = 0; i < 5; ++i) {
        seen.insert(str(fmt::sync_word(static_cast<SyncIndicator>(i))));
    }
    EXPECT_EQ(seen.size(), 5U);
    for (int i = 0; i <= static_cast<int>(Errc::kInternal); ++i) {
        EXPECT_FALSE(fmt::error_text(static_cast<Errc>(i)).empty());
    }
    EXPECT_EQ(fmt::error_text(Errc::kTimeout), "Timed out");
    EXPECT_EQ(fmt::condition_name(static_cast<WeatherCondition>(200)), "Unknown");
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

bool write_png(const std::string& dir, const std::string& name, const Framebuffer& fb) {
    FileSink sink(dir + "/" + name + ".png");
    return sink.ok() && gfx::encode_png(fb, sink).has_value();
}

Framebuffer icon_sheet() {
    Framebuffer fb;
    gfx::Canvas c(fb);
    std::int16_t x = 6;
    std::int16_t y = 6;
    for (int i = 0; i <= static_cast<int>(WeatherCondition::kThunder); ++i) {
        icons::draw(c, x, y, icons::weather(static_cast<WeatherCondition>(i)));
        x = static_cast<std::int16_t>(x + 30);
        if (i == 5) {
            x = 6;
            y = 36;
        }
    }
    y = 66;
    x = 6;
    for (int i = 0; i <= static_cast<int>(SyncIndicator::kOk); ++i) {
        icons::draw(c, x, y, icons::sync_state(static_cast<SyncIndicator>(i)));
        x = static_cast<std::int16_t>(x + 22);
    }
    icons::draw(c, x, y, icons::sync_running());
    icons::draw(c, static_cast<std::int16_t>(x + 22), y, icons::saver());
    icons::draw(c, static_cast<std::int16_t>(x + 44), y, icons::warning());
    y = 92;
    x = 6;
    for (std::size_t i = 0; i < icons::kBatteryLevelCount; ++i) {
        icons::draw(c, x, y, icons::battery(static_cast<icons::BatteryLevel>(i)));
        x = static_cast<std::int16_t>(x + 28);
    }
    icons::draw(c, 6, 112, icons::weather(WeatherCondition::kPartlyCloudy), 3);
    icons::draw(c, 90, 112, icons::sync_state(SyncIndicator::kOk), 3);
    icons::draw(c, 150, 112, icons::battery(icons::BatteryLevel::kHalf), 1);
    return fb;
}

TEST_F(ScreensTest, WritesReviewImagesWhenQzScreenPngDirIsSet) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test, getenv is safe here.
    const char* dir_env = std::getenv("QZ_SCREEN_PNG_DIR");
    if (dir_env == nullptr || *dir_env == '\0') {
        GTEST_SKIP() << "set QZ_SCREEN_PNG_DIR to write screen PNGs";
    }
    const std::string dir = dir_env;
    const WatchState base = nominal_state(settings_);
    ASSERT_TRUE(write_png(dir, "icons", icon_sheet()));

    const auto emit = [&](const std::string& name, ScreenId id, const WatchState& s, int page = 0) {
        EXPECT_TRUE(write_png(dir, name, render(id, s, page))) << name;
    };
    emit("steps", ScreenId::kStepsHistory, base);
    WatchState s = base;
    s.steps.goal = 0;
    s.steps.history_count = 3;
    emit("steps_nogoal_3days", ScreenId::kStepsHistory, s);

    emit("weather_fresh", ScreenId::kWeatherDetail, base);
    s = base;
    s.weather_freshness = model::WeatherFreshness::kStale;
    s.weather.condition = WeatherCondition::kRain;
    s.weather_age_s = 30 * 3600;
    s.temp_unit = model::TempUnit::kFahrenheit;
    emit("weather_stale_f", ScreenId::kWeatherDetail, s);
    s = base;
    s.weather.valid = 0;
    emit("weather_none", ScreenId::kWeatherDetail, s);

    for (const OpPhase phase :
         {OpPhase::kIdle, OpPhase::kRunning, OpPhase::kSucceeded, OpPhase::kFailed}) {
        s = base;
        s.op_phase = phase;
        s.op_error = Errc::kTimeout;
        emit("sync_phase" + std::to_string(static_cast<int>(phase)), ScreenId::kSyncNow, s);
    }
    s = base;
    s.has_credentials = false;
    s.last_sync_utc = 0;
    emit("sync_nocreds", ScreenId::kSyncNow, s);

    emit("provisioning", ScreenId::kProvisioning, base);
    s = base;
    s.prov_ssid = sv(kW32);
    s.prov_password = sv(kW32);
    emit("provisioning_long", ScreenId::kProvisioning, s);

    for (int p = 0; p < kDiagPages; ++p) {
        emit("diag" + std::to_string(p + 1), ScreenId::kDiagnostics, base, p);
    }
    emit("about", ScreenId::kAbout, base);
    s = base;
    s.fw_version = sv(kX90);
    s.git_hash = sv(kM40);
    emit("about_long", ScreenId::kAbout, s);
    emit("factory_reset", ScreenId::kFactoryReset, base);
    emit("charge_me", ScreenId::kChargeMe, base);
    emit("status", ScreenId::kStatusOverlay, base);
    s = base;
    s.battery.usb_present = true;
    s.battery.charging = true;
    s.power = model::PowerLevel::kSaver;
    s.sync = SyncIndicator::kLastFailed;
    s.steps.goal = 0;
    emit("status_charging_saver", ScreenId::kStatusOverlay, s);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::ui
