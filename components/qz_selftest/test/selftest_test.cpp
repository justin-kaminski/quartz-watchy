// Self-test framework tests (WP-19): runner, registry, every suite against testkit fakes,
// failure injection, skip behaviour, interactive prompts (scripted), scene coverage.
#include "qz/faces/registry.hpp"
#include "qz/selftest/selftest.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace qz::selftest {
namespace {

// gtest macros inflate the cognitive-complexity score of table-driven checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)

struct Result3 {
    std::string suite, name, detail;
    Outcome outcome;
    std::uint32_t duration_ms;
};

class Collector final : public ReportSink {
public:
    void on_result(const TestReport& r) override {
        results.push_back({std::string(r.suite),
                           std::string(r.name),
                           std::string(r.detail.view()),
                           r.outcome,
                           r.duration_ms});
    }
    [[nodiscard]] const Result3* find(std::string_view suite, std::string_view name) const {
        for (const Result3& r : results) {
            if (r.suite == suite && r.name == name) {
                return &r;
            }
        }
        return nullptr;
    }
    std::vector<Result3> results;
};

/// Delay that runs a script on every call (button presses while a test "waits").
class ScriptedDelay final : public hal::Delay {
public:
    explicit ScriptedDelay(testkit::VirtualClock& clock) : clock_(clock) {}
    void delay_us(std::uint32_t us) override { clock_.delay_us(us); }
    void delay_ms(std::uint32_t ms) override {
        ++calls;
        if (script) {
            script(calls);
        }
        clock_.delay_ms(ms);
    }
    std::function<void(int)> script;
    int calls = 0;

private:
    testkit::VirtualClock& clock_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): plain-function clock
std::uint32_t g_now_ms = 0;
std::uint32_t fake_now_ms() {
    g_now_ms += 5;
    return g_now_ms;
}

/// Everything a Context needs, wired to fakes.
struct Env {
    testkit::VirtualClock clock;
    testkit::FakeEpdPanel epd{clock};
    ssd1681::Panel panel{epd};
    testkit::FakeBma423 bma{clock};
    bma423::Accelerometer accel{bma, clock};
    testkit::FakeBoardIo io{clock};
    testkit::FakeKvStore kv;
    testkit::FakeSleepSystem sys{clock};
    ScriptedDelay delay{clock};
    faces::Registry faces;
    Context ctx;

