// SSD1681 driver against testkit::FakeEpdPanel (command-protocol model, R1 section 10).
// The sequence tables below are written out literally and cite docs/research/ssd1681.md (R1).
#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/epd_bus.hpp"
#include "qz/ssd1681/panel.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// NOLINTBEGIN(readability-function-cognitive-complexity) -- gtest macros inflate the metric
namespace qz::ssd1681 {
namespace {

using testkit::EpdLogEntry;
using testkit::EpdViolation;
using testkit::FakeEpdPanel;
using testkit::VirtualClock;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kHwReset = testkit::kEpdLogHardwareReset;
constexpr std::uint32_t kFrame = 5000;

EpdLogEntry cmd(std::uint16_t c, Bytes params = {}, std::uint32_t data_len = 0) {
    const auto n = static_cast<std::uint32_t>(params.size());
    return EpdLogEntry{c, std::move(params), data_len == 0 ? n : data_len};
}

/// RAM stream entry: bytes are not stored in the log, only their count.
EpdLogEntry stream(std::uint16_t c) {
    return EpdLogEntry{c, {}, kFrame};
}

// R1 s9.1 steps 1-5 (common start).
std::vector<EpdLogEntry> init_table() {
    return {
        cmd(kHwReset),                       // 9.1 step 1: RES# pulse
        cmd(0x12),                           // step 2: SW reset [s4]
        cmd(0x01, {0xC7, 0x00, 0x00}),       // step 3: 200 gates [s4 "send C7 00 00"]
        cmd(0x11, {0x03}),                   // data entry X inc / Y inc, AM X first [s3]
        cmd(0x44, {0x00, 0x18}),             // X window 0..24 bytes [s3]
        cmd(0x45, {0x00, 0x00, 0xC7, 0x00}), // Y window 0..199 [s3]
        cmd(0x18, {0x80}),                   // step 4: internal temperature sensor [s6]
        cmd(0x0C, {0x8B, 0x9C, 0x96, 0x0F}), // step 5: soft start [s4]
    };
}

// R1 s9.2 steps 2-6 (full refresh, after init).
std::vector<EpdLogEntry> full_table() {
    return {
        cmd(0x3C, {0x05}), // 9.2 step 2: border follows white LUT [s6]
        cmd(0x4E, {0x00}), // step 3: counters to origin [s3]
        cmd(0x4F, {0x00, 0x00}),
        stream(0x24),      // new image, 1 = white [s3]
        cmd(0x4E, {0x00}), // step 4: same image into RED [s3, ASSUMED]
        cmd(0x4F, {0x00, 0x00}),
        stream(0x26),
        cmd(0x22, {0xF7}), // step 5: display mode 1 [s5]
        cmd(0x20),         // master activation, BUSY [s4]
        cmd(0x10, {0x01}), // step 6: deep sleep mode 1, no BUSY wait [s2, s7]
    };
}

// R1 s9.3 steps 2-6 (partial update, OTP mode-2 waveform, after init).
std::vector<EpdLogEntry> partial_table() {
    return {
        cmd(0x3C, {0x80}), // 9.3 step 2: border held at VCOM [s6]
        cmd(0x4E, {0x00}), // step 3: previous image into RED [ASSUMED]
        cmd(0x4F, {0x00, 0x00}),
        stream(0x26),
        cmd(0x4E, {0x00}), // step 4: new image into BW [ASSUMED]
        cmd(0x4F, {0x00, 0x00}),
        stream(0x24),
        cmd(0x22, {0xFF}), // step 5: display mode 2 + temperature + OTP LUT [s5]
        cmd(0x20),
        cmd(0x10, {0x01}), // step 6
    };
}

std::vector<EpdLogEntry> concat(const std::vector<EpdLogEntry>& a,
                                const std::vector<EpdLogEntry>& b) {
    std::vector<EpdLogEntry> out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

/// Deterministic test image: diagonal stripes plus a few asymmetric marks.
gfx::Framebuffer pattern(std::uint32_t seed) {
    gfx::Framebuffer fb;
    for (std::int16_t y = 0; y < gfx::kHeight; ++y) {
        for (std::int16_t x = 0; x < gfx::kWidth; ++x) {
            if (((static_cast<std::uint32_t>(x) + (static_cast<std::uint32_t>(y) * 3U) + seed) %
                 7U) < 2U) {
                fb.set(x, y, gfx::Color::kBlack);
            }
        }
    }
    fb.set(0, 0, gfx::Color::kBlack);
    fb.set(199, 199, gfx::Color::kBlack);
    return fb;
}

struct Fixture : ::testing::Test {
    VirtualClock clock;
    FakeEpdPanel epd{clock};
    Panel panel{epd};

    void expect_clean() const {
        EXPECT_EQ(epd.violation_count(), 0U);
        EXPECT_EQ(epd.last_violation(), EpdViolation::kNone);
    }
    void full_update(const gfx::Framebuffer& fb) {
        ASSERT_TRUE(panel.init());
        ASSERT_TRUE(panel.update(fb, fb, UpdateMode::kFull));
    }
};

// --- sequences ----------------------------------------------------------------------------------

TEST_F(Fixture, InitMatchesTableAndWaitsForSoftReset) {
    ASSERT_TRUE(panel.init());
    EXPECT_EQ(epd.log(), init_table());
    EXPECT_FALSE(epd.busy());              // init waited for BUSY low after 0x12
    EXPECT_GE(clock.elapsed_us(), 10'000); // soft-reset BUSY consumed virtual time
    expect_clean();
}

TEST_F(Fixture, FullUpdateMatchesTable) {
    const gfx::Framebuffer fb = pattern(1);
    ASSERT_TRUE(panel.init());
    epd.clear_log();
    ASSERT_TRUE(panel.update(pattern(9), fb, UpdateMode::kFull));
    EXPECT_EQ(epd.log(), full_table());
    expect_clean();
}

TEST_F(Fixture, PartialUpdateMatchesTable) {
    const gfx::Framebuffer a = pattern(1);
    const gfx::Framebuffer b = pattern(2);
    full_update(a);
    ASSERT_TRUE(panel.init());
    epd.clear_log();
    ASSERT_TRUE(panel.update(a, b, UpdateMode::kPartial));
    EXPECT_EQ(epd.log(), partial_table());
    expect_clean();
}

TEST_F(Fixture, WholeCycleLogIsInitThenUpdate) {
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.update(pattern(0), pattern(0), UpdateMode::kFull));
    EXPECT_EQ(epd.log(), concat(init_table(), full_table()));
}

// --- images -------------------------------------------------------------------------------------

TEST_F(Fixture, FullRefreshShowsExactlyTheFrameAndLoadsBothPlanes) {
    const gfx::Framebuffer fb = pattern(3);
    full_update(fb);
    EXPECT_EQ(epd.displayed().bits, fb.bits);
    EXPECT_EQ(epd.ram_bw_image().bits, fb.bits);
    EXPECT_EQ(epd.ram_red_image().bits, fb.bits); // base for the next partial [R1 s9.2 step 4]
    EXPECT_EQ(epd.full_updates(), 1U);
    EXPECT_EQ(epd.partial_updates(), 0U);
}

TEST_F(Fixture, FullRefreshIgnoresPreviousFrameContent) {
    const gfx::Framebuffer fb = pattern(4);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.update(pattern(77), fb, UpdateMode::kFull));
    EXPECT_EQ(epd.displayed().bits, fb.bits);
}

TEST_F(Fixture, PartialUpdateShowsExactlyNext) {
    const gfx::Framebuffer a = pattern(1);
    const gfx::Framebuffer b = pattern(2);
    full_update(a);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.update(a, b, UpdateMode::kPartial));
    EXPECT_EQ(epd.displayed().bits, b.bits);
    EXPECT_EQ(epd.ram_red_image().bits, a.bits); // old image in RED, new in BW [R1 s9.3]
    EXPECT_EQ(epd.ram_bw_image().bits, b.bits);
    EXPECT_EQ(epd.partial_updates(), 1U);
    expect_clean();
}

TEST_F(Fixture, ChainOfPartialUpdatesTracksEveryFrame) {
    gfx::Framebuffer prev = pattern(0);
    full_update(prev);
    for (std::uint32_t i = 1; i <= 12; ++i) {
        const gfx::Framebuffer next = pattern(i * 5U);
        ASSERT_TRUE(panel.init()); // every wake: HW reset first, RAM contents are scrambled
        ASSERT_TRUE(panel.update(prev, next, UpdateMode::kPartial));
        ASSERT_EQ(epd.displayed().bits, next.bits) << "frame " << i;
        prev = next;
    }
    EXPECT_EQ(epd.partial_updates(), 12U);
    expect_clean();
}

TEST_F(Fixture, PartialUpdateWithWrongPreviousLeavesGhostPixels) {
    // Documents the contract: `previous` must be what the panel really shows (R1 s8).
    const gfx::Framebuffer shown = pattern(1);
    const gfx::Framebuffer lie = pattern(2);
    const gfx::Framebuffer next = pattern(3);
    full_update(shown);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.update(lie, next, UpdateMode::kPartial));
    EXPECT_NE(epd.displayed().bits, next.bits);
}

