// Simulator library tests (WP-23): argument parsing table, scripted-state derivation through the
// real TimeZone engine (Europe/Berlin spring-forward), determinism, PNG output, face differences.
#include "qz_sim.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qz::sim {
namespace {

// NOLINTBEGIN(readability-function-cognitive-complexity)

ParseOutcome parse(std::vector<const char*> argv) {
    return parse_args(argv);
}

Args must_parse(std::vector<const char*> argv) {
    const ParseOutcome r = parse(std::move(argv));
    EXPECT_TRUE(r.ok()) << r.error;
    return r.args;
}

std::uint32_t be32(const std::vector<std::uint8_t>& v, std::size_t off) {
    return (static_cast<std::uint32_t>(v[off]) << 24U) |
           (static_cast<std::uint32_t>(v[off + 1]) << 16U) |
           (static_cast<std::uint32_t>(v[off + 2]) << 8U) | v[off + 3];
}

TEST(SimParse, DefaultsAndHelp) {
    const ParseOutcome r = parse({});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.args.face, "default");
    EXPECT_EQ(r.args.scale, 1);
    EXPECT_EQ(r.args.out, "face.png");
    EXPECT_TRUE(r.args.time_valid);
    EXPECT_FALSE(r.args.help);
    EXPECT_TRUE(parse({"--help"}).args.help);
    EXPECT_FALSE(usage_text().empty());
}

TEST(SimParse, FullCommandLine) {
    const Args a = must_parse({"--face",
                               "minimal",
                               "--time",
                               "2026-10-06T08:15:30",
                               "--tz",
                               "Europe/Berlin",
                               "--hour-format",
                               "12",
                               "--steps",
                               "7421",
                               "--goal",
                               "0",
                               "--battery",
                               "76",
                               "--charging",
                               "--power",
                               "saver",
                               "--weather",
                               "temp_c=-3,code=61,age_min=20,high_c=2,low_c=-8",
                               "--sync",
                               "failed",
                               "--utc",
                               "--scale",
                               "3",
                               "--out",
                               "x.png"});
    EXPECT_EQ(a.face, "minimal");
    EXPECT_EQ(a.date, (time::CivilDate{2026, 10, 6}));
    EXPECT_EQ(a.time, (time::CivilTime{8, 15, 30}));
    EXPECT_EQ(a.tz, "Europe/Berlin");
    EXPECT_EQ(a.hour_format, model::HourFormat::k12h);
    EXPECT_EQ(a.steps, 7421U);
    EXPECT_EQ(a.goal, 0U);
    EXPECT_EQ(a.battery, 76);
    EXPECT_TRUE(a.charging);
    EXPECT_EQ(a.power, model::PowerLevel::kSaver);
    if (!a.weather.has_value()) {
        FAIL() << "--weather not parsed";
    }
    const WeatherSpec w = a.weather.value();
    EXPECT_EQ(w.temp_c, -3);
    EXPECT_EQ(w.wmo_code, 61);
    EXPECT_EQ(w.age_min, 20U);
    EXPECT_TRUE(w.has_high_low);
    EXPECT_EQ(w.low_c, -8);
    EXPECT_EQ(a.sync, model::SyncIndicator::kLastFailed);
    EXPECT_TRUE(a.time_is_utc);
    EXPECT_EQ(a.scale, 3);
    EXPECT_EQ(a.out, "x.png");
    EXPECT_FALSE(must_parse({"--time-invalid"}).time_valid);
}