    Env() {
        EXPECT_TRUE(accel.init({}).has_value());
        ctx.panel = &panel;
        ctx.accel = &accel;
        ctx.battery_adc = &io;
        ctx.io = &io;
        ctx.kv = &kv;
        ctx.system = &sys;
        ctx.delay = &delay;
        ctx.faces = &faces;
    }
    Collector run(std::string_view filter, Summary* out = nullptr) {
        Collector c;
        const Summary s = selftest::run(ctx, filter, c, &fake_now_ms);
        if (out != nullptr) {
            *out = s;
        }
        return c;
    }
    Outcome outcome(std::string_view suite, std::string_view name) {
        Collector c = run(std::string(suite) + "/" + std::string(name));
        EXPECT_EQ(c.results.size(), 1U) << suite << "/" << name;
        return c.results.empty() ? Outcome::kSkip : c.results[0].outcome;
    }
};

std::size_t count_tests(bool interactive) {
    std::size_t n = 0;
    for (const TestCase& t : all_tests()) {
        n += t.interactive == interactive ? 1U : 0U;
    }
    return n;
}

// ---- registry / runner ----

TEST(Registry, EveryTestIsWellFormedAndUnique) {
    std::set<std::string> seen;
    const std::set<std::string_view> suites{
        "drivers", "settings", "time", "screens", "interactive"};
    ASSERT_FALSE(all_tests().empty());
    for (const TestCase& t : all_tests()) {
        EXPECT_NE(t.fn, nullptr);
        EXPECT_TRUE(suites.contains(t.suite)) << t.suite;
        EXPECT_FALSE(t.name.empty());
        EXPECT_EQ(t.interactive, t.suite == "interactive") << t.name;
        EXPECT_TRUE(seen.insert(std::string(t.suite) + "/" + std::string(t.name)).second);
    }
    for (const std::string_view s : suites) {
        EXPECT_TRUE(std::ranges::any_of(all_tests(), [&](const TestCase& t) {
            return t.suite == s;
        })) << s;
    }
}

TEST(Runner, AllSuitesPassAgainstFakes) {
    Env env;
    Summary sum;
    const Collector c = env.run("all", &sum);
    for (const Result3& r : c.results) {
        if (r.suite != "interactive") {
            EXPECT_EQ(r.outcome, Outcome::kPass) << r.suite << "/" << r.name << ": " << r.detail;
        }
    }
    EXPECT_EQ(sum.failed, 0);
    EXPECT_EQ(sum.passed, count_tests(false));
    EXPECT_EQ(sum.skipped, count_tests(true)); // interactive tests are off
    EXPECT_EQ(c.results.size(), all_tests().size());
    EXPECT_EQ(env.io.vibration_pulses(), 0U); // no vibration without interactive mode
}

TEST(Runner, EmptyFilterMeansAll) {
    Env env;
    Summary sum;
    (void)env.run("", &sum);
    EXPECT_EQ(sum.passed + sum.failed + sum.skipped, all_tests().size());
}

TEST(Runner, FilterBySuiteAndByName) {
    Env env;
    Summary sum;
    const Collector suite = env.run("settings", &sum);
    EXPECT_EQ(suite.results.size(), 5U);
    for (const Result3& r : suite.results) {
        EXPECT_EQ(r.suite, "settings");
    }
    const Collector one = env.run("drivers/battery_adc", &sum);
    ASSERT_EQ(one.results.size(), 1U);
    EXPECT_EQ(one.results[0].name, "battery_adc");
    EXPECT_EQ(sum.passed, 1);
    EXPECT_TRUE(env.run("nosuch", &sum).results.empty());
    EXPECT_EQ(sum.passed + sum.failed + sum.skipped, 0);
    EXPECT_TRUE(env.run("drivers/nosuch", &sum).results.empty());
    EXPECT_TRUE(env.run("driver", &sum).results.empty()); // exact suite match, no prefixes
}

TEST(Runner, DurationsComeFromTheClockCallback) {
    Env env;
    g_now_ms = 100;
    // fake_now_ms advances 5 ms per call; the runner brackets each test with two calls.
    const Collector c = env.run("drivers/system_info");
    ASSERT_EQ(c.results.size(), 1U);
    EXPECT_EQ(c.results[0].duration_ms, 5U);
    EXPECT_EQ(g_now_ms, 110U);
}

TEST(Runner, NullClockIsAllowed) {
    Env env;
    Collector c;
    const Summary s = selftest::run(env.ctx, "drivers/system_info", c, nullptr);
    EXPECT_EQ(s.passed, 1);
}

TEST(Runner, MissingHardwareSkipsInsteadOfFailing) {
    Context empty;
    Collector c;
    const Summary s = selftest::run(empty, "drivers", c, &fake_now_ms);
    EXPECT_EQ(s.failed, 0);
    EXPECT_EQ(s.passed, 0);
    EXPECT_GT(s.skipped, 0);
    // pure suites need no hardware (screens/scenes_crc needs a face source)
    Collector pure;
    const Summary p = selftest::run(empty, "time", pure, &fake_now_ms);
    EXPECT_EQ(p.passed, 4);
    const Summary st = selftest::run(empty, "settings", pure, &fake_now_ms);
    EXPECT_EQ(st.failed, 0);
    EXPECT_EQ(st.passed, 3); // the two NVS tests skip
    EXPECT_EQ(st.skipped, 2);
}

// ---- driver checks: failure injection ----

TEST(Drivers, DisplayFailures) {
    Env env;
    env.epd.fail_next_busy_wait();
    EXPECT_EQ(env.outcome("drivers", "display_init"), Outcome::kFail);
    Env cold;
    cold.epd.set_temperature_dc(-600);
    // out-of-range panel temperature is refused by the model before the check can read a value
    const Outcome t = cold.outcome("drivers", "display_temp");
    EXPECT_NE(t, Outcome::kSkip);
    Env hot;
    hot.epd.set_temperature_dc(900);
    EXPECT_EQ(hot.outcome("drivers", "display_temp"), Outcome::kFail);
}

TEST(Drivers, DisplayFullAndPartialShowPatterns) {
    Env env;
    EXPECT_EQ(env.outcome("drivers", "display_full"), Outcome::kPass);
    EXPECT_EQ(env.epd.full_updates(), 1U);
    EXPECT_GT(env.epd.displayed().crc32(), 0U);
    EXPECT_EQ(env.outcome("drivers", "display_partial"), Outcome::kPass);
    EXPECT_EQ(env.epd.partial_updates(), 1U);
    EXPECT_EQ(env.epd.violation_count(), 0U);
}

TEST(Drivers, AccelFailures) {
    Env wrong_id;
    wrong_id.bma.set_chip_id(0x42);
    EXPECT_EQ(wrong_id.outcome("drivers", "accel_chip_id"), Outcome::kFail);
    Env no_features;
    no_features.bma.sensor_reset(); // config lost
    EXPECT_EQ(no_features.outcome("drivers", "accel_features"), Outcome::kFail);
    Env io_error;
    io_error.bma.fail_io(true);
    EXPECT_EQ(io_error.outcome("drivers", "accel_chip_id"), Outcome::kFail);
    EXPECT_EQ(io_error.outcome("drivers", "accel_features"), Outcome::kFail);
}

TEST(Drivers, BatteryRange) {
    Env env;
    env.io.set_pin_mv(1500); // 1.9 V behind the divider
    EXPECT_EQ(env.outcome("drivers", "battery_adc"), Outcome::kFail);
    env.io.set_pin_mv(3400); // 4.3 V: inside
    EXPECT_EQ(env.outcome("drivers", "battery_adc"), Outcome::kPass);
    env.io.set_pin_mv(3500); // 4.47 V: above 4.4 V
    EXPECT_EQ(env.outcome("drivers", "battery_adc"), Outcome::kFail);
    env.io.fail_adc(true);
    EXPECT_EQ(env.outcome("drivers", "battery_adc"), Outcome::kFail);
}

TEST(Drivers, ButtonsAndUsbPins) {
    Env env;
    env.io.press(hal::kButtonBitUp);
    Collector c = env.run("drivers/buttons_idle");
    ASSERT_EQ(c.results.size(), 1U);
    EXPECT_EQ(c.results[0].outcome, Outcome::kFail);
    EXPECT_NE(c.results[0].detail.find("mask 4"), std::string::npos);
    env.io.release(hal::kButtonBitUp);
    EXPECT_EQ(env.outcome("drivers", "buttons_idle"), Outcome::kPass);
    env.io.set_usb(true, true);
    EXPECT_EQ(env.outcome("drivers", "usb_pins"), Outcome::kPass);
    env.io.set_usb(false, false);
    EXPECT_EQ(env.outcome("drivers", "usb_pins"), Outcome::kPass);
}

TEST(Drivers, SlowClockToleranceIsPlusMinus500Ppm) {
    Env env;
    env.sys.set_slow_clock({.external_crystal = true, .measured_hz = 32768 + 16});
    EXPECT_EQ(env.outcome("drivers", "rtc_clock"), Outcome::kPass); // 488 ppm
    env.sys.set_slow_clock({.external_crystal = true, .measured_hz = 32768 - 16});
    EXPECT_EQ(env.outcome("drivers", "rtc_clock"), Outcome::kPass);
    env.sys.set_slow_clock({.external_crystal = true, .measured_hz = 32768 + 17}); // 519 ppm
    EXPECT_EQ(env.outcome("drivers", "rtc_clock"), Outcome::kFail);
    env.sys.set_slow_clock({.external_crystal = true, .measured_hz = 32768 - 17});
    EXPECT_EQ(env.outcome("drivers", "rtc_clock"), Outcome::kFail);
    env.sys.set_slow_clock({.external_crystal = false, .measured_hz = 32768});
    EXPECT_EQ(env.outcome("drivers", "rtc_clock"), Outcome::kFail); // RC fallback
    env.sys.set_slow_clock({.external_crystal = true, .measured_hz = 0});
    EXPECT_EQ(env.outcome("drivers", "rtc_clock"), Outcome::kSkip); // not calibrated
}

TEST(Drivers, SystemInfoFailsWithoutHeap) {
    Env env;
    env.sys.set_heap({.free_bytes = 0, .min_free_bytes = 0});
    EXPECT_EQ(env.outcome("drivers", "system_info"), Outcome::kFail);
}

TEST(Drivers, NvsRoundTripLeavesNothingBehind) {
    Env env;
    EXPECT_EQ(env.outcome("drivers", "nvs_roundtrip"), Outcome::kPass);
    EXPECT_EQ(env.kv.entry_count("qz_test"), 0U);
    EXPECT_GT(env.kv.commit_count(), 0U);
    Env failing;
    failing.kv.fail_writes_after(1);
    EXPECT_EQ(failing.outcome("drivers", "nvs_roundtrip"), Outcome::kFail);
}

// ---- settings suite ----

TEST(Settings, PersistTestNeverTouchesRealNamespaces) {
    Env env;
    EXPECT_EQ(env.outcome("settings", "persist_roundtrip"), Outcome::kPass);
    EXPECT_EQ(env.outcome("settings", "restore_defaults"), Outcome::kPass);
    EXPECT_EQ(env.kv.entry_count("qz_set"), 0U);
    EXPECT_EQ(env.kv.entry_count("qz_cred"), 0U);
    EXPECT_EQ(env.kv.entry_count("qz_test"), 0U);
    EXPECT_GT(env.kv.write_count(), 0U);
}

TEST(Settings, PersistFailsWhenFlashFails) {
    Env env;
    env.kv.fail_writes_after(2);
    EXPECT_EQ(env.outcome("settings", "persist_roundtrip"), Outcome::kFail);
}

TEST(Settings, ExistingUserSettingsSurviveTheSuite) {
    Env env;
    ASSERT_TRUE(env.kv.set_str("qz_set", "tz", "Europe/Berlin").has_value());
    (void)env.run("settings");
    std::array<char, 40> buf{};
    const Result<std::size_t> tz = env.kv.get_str("qz_set", "tz", buf);
    ASSERT_TRUE(tz.has_value());
    EXPECT_EQ(std::string(buf.data(), *tz), "Europe/Berlin");
}

// ---- screens suite ----

TEST(Screens, CrcFailsWhenARendererChanges) {
    // A face source that draws something else must not match the goldens.
    class Odd final : public ui::FaceSource {
    public:
        [[nodiscard]] std::size_t count() const override { return 1; }
        [[nodiscard]] std::uint8_t id_at(std::size_t /*index*/) const override { return 0; }
        [[nodiscard]] std::string_view name_of(std::uint8_t /*id*/) const override { return "odd"; }
        void render(std::uint8_t /*id*/,
                    const ui::WatchState& /*state*/,
                    gfx::Canvas& c) const override {
            c.fill_rect({0, 0, 9, 9}, gfx::Color::kBlack);
        }
    } odd;
    Env env;
    env.ctx.faces = &odd;
    Collector c = env.run("screens/scenes_crc");
    ASSERT_EQ(c.results.size(), 1U);
    EXPECT_EQ(c.results[0].outcome, Outcome::kFail);
    EXPECT_NE(c.results[0].detail.find("face_"), std::string::npos) << c.results[0].detail;
    env.ctx.faces = nullptr;
    EXPECT_EQ(env.outcome("screens", "scenes_crc"), Outcome::kSkip);
}

// ---- interactive suite ----

TEST(Interactive, SkippedUnlessEnabled) {
    Env env;
    env.delay.script = [&](int) {
        FAIL() << "interactive test must not wait";
    };
    const Collector c = env.run("interactive");
    EXPECT_EQ(c.results.size(), 5U);
    for (const Result3& r : c.results) {
        EXPECT_EQ(r.outcome, Outcome::kSkip);
    }
    EXPECT_EQ(env.delay.calls, 0);
}

TEST(Interactive, EachButtonIsPromptedAndAccepted) {
    struct Case {
        const char* name;
        std::uint8_t bit;
    };
    for (const Case c : {Case{"button_menu", hal::kButtonBitMenu},
                         Case{"button_back", hal::kButtonBitBack},
                         Case{"button_up", hal::kButtonBitUp},
                         Case{"button_down", hal::kButtonBitDown}}) {
        Env env;
        env.ctx.interactive = true;
        env.delay.script = [&](int call) {
            if (call == 3) {
                env.io.press(c.bit);
            } else if (call == 5) {
                env.io.release(c.bit);
            }
        };
        EXPECT_EQ(env.outcome("interactive", c.name), Outcome::kPass) << c.name;
        EXPECT_EQ(env.epd.partial_updates(), 1U) << "prompt drawn";
        EXPECT_EQ(env.io.pressed_buttons(), 0);
    }
}

TEST(Interactive, WrongButtonAndTimeoutFail) {
    Env wrong;
    wrong.ctx.interactive = true;
    wrong.delay.script = [&](int call) {
        if (call == 2) {
            wrong.io.press(hal::kButtonBitBack);
        } else if (call == 4) {
            wrong.io.release(hal::kButtonBitBack);
        }
    };
    EXPECT_EQ(wrong.outcome("interactive", "button_menu"), Outcome::kFail);
    Env timeout;
    timeout.ctx.interactive = true;
    EXPECT_EQ(timeout.outcome("interactive", "button_up"), Outcome::kFail);
    EXPECT_EQ(timeout.delay.calls, 500); // 10 s in 20 ms steps, then gives up
}

TEST(Interactive, VibrationPulsesThenAsksForConfirmation) {
    for (const bool buzzed : {true, false}) {
        Env env;
        env.ctx.interactive = true;
        const std::uint8_t answer = buzzed ? hal::kButtonBitUp : hal::kButtonBitDown;
        env.delay.script = [&](int call) {
            EXPECT_EQ(env.io.vibrating(), call == 1) << "motor only on during the 300 ms pulse";
            if (call == 4) {
                env.io.press(answer);
            } else if (call == 6) {
                env.io.release(answer);
            }
        };
        EXPECT_EQ(env.outcome("interactive", "vibration"),
                  buzzed ? Outcome::kPass : Outcome::kFail);
        EXPECT_EQ(env.io.vibration_pulses(), 1U);
        EXPECT_FALSE(env.io.vibrating());
        EXPECT_EQ(env.io.vibration_ms_total(), 300U);
    }
}

TEST(Interactive, MissingPanelSkips) {
    Env env;
    env.ctx.interactive = true;
    env.ctx.panel = nullptr;
    EXPECT_EQ(env.outcome("interactive", "button_menu"), Outcome::kSkip);
    EXPECT_EQ(env.outcome("interactive", "vibration"), Outcome::kSkip);
    EXPECT_EQ(env.io.vibration_pulses(), 0U);
}

// ---- scenes ----

TEST(Scenes, EveryScreenIdHasAScene) {
    for (std::uint8_t id = 0; id < static_cast<std::uint8_t>(ui::ScreenId::kCount); ++id) {
        const auto sid = static_cast<ui::ScreenId>(id);
        EXPECT_TRUE(std::ranges::any_of(scenes(), [&](const Scene& s) { return s.screen == sid; }))
            << ui::screen_name(sid);
    }
}

TEST(Scenes, EveryFaceHasAScene) {
    for (const faces::FaceDescriptor& face : faces::descriptors()) {
        bool found = false;
        for (const Scene& s : scenes()) {
            settings::Settings backing = settings::defaults();
            ui::WatchState state;
            s.build_state(state, backing);
            found = found || (s.screen == ui::ScreenId::kFace && backing.face_id == face.id);
        }
        EXPECT_TRUE(found) << face.name;
    }
}

TEST(Scenes, FaceVariantsAreCovered) {
    // every visible condition the face contract lists is pinned by at least one fixture
    bool saw_24h = false;
    bool saw_12h = false;
    bool saw_invalid = false;
    bool saw_goal = false;
    bool saw_nogoal = false;
    std::set<model::SyncIndicator> sync;
    std::set<model::WeatherFreshness> fresh;
    std::set<model::PowerLevel> power;
    bool saw_charging = false;
    for (const Scene& s : scenes()) {
        if (s.screen != ui::ScreenId::kFace) {
            continue;
        }
        settings::Settings backing = settings::defaults();
        ui::WatchState st;
        s.build_state(st, backing);
        saw_24h = saw_24h || (st.time_valid && st.hour_format == model::HourFormat::k24h);
        saw_12h = saw_12h || (st.time_valid && st.hour_format == model::HourFormat::k12h);
        saw_invalid = saw_invalid || !st.time_valid;
        saw_goal = saw_goal || st.steps.goal != 0;
        saw_nogoal = saw_nogoal || st.steps.goal == 0;
        sync.insert(st.sync);
        fresh.insert(st.weather_freshness);
        power.insert(st.power);
        saw_charging = saw_charging || st.battery.charging;
    }
    EXPECT_TRUE(saw_24h && saw_12h && saw_invalid && saw_goal && saw_nogoal && saw_charging);
    EXPECT_EQ(sync.size(), 5U);
    EXPECT_EQ(fresh.size(), 3U);
    EXPECT_EQ(power.size(), 4U);
}

TEST(Scenes, NamesAreUniqueFileSafeAndShort) {
    std::set<std::string_view> names;
    for (const Scene& s : scenes()) {
        EXPECT_TRUE(names.insert(s.name).second) << s.name;
        EXPECT_LE(s.name.size(), 40U) << s.name;
        EXPECT_TRUE(std::ranges::all_of(s.name, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        })) << s.name;
    }
}