TEST_F(Fixture, UpdateWithBeginAndFinishIsSplit) {
    const gfx::Framebuffer fb = pattern(5);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.begin_update(fb, fb, UpdateMode::kFull));
    EXPECT_TRUE(epd.busy());
    EXPECT_FALSE(epd.in_deep_sleep());
    EXPECT_NE(epd.displayed().bits, fb.bits); // BUSY still high: nothing committed
    ASSERT_TRUE(panel.finish(UpdateMode::kFull));
    EXPECT_TRUE(epd.in_deep_sleep());
    EXPECT_EQ(epd.displayed().bits, fb.bits);
}

// --- polarity -----------------------------------------------------------------------------------

TEST(PolarityTest, InvertsEveryByte) {
    const std::array<std::uint8_t, 4> in{0x00, 0xFF, 0x0F, 0xA5};
    std::array<std::uint8_t, 4> out{};
    ASSERT_TRUE(to_ram_polarity(in, out));
    EXPECT_EQ(out, (std::array<std::uint8_t, 4>{0xFF, 0x00, 0xF0, 0x5A}));
}

TEST(PolarityTest, RejectsLengthMismatch) {
    const std::array<std::uint8_t, 4> in{};
    std::array<std::uint8_t, 3> out{};
    const Status s = to_ram_polarity(in, out);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Errc::kBadArgs);
}