TEST(SimParse, InvalidInputsAreRejected) {
    const std::vector<std::vector<const char*>> bad{
        {"--bogus"},
        {"--bogus", "1"},
        {"positional"},
        {"--face"},                        // missing value
        {"--face", "neon"},                // not registered
        {"--time", "2026-10-06 08:15:00"}, // wrong separator
        {"--time", "2026-02-30T08:15:00"}, // no such day
        {"--time", "2026-10-06T24:00:00"}, // hour out of range
        {"--time", "1969-12-31T23:59:59"}, // before the supported range
        {"--tz", "Mars/Olympus"},
        {"--hour-format", "13"},
        {"--steps", "-1"},
        {"--steps", "12x"},
        {"--steps", "1000000"},
        {"--goal", ""},
        {"--battery", "101"},
        {"--power", "turbo"},
        {"--sync", "maybe"},
        {"--scale", "0"},
        {"--scale", "4"},
        {"--out", ""},
        {"--weather", "temp_c=18,code"},      // no '='
        {"--weather", "code=61"},             // temp_c required
        {"--weather", "temp_c=18,high_c=20"}, // high without low
        {"--weather", "temp_c=18,wind=3"},    // unknown key
        {"--weather", "temp_c=99"},           // out of range
    };
    for (const auto& argv : bad) {
        const ParseOutcome r = parse(argv);
        EXPECT_FALSE(r.ok()) << "accepted: " << argv[0];
    }
}

TEST(SimScene, LocalTimeAcrossBerlinSpringForward) {
    // 2026-03-29: 02:00 CET jumps to 03:00 CEST at 01:00:00 UTC (= 1774746000).
    const auto scene_of = [](std::vector<const char*> argv) {
        argv.insert(argv.end(), {"--tz", "Europe/Berlin"});
        const Result<Scene> s = build_scene(must_parse(std::move(argv)));
        EXPECT_TRUE(s.has_value());
        return *s;
    };
    { // last second before the jump, local input
        const Scene s = scene_of({"--time", "2026-03-29T01:59:59"});
        EXPECT_EQ(s.utc, 1774746000 - 1);
        EXPECT_EQ(s.state.local.time, (time::CivilTime{1, 59, 59}));
        EXPECT_EQ(s.state.local.utc_offset_s, 3600);
        EXPECT_FALSE(s.state.local.is_dst);
    }
    { // the instant of the jump given in UTC
        const Scene s = scene_of({"--time", "2026-03-29T01:00:00", "--utc"});
        EXPECT_EQ(s.utc, 1774746000);
        EXPECT_EQ(s.state.local.date, (time::CivilDate{2026, 3, 29}));
        EXPECT_EQ(s.state.local.time, (time::CivilTime{3, 0, 0}));
        EXPECT_EQ(s.state.local.utc_offset_s, 7200);
        EXPECT_TRUE(s.state.local.is_dst);
    }
    { // non-existent local time 02:30 is shifted forward by the gap length
        const Scene s = scene_of({"--time", "2026-03-29T02:30:00"});
        EXPECT_EQ(s.state.local.time, (time::CivilTime{3, 30, 0}));
        EXPECT_TRUE(s.state.local.is_dst);
    }
    { // one second before the jump in UTC is still CET
        const Scene s = scene_of({"--time", "2026-03-29T00:59:59", "--utc"});
        EXPECT_EQ(s.state.local.time, (time::CivilTime{1, 59, 59}));
        EXPECT_EQ(s.state.local.utc_offset_s, 3600);
    }
}