TEST(Scenes, FixturesUseOnlyTheFixedVersionStrings) {
    for (const Scene& s : scenes()) {
        settings::Settings backing = settings::defaults();
        ui::WatchState st;
        s.build_state(st, backing);
        EXPECT_EQ(st.fw_version, "1.2.3") << s.name;
        EXPECT_EQ(st.git_hash, "abc1234") << s.name;
    }
}

TEST(Scenes, RenderIsDeterministicAndNotBlank) {
    const faces::Registry faces;
    for (const Scene& s : scenes()) {
        gfx::Framebuffer a;
        gfx::Framebuffer b;
        ASSERT_TRUE(render_scene(s, faces, a).has_value()) << s.name;
        // pre-dirty the target: render must not depend on its previous content
        b.clear(gfx::Color::kBlack);
        ASSERT_TRUE(render_scene(s, faces, b).has_value()) << s.name;
        EXPECT_EQ(a.bits, b.bits) << s.name;
        EXPECT_NE(a.crc32(), gfx::Framebuffer{}.crc32()) << s.name << " is blank";
    }
}

TEST(Scenes, DistinctScenesRenderDifferently) {
    const faces::Registry faces;
    std::set<std::uint32_t> crcs;
    for (const Scene& s : scenes()) {
        gfx::Framebuffer fb;
        ASSERT_TRUE(render_scene(s, faces, fb).has_value());
        EXPECT_TRUE(crcs.insert(fb.crc32()).second) << s.name << " renders like another scene";
    }
}