TEST_F(Fixture, WhiteFrameIsAllOnesInRamAndBlackPixelsAreZeroBitsMsbFirst) {
    gfx::Framebuffer fb;
    fb.set(3, 0, gfx::Color::kBlack);     // byte 0, bit 4 from the MSB
    fb.set(199, 199, gfx::Color::kBlack); // last byte, LSB
    full_update(fb);
    EXPECT_EQ(epd.ram_bw()[0], 0xEF);
    EXPECT_EQ(epd.ram_bw()[1], 0xFF);
    EXPECT_EQ(epd.ram_bw()[kFrame - 1], 0xFE);
    for (std::size_t i = 0; i < kFrame; ++i) {
        EXPECT_EQ(epd.ram_bw()[i], static_cast<std::uint8_t>(~fb.bits[i])) << i;
    }
}

// --- timeouts and failure handling --------------------------------------------------------------

TEST_F(Fixture, PartialBusyTimeoutResetsPanelAndReturnsTimeout) {
    const gfx::Framebuffer a = pattern(1);
    const gfx::Framebuffer b = pattern(2);
    full_update(a);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.begin_update(a, b, UpdateMode::kPartial));
    epd.fail_next_busy_wait();
    const std::int64_t before = clock.elapsed_us();
    const Status s = panel.finish(UpdateMode::kPartial);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Errc::kTimeout);
    EXPECT_GE(clock.elapsed_us() - before, 5'000'000); // waited the 5 s partial timeout
    EXPECT_EQ(epd.hardware_resets(), 3U);              // init, init, timeout recovery
    EXPECT_EQ(epd.aborted_updates(), 1U);
    EXPECT_FALSE(epd.busy());                // reset released the stuck BUSY
    EXPECT_EQ(epd.displayed().bits, a.bits); // the update never landed
    EXPECT_EQ(epd.deep_sleep_entries(), 1U); // only the first (full) update slept
    // The driver needs init() again; the caller marks the frame invalid.
    EXPECT_EQ(panel.begin_update(a, b, UpdateMode::kPartial).error().code, Errc::kInvalidState);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.update(a, b, UpdateMode::kFull));
    EXPECT_EQ(epd.displayed().bits, b.bits);
    expect_clean();
}

