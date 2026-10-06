// Golden image tests (docs/TEST_PLAN.md layer G): every scene's PNG is compared byte for byte
// with the committed file; the generated CRC table must equal the committed golden_crc.inc.
// On failure the actual and diff PNGs are written to $QZ_GOLDEN_OUT_DIR (default ./golden_failures)
// and the number of differing pixels is printed. Update with: build/<dir>/golden/qz_golden --update
#include "../../../host/golden/golden_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

#ifndef QZ_GOLDEN_DIR
#error "QZ_GOLDEN_DIR must be defined by the build"
#endif

namespace qz::golden {
namespace {

// gtest macros inflate the cognitive-complexity score.
// NOLINTBEGIN(readability-function-cognitive-complexity)

/// Value of an optional the test already asserted on (keeps static analysis happy).
template<class T>
const T& need(const std::optional<T>& value) {
    if (!value) {
        std::abort();
    }
    return *value;
}

fs::path golden_dir() {
    return QZ_GOLDEN_DIR;
}
fs::path crc_inc_path() {
    return golden_dir() / ".." / "src" / "golden_crc.inc";
}
fs::path failure_dir() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, single-threaded test
    const char* env = std::getenv("QZ_GOLDEN_OUT_DIR");
    return env != nullptr ? fs::path(env) : fs::path("golden_failures");
}

class SceneGolden : public ::testing::TestWithParam<std::size_t> {};

TEST_P(SceneGolden, MatchesCommittedPng) {
    const selftest::Scene& scene = selftest::scenes()[GetParam()];
    const faces::Registry faces;
    const SceneCheck r = check_scene(scene, faces, golden_dir(), failure_dir());
    EXPECT_TRUE(r.ok) << scene.name << ": " << r.message;
}

TEST_P(SceneGolden, PngAgreesWithCrcTable) {
    const selftest::Scene& scene = selftest::scenes()[GetParam()];
    const std::optional<Bytes> png = read_file(png_path(golden_dir(), scene.name));
    ASSERT_TRUE(png.has_value()) << "missing golden for " << scene.name;
    const std::optional<gfx::Framebuffer> fb = decode(need(png));
    ASSERT_TRUE(fb.has_value()) << scene.name << ".png is not a Quartz PNG";
    const auto it =
        std::ranges::find(selftest::golden_crcs(), scene.name, &selftest::GoldenCrc::scene);
    ASSERT_NE(it, selftest::golden_crcs().end()) << scene.name;
    EXPECT_EQ(need(fb).crc32(), it->crc32) << scene.name;
}

INSTANTIATE_TEST_SUITE_P(Scenes,
                         SceneGolden,
                         ::testing::Range<std::size_t>(0, selftest::scenes().size()),
                         [](const ::testing::TestParamInfo<std::size_t>& param) {
                             return std::string(selftest::scenes()[param.param].name);
                         });

TEST(GoldenSet, NoStaleGoldenFiles) {
    EXPECT_TRUE(orphan_goldens(golden_dir()).empty())
        << "PNG without a scene: " << orphan_goldens(golden_dir()).front();
}

TEST(GoldenSet, RegeneratedCrcTableEqualsCommittedInc) {
    const faces::Registry faces;
    const std::optional<std::string> text = crc_table_text(faces);
    ASSERT_TRUE(text.has_value());
    const std::optional<Bytes> committed = read_file(crc_inc_path());
    ASSERT_TRUE(committed.has_value()) << crc_inc_path();
    EXPECT_EQ(std::string(need(committed).begin(), need(committed).end()), need(text))
        << "golden_crc.inc is stale: run qz_golden --update";
}

// ---- the machinery itself ----

class Machinery : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per process: ctest runs each test in its own process, in parallel.
        root_ = fs::path(::testing::TempDir()) /
                ("qz_golden_machinery_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_ / "golden", ec);
        fs::create_directories(root_ / "out", ec);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    fs::path root_;
    faces::Registry faces_;
};

TEST_F(Machinery, EncodeDecodeRoundTrip) {
    gfx::Framebuffer fb;
    gfx::Canvas c(fb);
    c.circle(100, 100, 60, false, gfx::Color::kBlack);
    c.fill_rect({3, 5, 17, 9}, gfx::Color::kBlack);
    const std::optional<Bytes> png = encode(fb);
    ASSERT_TRUE(png.has_value());
    const std::optional<gfx::Framebuffer> back = decode(need(png));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(need(back).bits, fb.bits);
    Bytes garbage = need(png);
    garbage.resize(garbage.size() - 1);
    EXPECT_FALSE(decode(garbage).has_value());
    garbage = need(png);
    garbage[20] ^= 0xFFU; // inside IHDR
    EXPECT_FALSE(decode(garbage).has_value());
}

TEST_F(Machinery, DiffHelpers) {
    const gfx::Framebuffer a;
    gfx::Framebuffer b;
    b.set(0, 0, gfx::Color::kBlack);
    b.set(199, 199, gfx::Color::kBlack);
    b.set(100, 50, gfx::Color::kBlack);
    EXPECT_EQ(diff_pixels(a, a), 0U);
    EXPECT_EQ(diff_pixels(a, b), 3U);
    const gfx::Framebuffer d = diff_image(a, b);
    EXPECT_EQ(diff_pixels(d, b), 0U);
}

TEST_F(Machinery, CheckPassesOnIdenticalPng) {
    const selftest::Scene& scene = selftest::scenes()[0];
    const std::optional<gfx::Framebuffer> fb = render(scene, faces_);
    ASSERT_TRUE(fb.has_value());
    ASSERT_TRUE(write_file(png_path(root_ / "golden", scene.name), need(encode(need(fb)))));
    const SceneCheck r = check_scene(scene, faces_, root_ / "golden", root_ / "out");
    EXPECT_TRUE(r.ok) << r.message;
    EXPECT_FALSE(fs::exists(root_ / "out" / (std::string(scene.name) + ".actual.png")));
}

TEST_F(Machinery, FailureReportsPixelCountAndWritesArtifacts) {
    const selftest::Scene& scene = selftest::scenes()[0];
    const std::optional<gfx::Framebuffer> fb = render(scene, faces_);
    ASSERT_TRUE(fb.has_value());
    gfx::Framebuffer tampered = need(fb);
    // flip exactly 7 pixels
    for (std::int16_t i = 0; i < 7; ++i) {
        tampered.set(i,
                     0,
                     tampered.get(i, 0) == gfx::Color::kBlack ? gfx::Color::kWhite
                                                              : gfx::Color::kBlack);
    }
    ASSERT_TRUE(write_file(png_path(root_ / "golden", scene.name), need(encode(tampered))));
    const SceneCheck r = check_scene(scene, faces_, root_ / "golden", root_ / "out");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.diff_pixels, 7U);
    EXPECT_NE(r.message.find("7 pixel(s) differ"), std::string::npos) << r.message;
    const fs::path actual = root_ / "out" / (std::string(scene.name) + ".actual.png");
    const fs::path diff = root_ / "out" / (std::string(scene.name) + ".diff.png");
    ASSERT_TRUE(fs::exists(actual));
    ASSERT_TRUE(fs::exists(diff));
    const std::optional<gfx::Framebuffer> diff_fb = decode(need(read_file(diff)));
    ASSERT_TRUE(diff_fb.has_value());
    EXPECT_EQ(diff_pixels(need(diff_fb), diff_image(need(fb), tampered)), 0U);
    EXPECT_EQ(need(decode(need(read_file(actual)))).bits, need(fb).bits);
}

TEST_F(Machinery, MissingAndForeignGoldensAreReported) {
    const selftest::Scene& scene = selftest::scenes()[0];
    const SceneCheck missing = check_scene(scene, faces_, root_ / "golden", root_ / "out");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.message.find("missing"), std::string::npos);
    const Bytes junk{1, 2, 3};
    ASSERT_TRUE(write_file(png_path(root_ / "golden", scene.name), junk));
    const SceneCheck foreign = check_scene(scene, faces_, root_ / "golden", root_ / "out");
    EXPECT_FALSE(foreign.ok);
    EXPECT_NE(foreign.message.find("pixel diff unavailable"), std::string::npos) << foreign.message;
}

TEST_F(Machinery, OrphansAreFound) {
    ASSERT_TRUE(write_file(root_ / "golden" / "no_such_scene.png", Bytes{0}));
    ASSERT_TRUE(write_file(root_ / "golden" / "notes.txt", Bytes{0}));
    const std::vector<std::string> orphans = orphan_goldens(root_ / "golden");
    ASSERT_EQ(orphans.size(), 1U);
    EXPECT_EQ(orphans[0], "no_such_scene.png");
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::golden