TEST(Scenes, InputScriptsAreApplied) {
    const faces::Registry faces;
    const auto find = [](std::string_view name) -> const Scene& {
        for (const Scene& s : scenes()) {
            if (s.name == name) {
                return s;
            }
        }
        ADD_FAILURE() << name;
        return scenes()[0];
    };
    gfx::Framebuffer plain;
    gfx::Framebuffer moved;
    ASSERT_TRUE(render_scene(find("screen_menu"), faces, plain).has_value());
    ASSERT_TRUE(render_scene(find("screen_menu_down3"), faces, moved).has_value());
    EXPECT_NE(plain.crc32(), moved.crc32());
    EXPECT_FALSE(find("screen_menu_down3").inputs.empty());
}

TEST(Scenes, RenderRejectsBadScenes) {
    const faces::Registry faces;
    gfx::Framebuffer fb;
    const Scene no_builder{"x", ui::ScreenId::kFace, nullptr, {}};
    EXPECT_EQ(render_scene(no_builder, faces, fb).error().code, Errc::kBadArgs);
    const Scene bad_screen{"x", ui::ScreenId::kCount, scenes()[0].build_state, {}};
    EXPECT_EQ(render_scene(bad_screen, faces, fb).error().code, Errc::kBadArgs);
}

TEST(Scenes, GoldenTableMatchesTheRenderer) {
    const faces::Registry faces;
    ASSERT_EQ(golden_crcs().size(), scenes().size());
    for (const Scene& s : scenes()) {
        const auto it = std::ranges::find(golden_crcs(), s.name, &GoldenCrc::scene);
        ASSERT_NE(it, golden_crcs().end()) << s.name;
        gfx::Framebuffer fb;
        ASSERT_TRUE(render_scene(s, faces, fb).has_value());
        EXPECT_EQ(fb.crc32(), it->crc32) << s.name << ": run qz_golden --update";
    }
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::selftest