TEST_F(Fixture, FullBusyTimeoutUsesTenSeconds) {
    ASSERT_TRUE(panel.init());
    const gfx::Framebuffer fb = pattern(1);
    ASSERT_TRUE(panel.begin_update(fb, fb, UpdateMode::kFull));
    epd.fail_next_busy_wait();
    const std::int64_t before = clock.elapsed_us();
    EXPECT_EQ(panel.finish(UpdateMode::kFull).error().code, Errc::kTimeout);
    EXPECT_GE(clock.elapsed_us() - before, 10'000'000);
    EXPECT_LT(clock.elapsed_us() - before, 11'000'000);
}

TEST_F(Fixture, SlowRealBusyBeyondTimeoutIsATimeout) {
    epd.set_busy_durations_us(20'000'000, 6'000'000); // cold panel: partial takes 6 s > 5 s
    const gfx::Framebuffer a = pattern(1);
    const gfx::Framebuffer b = pattern(2);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.update(a, a, UpdateMode::kFull).error().code == Errc::kTimeout);
    ASSERT_TRUE(panel.init());
    EXPECT_EQ(panel.update(a, b, UpdateMode::kPartial).error().code, Errc::kTimeout);
}

TEST_F(Fixture, SoftResetBusyTimeoutFailsInit) {
    epd.fail_next_busy_wait(); // BUSY sticks after the 0x12 of init()
    const std::int64_t before = clock.elapsed_us();
    const Status s = panel.init();
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Errc::kTimeout);
    EXPECT_GE(clock.elapsed_us() - before, 200'000);
    EXPECT_EQ(panel.begin_update(pattern(0), pattern(0), UpdateMode::kFull).error().code,
              Errc::kInvalidState);
    ASSERT_TRUE(panel.init()); // recovers
}

TEST_F(Fixture, ColdPanelRefusalDoesNotHangTheDriver) {
    epd.set_temperature_dc(-100); // -10 C: no OTP range matches [R1 s6]
    const gfx::Framebuffer fb = pattern(1);
    ASSERT_TRUE(panel.init());
    EXPECT_TRUE(panel.update(fb, fb, UpdateMode::kFull)); // BUSY fell, nothing displayed
    EXPECT_EQ(epd.refused_updates(), 1U);
    EXPECT_NE(epd.displayed().bits, fb.bits);
}

// --- bus errors and state machine ---------------------------------------------------------------