TEST(SimScene, StateFieldsAndWeatherFreshness) {
    const Scene s = *build_scene(must_parse({"--steps",
                                             "7421",
                                             "--battery",
                                             "76",
                                             "--charging",
                                             "--weather",
                                             "temp_c=18,code=61,age_min=20",
                                             "--sync",
                                             "ok",
                                             "--hour-format",
                                             "12"}));
    EXPECT_TRUE(s.state.time_valid);
    EXPECT_EQ(s.state.steps.today, 7421U);
    EXPECT_EQ(s.state.steps.goal, 10000U);
    EXPECT_EQ(s.state.battery.percent, 76);
    EXPECT_TRUE(s.state.battery.charging);
    EXPECT_TRUE(s.state.battery.usb_present);
    EXPECT_EQ(s.state.weather.temp_dc, 180);
    EXPECT_EQ(s.state.weather.condition, model::WeatherCondition::kRain);
    EXPECT_EQ(s.state.weather_age_s, 1200U);
    EXPECT_EQ(s.state.weather_freshness, model::WeatherFreshness::kFresh);
    EXPECT_EQ(s.state.sync, model::SyncIndicator::kOk);
    EXPECT_EQ(s.state.hour_format, model::HourFormat::k12h);

    const Scene stale = *build_scene(must_parse({"--weather", "temp_c=18,code=1,age_min=240"}));
    EXPECT_EQ(stale.state.weather_freshness, model::WeatherFreshness::kStale);
    const Scene none = *build_scene(must_parse({}));
    EXPECT_EQ(none.state.weather_freshness, model::WeatherFreshness::kHidden);
    const Scene bad_clock = *build_scene(must_parse({"--time-invalid", "--weather", "temp_c=1"}));
    EXPECT_FALSE(bad_clock.state.time_valid);
    EXPECT_EQ(bad_clock.state.weather_freshness, model::WeatherFreshness::kHidden);
}

TEST(SimRender, Deterministic) {
    const Args a = must_parse({"--steps", "5000", "--weather", "temp_c=18,code=3"});
    const auto first = render_png(a);
    const auto second = render_png(a);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(render_frame(a)->crc32(), render_frame(a)->crc32());
}

TEST(SimRender, PngSignatureAndSize) {
    constexpr std::array<std::uint8_t, 8> kSig{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    for (const int scale : {1, 2, 3}) {
        const std::string scale_arg = std::to_string(scale);
        const auto png = render_png(must_parse({"--scale", scale_arg.c_str()}));
        ASSERT_TRUE(png.has_value()) << scale;
        ASSERT_GT(png->size(), 33U);
        EXPECT_TRUE(std::equal(kSig.begin(), kSig.end(), png->begin())) << scale;
        EXPECT_EQ(std::string(png->begin() + 12, png->begin() + 16), "IHDR");
        EXPECT_EQ(be32(*png, 16), static_cast<std::uint32_t>(200 * scale)) << scale;
        EXPECT_EQ(be32(*png, 20), static_cast<std::uint32_t>(200 * scale)) << scale;
        EXPECT_EQ((*png)[24], scale == 1 ? 1 : 8) << "bit depth";
        EXPECT_EQ(std::string(png->end() - 8, png->end() - 4), "IEND");
    }
}

TEST(SimRender, ScaledImageIsLargerAndStoredBlocksAreConsistent) {
    const auto p2 = render_png(must_parse({"--scale", "2"}));
    const auto p3 = render_png(must_parse({"--scale", "3"}));
    ASSERT_TRUE(p2.has_value() && p3.has_value());
    // Raw size = (side + 1) * side, stored (uncompressed) plus a few bytes of framing per block.
    EXPECT_GT(p2->size(), 400U * 401U);
    EXPECT_GT(p3->size(), 600U * 601U);
    EXPECT_LT(p3->size(), (600U * 601U) + 200U);
}

TEST(SimRender, FacesDiffer) {
    const auto d = render_frame(must_parse({"--face", "default", "--weather", "temp_c=18,code=1"}));
    const auto m = render_frame(must_parse({"--face", "minimal", "--weather", "temp_c=18,code=1"}));
    ASSERT_TRUE(d.has_value() && m.has_value());
    EXPECT_NE(d->crc32(), m->crc32());
}

TEST(SimRender, StateChangesTheImage) {
    const auto a = render_frame(must_parse({"--time", "2026-10-06T08:15:00"}));
    const auto b = render_frame(must_parse({"--time", "2026-10-06T08:16:00"}));
    const auto c = render_frame(must_parse({"--time-invalid"}));
    ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
    EXPECT_NE(a->crc32(), b->crc32());
    EXPECT_NE(a->crc32(), c->crc32());
}

TEST(SimRender, UnknownZoneFails) {
    Args a;
    a.tz = "Nowhere/Land";
    const auto r = render_png(a);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Errc::kBadArgs);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::sim