/// Delegates to a FakeEpdPanel; fails command() after `budget` successful calls.
class FlakyBus final : public hal::EpdBus {
public:
    explicit FlakyBus(FakeEpdPanel& inner) : inner_(inner) {}
    int budget = 1'000'000;
    bool read_unsupported = false;
    Status hardware_reset() override { return inner_.hardware_reset(); }
    Status command(std::uint8_t c) override {
        if (budget-- <= 0) {
            return Error{Errc::kIo, 7};
        }
        return inner_.command(c);
    }
    Status data(std::span<const std::uint8_t> b) override { return inner_.data(b); }
    Status read(std::span<std::uint8_t> out) override {
        return read_unsupported ? Status{Errc::kUnsupported} : inner_.read(out);
    }
    [[nodiscard]] bool busy() const override { return inner_.busy(); }
    Status wait_idle(std::uint32_t t) override { return inner_.wait_idle(t); }

private:
    FakeEpdPanel& inner_;
};

TEST_F(Fixture, BusErrorMidUpdateIsReportedAndForcesReinit) {
    FlakyBus bus(epd);
    Panel p(bus);
    ASSERT_TRUE(p.init());
    bus.budget = 3; // dies inside the RAM writes
    const gfx::Framebuffer fb = pattern(1);
    const Status s = p.begin_update(fb, fb, UpdateMode::kFull);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Errc::kIo);
    EXPECT_EQ(s.error().detail, 7);
    EXPECT_EQ(p.begin_update(fb, fb, UpdateMode::kFull).error().code, Errc::kInvalidState);
    bus.budget = 1'000'000;
    ASSERT_TRUE(p.init()); // the abandoned half-written command stream is cleared by the reset
    ASSERT_TRUE(p.update(fb, fb, UpdateMode::kFull));
    EXPECT_EQ(epd.displayed().bits, fb.bits);
}

TEST_F(Fixture, CallsOutOfOrderAreInvalidState) {
    const gfx::Framebuffer fb = pattern(1);
    EXPECT_EQ(panel.begin_update(fb, fb, UpdateMode::kFull).error().code, Errc::kInvalidState);
    EXPECT_EQ(panel.finish(UpdateMode::kFull).error().code, Errc::kInvalidState);
    EXPECT_EQ(panel.sleep().error().code, Errc::kInvalidState); // never initialised
    EXPECT_EQ(panel.temperature_dc().error().code, Errc::kInvalidState);
    EXPECT_EQ(panel.probe_mode2_waveform().error().code, Errc::kInvalidState);
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.begin_update(fb, fb, UpdateMode::kFull));
    EXPECT_EQ(panel.begin_update(fb, fb, UpdateMode::kFull).error().code, Errc::kInvalidState);
    EXPECT_EQ(panel.finish(UpdateMode::kPartial).error().code, Errc::kInvalidState); // wrong mode
    EXPECT_EQ(panel.sleep().error().code, Errc::kInvalidState);                      // use finish()
    EXPECT_EQ(panel.init().error().code, Errc::kInvalidState); // never interrupt an update
    ASSERT_TRUE(panel.finish(UpdateMode::kFull));
    EXPECT_EQ(epd.displayed().bits, fb.bits);
    expect_clean();
}

// --- sleep / wake -------------------------------------------------------------------------------

TEST_F(Fixture, UpdateEndsInDeepSleepAndOnlyInitWakesIt) {
    const gfx::Framebuffer a = pattern(1);
    const gfx::Framebuffer b = pattern(2);
    full_update(a);
    EXPECT_TRUE(epd.in_deep_sleep());
    EXPECT_TRUE(epd.busy()); // BUSY stays high after 0x10 [R1 s2]
    EXPECT_EQ(panel.begin_update(a, b, UpdateMode::kPartial).error().code, Errc::kInvalidState);
    ASSERT_TRUE(panel.init());
    EXPECT_FALSE(epd.in_deep_sleep());
    ASSERT_TRUE(panel.update(a, b, UpdateMode::kPartial));
    EXPECT_TRUE(epd.in_deep_sleep());
    EXPECT_EQ(epd.deep_sleep_entries(), 2U);
    EXPECT_EQ(epd.hardware_resets(), 2U);
    expect_clean(); // no bus traffic and no BUSY wait ever reached the sleeping chip
}

TEST_F(Fixture, StandaloneSleepIsIdempotentAndHardwareResetWakes) {
    ASSERT_TRUE(panel.init());
    ASSERT_TRUE(panel.sleep());
    EXPECT_TRUE(epd.in_deep_sleep());
    ASSERT_TRUE(panel.sleep()); // already asleep: no traffic
    EXPECT_EQ(epd.deep_sleep_entries(), 1U);
    ASSERT_TRUE(panel.init());
    EXPECT_FALSE(epd.in_deep_sleep());
    expect_clean();
}

TEST_F(Fixture, RamIsRewrittenAfterWakeBecauseRetentionIsNotRelied) {
    const gfx::Framebuffer a = pattern(1);
    full_update(a);
    ASSERT_TRUE(panel.init()); // RES# scrambles RAM in the model [R1 s10 rule 3]
    EXPECT_NE(epd.ram_bw_image().bits, a.bits);
    ASSERT_TRUE(panel.update(a, a, UpdateMode::kPartial)); // still correct: driver rewrites both
    EXPECT_EQ(epd.displayed().bits, a.bits);
}

// --- temperature and OTP probe ------------------------------------------------------------------

TEST_F(Fixture, TemperatureLoadsSensorThenReadsRegister) {
    ASSERT_TRUE(panel.init());
    epd.clear_log();
    epd.set_temperature_dc(238); // 1/16 C register resolution: exact for 238
    const Result<std::int16_t> t = panel.temperature_dc();
    ASSERT_TRUE(t);
    EXPECT_EQ(*t, 238);
    // Sensor load sequence B1 [R1 s5], then the 0x1B read [R1 s4].
    EXPECT_EQ(epd.log(), (std::vector<EpdLogEntry>{cmd(0x22, {0xB1}), cmd(0x20), cmd(0x1B)}));
    epd.set_temperature_dc(-55);
    EXPECT_EQ(*panel.temperature_dc(), -55);
    epd.set_temperature_dc(480);
    EXPECT_EQ(*panel.temperature_dc(), 480);
    epd.set_temperature_dc(237); // not representable: within one register step (0.6 dC)
    EXPECT_NEAR(*panel.temperature_dc(), 237, 1);
    expect_clean();
}

TEST_F(Fixture, TemperatureWithoutReadSupportKeepsPanelUsable) {
    FlakyBus bus(epd);
    bus.read_unsupported = true;
    Panel p(bus);
    ASSERT_TRUE(p.init());
    EXPECT_EQ(p.temperature_dc().error().code, Errc::kUnsupported);
    EXPECT_EQ(p.probe_mode2_waveform().error().code, Errc::kUnsupported);
    const gfx::Framebuffer fb = pattern(1);
    ASSERT_TRUE(p.update(fb, fb, UpdateMode::kFull)); // still READY
    EXPECT_EQ(epd.displayed().bits, fb.bits);
}

TEST_F(Fixture, ProbeMode2ReadsOtpBytesCToG) {
    ASSERT_TRUE(panel.init());
    EXPECT_TRUE(*panel.probe_mode2_waveform()); // fake default: mode-2 bits present
    epd.set_otp_display_option(
        {0xFF, 0xFF, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}); // only A,B,H..K
    EXPECT_FALSE(*panel.probe_mode2_waveform());
    epd.set_otp_display_option({0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0}); // byte G
    EXPECT_TRUE(*panel.probe_mode2_waveform());
    epd.set_otp_display_option({0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0}); // byte C
    EXPECT_TRUE(*panel.probe_mode2_waveform());
    expect_clean();
}

// --- custom waveform hook (Q-12) ----------------------------------------------------------------

struct HookFixture : ::testing::Test {
    VirtualClock clock;
    FakeEpdPanel epd{clock};
    std::array<std::uint8_t, kLutBytes> lut{};
    Waveform wf;
    HookFixture() {
        for (std::size_t i = 0; i < lut.size(); ++i) {
            lut[i] = static_cast<std::uint8_t>(i ^ 0x5A); // TEST PATTERN, not a real waveform
        }
        wf.lut = lut;
        wf.eopt = 0x22;
        wf.vgh = 0x17;
        wf.vsh_vsl = {0x41, 0xA8, 0x32};
        wf.vcom = 0x3C;
    }
};

TEST_F(HookFixture, PartialWithWaveformWritesLutAndUsesCf) {
    Panel p(epd, {}, &wf);
    const gfx::Framebuffer a = pattern(1);
    const gfx::Framebuffer b = pattern(2);
    ASSERT_TRUE(p.init());
    ASSERT_TRUE(p.update(a, a, UpdateMode::kFull));
    ASSERT_TRUE(p.init());
    epd.clear_log();
    ASSERT_TRUE(p.update(a, b, UpdateMode::kPartial));
    const Bytes lut_bytes(lut.begin(), lut.end());
    // R1 s9.3 fallback: 32 + 153 bytes, 3F, 03, 04, 2C, then 22 = CF.
    const std::vector<EpdLogEntry> expect{
        cmd(0x3C, {0x80}),
        cmd(0x32, lut_bytes),
        cmd(0x3F, {0x22}),
        cmd(0x03, {0x17}),
        cmd(0x04, {0x41, 0xA8, 0x32}),
        cmd(0x2C, {0x3C}),
        cmd(0x4E, {0x00}),
        cmd(0x4F, {0x00, 0x00}),
        stream(0x26),
        cmd(0x4E, {0x00}),
        cmd(0x4F, {0x00, 0x00}),
        stream(0x24),
        cmd(0x22, {0xCF}),
        cmd(0x20),
        cmd(0x10, {0x01}),
    };
    EXPECT_EQ(epd.log(), expect);
    EXPECT_EQ(epd.displayed().bits, b.bits);
    EXPECT_EQ(epd.violation_count(), 0U);
}

TEST_F(HookFixture, FullUpdateNeverTouchesTheWaveform) {
    Panel p(epd, {}, &wf);
    ASSERT_TRUE(p.init());
    epd.clear_log();
    const gfx::Framebuffer fb = pattern(1);
    ASSERT_TRUE(p.update(fb, fb, UpdateMode::kFull));
    for (const EpdLogEntry& e : epd.log()) {
        EXPECT_NE(e.cmd, 0x32);
    }
}

TEST_F(HookFixture, WrongLutSizeIsBadArgsAndSendsNothing) {
    wf.lut = std::span<const std::uint8_t>(lut).first(100);
    Panel p(epd, {}, &wf);
    ASSERT_TRUE(p.init());
    epd.clear_log();
    const gfx::Framebuffer fb = pattern(1);
    EXPECT_EQ(p.begin_update(fb, fb, UpdateMode::kPartial).error().code, Errc::kBadArgs);
    EXPECT_TRUE(epd.log().empty());
    EXPECT_TRUE(p.update(fb, fb, UpdateMode::kFull)); // the panel is still READY
}

TEST_F(Fixture, DefaultPanelHasNoWaveformHookActive) {
    const gfx::Framebuffer fb = pattern(1);
    full_update(fb);
    ASSERT_TRUE(panel.init());
    epd.clear_log();
    ASSERT_TRUE(panel.update(fb, pattern(2), UpdateMode::kPartial));
    for (const EpdLogEntry& e : epd.log()) {
        EXPECT_NE(e.cmd, 0x32);
        EXPECT_NE(e.cmd, 0x2C);
    }
}

} // namespace
} // namespace qz::ssd1681
// NOLINTEND(readability-function-cognitive-complexity)
