// FakeEpdPanel protocol model: every rule of docs/research/ssd1681.md section 10 (R1) that the
// model enforces must fail a test here, so that a driver bug cannot hide behind a lenient fake.
// Lives in qz_ssd1681/test because that WP owns it; the fake itself is in qz_testkit.
#include "qz/gfx/framebuffer.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

// NOLINTBEGIN(readability-function-cognitive-complexity) -- gtest macros inflate the metric
namespace qz::testkit {
namespace {

using Bytes = std::vector<std::uint8_t>;

struct EpdTest : ::testing::Test {
    VirtualClock clock;
    FakeEpdPanel epd{clock};

    /// command + parameter bytes in one go.
    Status send(std::uint8_t c, const Bytes& params = {}) {
        if (const Status s = epd.command(c); !s) {
            return s;
        }
        return params.empty() ? ok() : epd.data(params);
    }
    void expect_violation(const Status& s, EpdViolation v) const {
        ASSERT_FALSE(s);
        EXPECT_EQ(s.error().code, Errc::kInvalidState);
        EXPECT_EQ(s.error().detail, static_cast<std::uint16_t>(v));
        EXPECT_EQ(epd.last_violation(), v);
    }
    /// HW reset, SW reset + wait, 200 x 200 window, internal sensor (R1 s9.1).
    void bring_up() {
        ASSERT_TRUE(epd.hardware_reset());
        ASSERT_TRUE(send(0x12));
        ASSERT_TRUE(epd.wait_idle(200));
        ASSERT_TRUE(send(0x01, {0xC7, 0x00, 0x00}));
        ASSERT_TRUE(send(0x11, {0x03}));
        ASSERT_TRUE(send(0x44, {0x00, 0x18}));
        ASSERT_TRUE(send(0x45, {0x00, 0x00, 0xC7, 0x00}));
        ASSERT_TRUE(send(0x18, {0x80}));
    }
    /// Writes `fb` (gfx polarity) to RAM `plane_cmd` with the counters set.
    void write_plane(std::uint8_t plane_cmd, const gfx::Framebuffer& fb) {
        ASSERT_TRUE(send(0x4E, {0x00}));
        ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
        ASSERT_TRUE(epd.command(plane_cmd));
        Bytes inv(fb.bits.begin(), fb.bits.end());
        for (std::uint8_t& b : inv) {
            b = static_cast<std::uint8_t>(~b);
        }
        ASSERT_TRUE(epd.data(inv));
    }
    void run(std::uint8_t ctrl2) {
        ASSERT_TRUE(send(0x22, {ctrl2}));
        ASSERT_TRUE(send(0x20));
    }
};

gfx::Framebuffer box(std::int16_t x, std::int16_t y, std::int16_t w, std::int16_t h) {
    gfx::Framebuffer fb;
    for (std::int16_t j = y; j < y + h; ++j) {
        for (std::int16_t i = x; i < x + w; ++i) {
            fb.set(i, j, gfx::Color::kBlack);
        }
    }
    return fb;
}

// --- command set and parameter validation -------------------------------------------------------

TEST_F(EpdTest, UnknownAndOtpProgrammingCommandsAreRejected) {
    bring_up();
    expect_violation(epd.command(0x55), EpdViolation::kUnknownCommand);
    for (const std::uint8_t otp :
         std::array<std::uint8_t, 7>{0x08, 0x09, 0x0A, 0x2A, 0x30, 0x36, 0x39}) {
        expect_violation(epd.command(otp), EpdViolation::kOtpProgramCommand);
    }
    EXPECT_EQ(epd.violation_count(), 8U);
    EXPECT_TRUE(send(0x11, {0x03})); // the model is still usable
}

TEST_F(EpdTest, ParameterCountsAreEnforced) {
    bring_up();
    ASSERT_TRUE(epd.command(0x11));
    expect_violation(epd.data(Bytes{0x03, 0x00}), EpdViolation::kWrongParamCount); // too many
    expect_violation(epd.command(0x12), EpdViolation::kWrongParamCount); // 0x11 got 0 params
    EXPECT_TRUE(send(0x12));                                             // command cleared
    ASSERT_TRUE(epd.wait_idle(200));
    ASSERT_TRUE(epd.command(0x44));
    ASSERT_TRUE(epd.data(Bytes{0x00}));
    expect_violation(epd.command(0x45), EpdViolation::kWrongParamCount); // 0x44 got 1 of 2
    expect_violation(epd.data(Bytes{0x01}), EpdViolation::kDataWithoutCommand);
}

TEST_F(EpdTest, ParameterCountTable) {
    // R1 s10 rule 5: command -> parameter count; sending count+1 bytes must fail.
    const std::vector<std::pair<std::uint8_t, std::size_t>> table{
        {0x01, 3}, {0x03, 1}, {0x04, 3}, {0x0C, 4},   {0x10, 1}, {0x11, 1}, {0x18, 1},
        {0x1A, 2}, {0x21, 2}, {0x22, 1}, {0x32, 153}, {0x3C, 1}, {0x3F, 1}, {0x44, 2},
        {0x45, 4}, {0x46, 1}, {0x47, 1}, {0x4E, 1},   {0x4F, 2}, {0x2C, 1},
    };
    for (const auto& [c, n] : table) {
        ASSERT_TRUE(epd.hardware_reset());
        ASSERT_TRUE(epd.command(c));
        const auto s = epd.data(Bytes(n + 1, 0));
        ASSERT_FALSE(s) << "cmd " << static_cast<int>(c);
        EXPECT_EQ(epd.last_violation(), EpdViolation::kWrongParamCount);
    }
}

TEST_F(EpdTest, ParameterRangesAndReservedBits) {
    ASSERT_TRUE(epd.hardware_reset());
    expect_violation(send(0x01, {0xC8, 0x00, 0x00}), EpdViolation::kBadParameter); // 201 gates
    expect_violation(send(0x01, {0xC7, 0x01, 0x00}), EpdViolation::kBadParameter);
    expect_violation(send(0x01, {0xC7, 0x00, 0x08}), EpdViolation::kBadParameter);
    expect_violation(send(0x03, {0x20}), EpdViolation::kBadParameter);
    expect_violation(send(0x11, {0x08}), EpdViolation::kBadParameter);
    expect_violation(send(0x3C, {0x08}), EpdViolation::kBadParameter);
    expect_violation(send(0x44, {0x00, 0x19}), EpdViolation::kBadParameter);
    expect_violation(send(0x45, {0x00, 0x00, 0xC8, 0x00}), EpdViolation::kBadParameter);
    expect_violation(send(0x45, {0x00, 0x02, 0xC7, 0x00}), EpdViolation::kBadParameter);
    expect_violation(send(0x4E, {0x19}), EpdViolation::kBadParameter);
    expect_violation(send(0x4F, {0xC8, 0x00}), EpdViolation::kBadParameter);
    expect_violation(send(0x10, {0x02}), EpdViolation::kBadParameter);
    expect_violation(send(0x21, {0x00, 0x80}), EpdViolation::kBadParameter); // not modelled
    expect_violation(send(0x46, {0x00}), EpdViolation::kBadParameter);
    EXPECT_FALSE(epd.in_deep_sleep());
    EXPECT_TRUE(send(0x3C, {0x05}));
    EXPECT_TRUE(send(0x3C, {0x80}));
    EXPECT_TRUE(send(0x3C, {0xC0}));
}

TEST_F(EpdTest, ExternalTemperatureSensorIsRejectedOnThisBoard) {
    ASSERT_TRUE(epd.hardware_reset());
    expect_violation(send(0x18, {0x48}), EpdViolation::kExternalSensor);
    expect_violation(send(0x18, {0x12}), EpdViolation::kBadParameter);
    EXPECT_TRUE(send(0x18, {0x80}));
}

TEST_F(EpdTest, LogRecordsAcceptedCommandsAndHardwareResets) {
    ASSERT_TRUE(epd.hardware_reset());
    ASSERT_TRUE(send(0x11, {0x03}));
    EXPECT_FALSE(epd.command(0x55));
    Bytes lut(153);
    for (std::size_t i = 0; i < lut.size(); ++i) {
        lut[i] = static_cast<std::uint8_t>(i);
    }
    ASSERT_TRUE(send(0x32, lut));
    const std::vector<EpdLogEntry> expect{
        {kEpdLogHardwareReset, {}, 0}, {0x11, {0x03}, 1}, {0x32, lut, 153}};
    EXPECT_EQ(epd.log(), expect);
    epd.clear_log();
    EXPECT_TRUE(epd.log().empty());
}

// --- BUSY ---------------------------------------------------------------------------------------

TEST_F(EpdTest, BusyAfterSoftResetBlocksCommandsDataAndReads) {
    ASSERT_TRUE(epd.hardware_reset());
    ASSERT_TRUE(send(0x12));
    EXPECT_TRUE(epd.busy());
    expect_violation(epd.command(0x11), EpdViolation::kWhileBusy);
    expect_violation(epd.data(Bytes{1}), EpdViolation::kWhileBusy);
    clock.advance_us(10'000);
    EXPECT_FALSE(epd.busy());
    EXPECT_TRUE(send(0x11, {0x03}));
}

TEST_F(EpdTest, WaitIdleAdvancesVirtualTimeAndHonoursTimeout) {
    ASSERT_TRUE(epd.hardware_reset());
    ASSERT_TRUE(send(0x12));
    const std::int64_t t0 = clock.elapsed_us();
    const Status s = epd.wait_idle(1); // 1 ms < 10 ms busy
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Errc::kTimeout);
    EXPECT_EQ(clock.elapsed_us() - t0, 1000);
    EXPECT_TRUE(epd.wait_idle(100));
    EXPECT_EQ(clock.elapsed_us() - t0, 10'000);
    EXPECT_TRUE(epd.wait_idle(100)); // already idle: no time passes
    EXPECT_EQ(clock.elapsed_us() - t0, 10'000);
}

TEST_F(EpdTest, StatusReadWorksWhileBusyAndReportsBusyFlagAndChipId) {
    ASSERT_TRUE(epd.hardware_reset());
    ASSERT_TRUE(send(0x12));
    std::array<std::uint8_t, 1> st{};
    ASSERT_TRUE(epd.command(0x2F));
    ASSERT_TRUE(epd.read(st));
    EXPECT_EQ(st[0], 0x05); // busy flag A[2] | chip id 01
    clock.advance_us(10'000);
    ASSERT_TRUE(epd.command(0x2F));
    ASSERT_TRUE(epd.read(st));
    EXPECT_EQ(st[0], 0x01);
    EXPECT_EQ(epd.violation_count(), 0U);
}

TEST_F(EpdTest, UpdateTimingIsFullTwoSecondsPartialQuarterSecond) {
    bring_up();
    write_plane(0x24, box(0, 0, 10, 10));
    write_plane(0x26, box(0, 0, 10, 10));
    run(0xF7);
    EXPECT_TRUE(epd.busy());
    clock.advance_us(1'999'999);
    EXPECT_TRUE(epd.busy());
    EXPECT_EQ(epd.full_updates(), 0U); // not committed before BUSY falls
    clock.advance_us(1);
    EXPECT_FALSE(epd.busy());
    EXPECT_EQ(epd.full_updates(), 1U);
    EXPECT_EQ(epd.displayed().bits, box(0, 0, 10, 10).bits);
    ASSERT_TRUE(epd.hardware_reset());
    bring_up();
    write_plane(0x26, box(0, 0, 10, 10));
    write_plane(0x24, box(20, 0, 10, 10));
    run(0xFF);
    clock.advance_us(259'999);
    EXPECT_TRUE(epd.busy());
    clock.advance_us(1);
    EXPECT_FALSE(epd.busy());
    EXPECT_EQ(epd.partial_updates(), 1U);
}

TEST_F(EpdTest, StuckBusyInjectionAppliesToRunningAndNextBusyPeriods) {
    bring_up();
    write_plane(0x24, box(0, 0, 4, 4));
    write_plane(0x26, box(0, 0, 4, 4));
    run(0xF7);
    epd.fail_next_busy_wait(); // running busy period sticks
    const Status s = epd.wait_idle(60'000);
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code, Errc::kTimeout);
    EXPECT_TRUE(epd.busy());
    ASSERT_TRUE(epd.hardware_reset()); // releases it, counts an aborted update
    EXPECT_EQ(epd.aborted_updates(), 1U);
    EXPECT_FALSE(epd.busy());
    EXPECT_EQ(epd.full_updates(), 0U);
    // Not busy now: the injection waits for the next busy period.
    epd.fail_next_busy_wait();
    ASSERT_TRUE(send(0x12));
    EXPECT_EQ(epd.wait_idle(1000).error().code, Errc::kTimeout);
}

// --- RAM, windows, counters ---------------------------------------------------------------------

TEST_F(EpdTest, RamWriteNeedsCountersSetFirst) {
    bring_up();
    ASSERT_TRUE(epd.command(0x24));
    expect_violation(epd.data(Bytes(10, 0)), EpdViolation::kCounterNotSet);
    // The stream never started, so the next command reports it incomplete and clears it.
    expect_violation(epd.command(0x4E), EpdViolation::kRamWriteIncomplete);
    ASSERT_TRUE(send(0x4E, {0x00})); // only X set: still not enough
    ASSERT_TRUE(epd.command(0x24));
    expect_violation(epd.data(Bytes(10, 0)), EpdViolation::kCounterNotSet);
}

TEST_F(EpdTest, RamWriteNeedsFreshCountersAfterEachStreamAndWindowChange) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    ASSERT_TRUE(epd.command(0x26));
    expect_violation(epd.data(Bytes(5000, 0)), EpdViolation::kCounterNotSet);
    expect_violation(epd.command(0x7F), EpdViolation::kRamWriteIncomplete); // clears the stream
    ASSERT_TRUE(send(0x4E, {0x00}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(send(0x44, {0x00, 0x18})); // window write invalidates the counters
    ASSERT_TRUE(epd.command(0x24));
    expect_violation(epd.data(Bytes(5000, 0)), EpdViolation::kCounterNotSet);
}

TEST_F(EpdTest, RamWriteOverflowAndUnderflowAreRejected) {
    bring_up();
    ASSERT_TRUE(send(0x4E, {0x00}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x24));
    ASSERT_TRUE(epd.data(Bytes(5000, 0xFF)));
    expect_violation(epd.data(Bytes{0x00}), EpdViolation::kRamWriteOverflow);
    ASSERT_TRUE(send(0x4E, {0x00})); // next command ends the (complete) stream fine
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x26));
    ASSERT_TRUE(epd.data(Bytes(4999, 0xFF)));
    expect_violation(epd.command(0x22), EpdViolation::kRamWriteIncomplete);
    ASSERT_TRUE(epd.command(0x26)); // a stream command with no data at all is incomplete too
    expect_violation(epd.command(0x7F), EpdViolation::kRamWriteIncomplete);
}

TEST_F(EpdTest, StreamsMayArriveInSeveralChunksAndNopEndsAStream) {
    bring_up();
    ASSERT_TRUE(send(0x4E, {0x00}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x24));
    for (int i = 0; i < 20; ++i) {
        Bytes chunk(250, static_cast<std::uint8_t>(i));
        ASSERT_TRUE(epd.data(chunk));
    }
    ASSERT_TRUE(epd.command(0x7F));
    EXPECT_EQ(epd.ram_bw()[0], 0);
    EXPECT_EQ(epd.ram_bw()[250], 1);
    EXPECT_EQ(epd.ram_bw()[4999], 19);
    EXPECT_EQ(epd.log().back().cmd, 0x7F);
    EXPECT_EQ(epd.violation_count(), 0U);
}

TEST_F(EpdTest, CounterMustStartAtTheWindowCornerAndWindowMustMatchEntryMode) {
    bring_up();
    ASSERT_TRUE(send(0x4E, {0x01}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x24));
    expect_violation(epd.data(Bytes{0}), EpdViolation::kCounterNotAtWindowStart);
    // Window from the POR (never set) is off-RAM: 0x45 POR Y end = 0x127.
    ASSERT_TRUE(epd.hardware_reset());
    ASSERT_TRUE(send(0x4E, {0x00}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x24));
    expect_violation(epd.data(Bytes{0}), EpdViolation::kBadParameter);
    // Decrementing entry mode needs start > end.
    ASSERT_TRUE(epd.hardware_reset());
    ASSERT_TRUE(send(0x11, {0x00}));
    ASSERT_TRUE(send(0x44, {0x00, 0x18}));
    ASSERT_TRUE(send(0x45, {0x00, 0x00, 0xC7, 0x00}));
    ASSERT_TRUE(send(0x4E, {0x00}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x24));
    expect_violation(epd.data(Bytes{0}), EpdViolation::kBadParameter);
}

TEST_F(EpdTest, AddressCountersFollowDataEntryMode) {
    // 2 x 2 byte window at X 2..3, Y 0..1; bytes 1,2,3,4. R1 s3 (0x11 ID/AM).
    struct Case {
        std::uint8_t mode;
        std::uint8_t xs, xe, ys, ye;
        std::array<std::size_t, 4> where; // RAM index of bytes 1..4
    };
    const std::array<Case, 4> cases{{
        {0x03, 2, 3, 0, 1, {2, 3, 27, 28}}, // X inc, Y inc, X first
        {0x07, 2, 3, 0, 1, {2, 27, 3, 28}}, // AM = 1: Y first
        {0x00, 3, 2, 1, 0, {28, 27, 3, 2}}, // X dec, Y dec
        {0x01, 2, 3, 1, 0, {27, 28, 2, 3}}, // X inc, Y dec
    }};
    for (const Case& c : cases) {
        ASSERT_TRUE(epd.hardware_reset());
        ASSERT_TRUE(send(0x11, {c.mode}));
        ASSERT_TRUE(send(0x44, {c.xs, c.xe}));
        ASSERT_TRUE(send(0x45, {c.ys, 0x00, c.ye, 0x00}));
        ASSERT_TRUE(send(0x4E, {c.xs}));
        ASSERT_TRUE(send(0x4F, {c.ys, 0x00}));
        ASSERT_TRUE(epd.command(0x26));
        ASSERT_TRUE(epd.data(Bytes{1, 2, 3, 4})) << "mode " << static_cast<int>(c.mode);
        for (std::size_t i = 0; i < 4; ++i) {
            EXPECT_EQ(epd.ram_red()[c.where[i]], i + 1) << "mode " << static_cast<int>(c.mode);
        }
    }
}

TEST_F(EpdTest, AutoFillWritesWhitePlaneAndRaisesBusy) {
    bring_up();
    ASSERT_TRUE(send(0x47, {0xF7}));
    EXPECT_TRUE(epd.busy());
    EXPECT_TRUE(epd.wait_idle(100));
    EXPECT_TRUE(std::ranges::all_of(epd.ram_bw(), [](std::uint8_t b) { return b == 0xFF; }));
    ASSERT_TRUE(send(0x46, {0xF7}));
    EXPECT_TRUE(epd.wait_idle(100));
    EXPECT_TRUE(std::ranges::all_of(epd.ram_red(), [](std::uint8_t b) { return b == 0xFF; }));
}

// --- master activation rules --------------------------------------------------------------------

TEST_F(EpdTest, ActivationNeedsAFreshUpdateControlValue) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    expect_violation(epd.command(0x20), EpdViolation::kNoUpdateControl); // POR FF is not used
    run(0xF7);
    ASSERT_TRUE(epd.wait_idle(5000));
    expect_violation(epd.command(0x20), EpdViolation::kNoUpdateControl); // value is consumed
}

TEST_F(EpdTest, OnlyTheTwelveDocumentedUpdateValuesAreAccepted) {
    ASSERT_TRUE(epd.hardware_reset());
    for (const std::uint8_t ok_value : std::array<std::uint8_t, 12>{
             0xF7, 0xFF, 0xC7, 0xCF, 0xB1, 0xB9, 0x91, 0x99, 0x80, 0xC0, 0x01, 0x03}) {
        EXPECT_TRUE(send(0x22, {ok_value}));
    }
    for (const std::uint8_t bad : std::array<std::uint8_t, 4>{0x00, 0x55, 0xF6, 0xFE}) {
        expect_violation(send(0x22, {bad}), EpdViolation::kUndocumentedUpdateValue);
    }
}

TEST_F(EpdTest, TemperatureLoadingSequenceNeedsTheInternalSensor) {
    ASSERT_TRUE(epd.hardware_reset()); // window set up, but 0x18 left at POR (external)
    ASSERT_TRUE(send(0x44, {0x00, 0x18}));
    ASSERT_TRUE(send(0x45, {0x00, 0x00, 0xC7, 0x00}));
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    ASSERT_TRUE(send(0x22, {0xF7}));
    expect_violation(epd.command(0x20), EpdViolation::kExternalSensor); // 0x18 at POR (48)
}

TEST_F(EpdTest, DisplayNeedsTheRamPlanesWritten) {
    bring_up();
    ASSERT_TRUE(send(0x22, {0xF7}));
    expect_violation(epd.command(0x20), EpdViolation::kRamNotWritten);
    write_plane(0x24, box(0, 0, 8, 8)); // full refresh needs BW only [R1 s5]
    ASSERT_TRUE(send(0x22, {0xF7}));
    EXPECT_TRUE(epd.command(0x20));
    ASSERT_TRUE(epd.wait_idle(5000));
    ASSERT_TRUE(send(0x22, {0xFF})); // display mode 2 needs RED too [R1 s10 rule 8]
    expect_violation(epd.command(0x20), EpdViolation::kRamNotWritten);
    write_plane(0x26, box(0, 0, 8, 8));
    ASSERT_TRUE(send(0x22, {0xFF}));
    EXPECT_TRUE(epd.command(0x20));
}

TEST_F(EpdTest, NoTemperatureLoadSequencesNeedTemperatureAndLutFirst) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    ASSERT_TRUE(send(0x22, {0xC7}));
    expect_violation(epd.command(0x20), EpdViolation::kNoTemperatureSource); // register at 127.9 C
    ASSERT_TRUE(send(0x22, {0x91}));
    expect_violation(epd.command(0x20), EpdViolation::kNoTemperatureSource);
    ASSERT_TRUE(send(0x1A, {0x17, 0x00})); // manual 23 C
    ASSERT_TRUE(send(0x22, {0xC7}));
    expect_violation(epd.command(0x20), EpdViolation::kNoLut); // LUT never loaded
    run(0x91);                                                 // load LUT only
    ASSERT_TRUE(epd.wait_idle(500));
    run(0xC7); // now legal
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.full_updates(), 1U);
}

TEST_F(EpdTest, PreloadThenCf) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    run(0xB9); // load temperature + LUT, no display [R1 s5]
    ASSERT_TRUE(epd.wait_idle(500));
    EXPECT_EQ(epd.temperature_register(), 0x170); // 23.0 C = 368/16
    run(0xCF);
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.partial_updates(), 1U);
    EXPECT_EQ(epd.violation_count(), 0U);
}

TEST_F(EpdTest, CustomLutAllowsCfWithoutTemperature) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    ASSERT_TRUE(send(0x32, Bytes(153, 0x11)));
    run(0xCF);
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.partial_updates(), 1U);
    EXPECT_EQ(epd.violation_count(), 0U);
}

TEST_F(EpdTest, UpdateOutsideOperatingRangeIsRefusedWithoutHanging) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    epd.set_temperature_dc(-30);
    run(0xF7);
    EXPECT_TRUE(epd.wait_idle(100));
    EXPECT_EQ(epd.refused_updates(), 1U);
    EXPECT_EQ(epd.full_updates(), 0U);
    EXPECT_EQ(epd.displayed().bits, gfx::Framebuffer{}.bits);
    epd.set_temperature_dc(510); // 51 C
    run(0xF7);
    EXPECT_TRUE(epd.wait_idle(100));
    EXPECT_EQ(epd.refused_updates(), 2U);
    epd.set_temperature_dc(0); // boundary: 0 C is in range [R1 s1]
    run(0xF7);
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.full_updates(), 1U);
}

// --- image model --------------------------------------------------------------------------------

TEST_F(EpdTest, FullUpdateShowsBwRamExactlyIgnoringRed) {
    bring_up();
    const gfx::Framebuffer img = box(10, 20, 30, 40);
    write_plane(0x24, img);
    write_plane(0x26, box(100, 100, 5, 5)); // ignored for B/W in mode 1 [R1 s3]
    run(0xF7);
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.displayed().bits, img.bits);
}

TEST_F(EpdTest, PartialUpdateDrivesOnlyPixelsWhereOldDiffersFromNew) {
    bring_up();
    const gfx::Framebuffer shown = box(0, 0, 100, 100);
    write_plane(0x24, shown);
    write_plane(0x26, shown);
    run(0xF7);
    ASSERT_TRUE(epd.wait_idle(5000));
    // old (RED) = shown, new (BW) = another box: changed pixels follow `new`.
    const gfx::Framebuffer next = box(50, 50, 100, 100);
    ASSERT_TRUE(epd.hardware_reset());
    bring_up();
    write_plane(0x26, shown);
    write_plane(0x24, next);
    run(0xFF);
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.displayed().bits, next.bits);
    // E2 control: both planes equal (nothing changed) must leave the panel alone.
    ASSERT_TRUE(epd.hardware_reset());
    bring_up();
    write_plane(0x26, box(150, 150, 20, 20));
    write_plane(0x24, box(150, 150, 20, 20));
    run(0xFF);
    ASSERT_TRUE(epd.wait_idle(5000));
    EXPECT_EQ(epd.displayed().bits, next.bits);
}

TEST_F(EpdTest, DisplayedStaysOldWhenUpdateIsAbortedByReset) {
    bring_up();
    write_plane(0x24, box(0, 0, 8, 8));
    write_plane(0x26, box(0, 0, 8, 8));
    run(0xF7);
    clock.advance_us(1'000'000); // half way
    ASSERT_TRUE(epd.hardware_reset());
    EXPECT_EQ(epd.aborted_updates(), 1U);
    EXPECT_EQ(epd.full_updates(), 0U);
    EXPECT_EQ(epd.displayed().bits, gfx::Framebuffer{}.bits);
}

// --- reads --------------------------------------------------------------------------------------

TEST_F(EpdTest, TemperatureRegisterRoundTripAndReadFraming) {
    bring_up();
    EXPECT_EQ(epd.temperature_register(), 0x7FF);
    epd.set_temperature_dc(-50); // -5.0 C -> -80/16 -> 0xFB0
    run(0xB1);
    ASSERT_TRUE(epd.wait_idle(500));
    EXPECT_EQ(epd.temperature_register(), 0xFB0);
    std::array<std::uint8_t, 2> t{};
    ASSERT_TRUE(epd.command(0x1B));
    ASSERT_TRUE(epd.read(t));
    EXPECT_EQ(t[0], 0xFB);
    EXPECT_EQ(t[1], 0x00);
}

TEST_F(EpdTest, ReadRulesAndOtpOptionBytes) {
    bring_up();
    std::array<std::uint8_t, 11> otp{};
    expect_violation(epd.read(otp), EpdViolation::kReadWithoutCommand);
    ASSERT_TRUE(epd.command(0x2D));
    std::array<std::uint8_t, 10> short_buf{};
    expect_violation(epd.read(short_buf), EpdViolation::kBadReadLength);
    const std::array<std::uint8_t, 11> want{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    epd.set_otp_display_option(want);
    ASSERT_TRUE(epd.read(otp));
    EXPECT_EQ(otp, want);
    expect_violation(epd.read(otp), EpdViolation::kReadWithoutCommand); // consumed
    ASSERT_TRUE(epd.command(0x2D));
    expect_violation(epd.data(Bytes{0}), EpdViolation::kDataWithoutCommand);
}

// --- reset and deep sleep -----------------------------------------------------------------------

TEST_F(EpdTest, SoftResetRestoresRegistersButKeepsRam) {
    bring_up();
    write_plane(0x24, box(0, 0, 16, 16));
    const Bytes before(epd.ram_bw().begin(), epd.ram_bw().end());
    ASSERT_TRUE(send(0x22, {0xF7}));
    const std::uint32_t soft_before = epd.soft_resets();
    ASSERT_TRUE(send(0x12));
    EXPECT_EQ(epd.soft_resets(), soft_before + 1U);
    ASSERT_TRUE(epd.wait_idle(200));
    EXPECT_EQ(Bytes(epd.ram_bw().begin(), epd.ram_bw().end()), before);
    expect_violation(epd.command(0x20), EpdViolation::kNoUpdateControl); // 0x22 back to POR
    ASSERT_TRUE(send(0x4E, {0x00}));
    ASSERT_TRUE(send(0x4F, {0x00, 0x00}));
    ASSERT_TRUE(epd.command(0x24)); // window is POR again: off-RAM
    expect_violation(epd.data(Bytes{0}), EpdViolation::kBadParameter);
}

TEST_F(EpdTest, DeepSleepKeepsBusyHighRejectsTrafficAndNeedsHardwareReset) {
    bring_up();
    write_plane(0x24, box(0, 0, 16, 16));
    ASSERT_TRUE(send(0x10, {0x01}));
    EXPECT_TRUE(epd.in_deep_sleep());
    EXPECT_TRUE(epd.busy());
    expect_violation(epd.command(0x11), EpdViolation::kWhileAsleep);
    expect_violation(epd.data(Bytes{0}), EpdViolation::kWhileAsleep);
    std::array<std::uint8_t, 1> b{};
    expect_violation(epd.read(b), EpdViolation::kWhileAsleep);
    const std::int64_t t0 = clock.elapsed_us();
    const Status w = epd.wait_idle(50); // forbidden: BUSY never falls [R1 s2]
    ASSERT_FALSE(w);
    EXPECT_EQ(w.error().code, Errc::kTimeout);
    EXPECT_EQ(epd.last_violation(), EpdViolation::kBusyWaitAfterSleep);
    EXPECT_EQ(clock.elapsed_us() - t0, 50'000);
    EXPECT_TRUE(epd.in_deep_sleep());
    ASSERT_TRUE(epd.hardware_reset());
    EXPECT_FALSE(epd.in_deep_sleep());
    EXPECT_FALSE(epd.busy());
    EXPECT_TRUE(send(0x12));
    EXPECT_EQ(epd.deep_sleep_entries(), 1U);
}

TEST_F(EpdTest, DeepSleepMode2AndNormalModeValues) {
    bring_up();
    ASSERT_TRUE(send(0x10, {0x00})); // 00 = normal: stays awake
    EXPECT_FALSE(epd.in_deep_sleep());
    ASSERT_TRUE(send(0x10, {0x03})); // mode 2
    EXPECT_TRUE(epd.in_deep_sleep());
}

TEST_F(EpdTest, HardwareResetScramblesRamAndClearsRegisters) {
    bring_up();
    write_plane(0x24, box(0, 0, 64, 64));
    write_plane(0x26, box(0, 0, 64, 64));
    ASSERT_TRUE(epd.hardware_reset());
    EXPECT_NE(epd.ram_bw_image().bits, box(0, 0, 64, 64).bits);
    EXPECT_NE(epd.ram_red_image().bits, box(0, 0, 64, 64).bits);
    ASSERT_TRUE(send(0x22, {0xF7}));
    expect_violation(epd.command(0x20), EpdViolation::kExternalSensor); // sensor select at POR
    EXPECT_EQ(epd.hardware_resets(), 2U);
}

// --- rendering ----------------------------------------------------------------------------------

class VectorSink final : public gfx::ByteSink {
public:
    Status write(std::span<const std::uint8_t> bytes) override {
        data.insert(data.end(), bytes.begin(), bytes.end());
        return ok();
    }
    Bytes data;
};

TEST_F(EpdTest, RendersDisplayAndRamToPng) {
    bring_up();
    const gfx::Framebuffer img = box(10, 10, 50, 50);
    write_plane(0x24, img);
    write_plane(0x26, img);
    run(0xF7);
    ASSERT_TRUE(epd.wait_idle(5000));
    VectorSink shown;
    ASSERT_TRUE(epd.encode_displayed_png(shown));
    VectorSink ram;
    ASSERT_TRUE(epd.encode_ram_bw_png(ram));
    const Bytes sig{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    ASSERT_GT(shown.data.size(), sig.size() + gfx::kFrameBytes);
    EXPECT_TRUE(std::equal(sig.begin(), sig.end(), shown.data.begin()));
    EXPECT_EQ(shown.data, ram.data); // identical bytes: same image, deterministic encoder
    VectorSink direct;
    ASSERT_TRUE(gfx::encode_png(img, direct));
    EXPECT_EQ(shown.data, direct.data);
}

} // namespace
} // namespace qz::testkit
// NOLINTEND(readability-function-cognitive-complexity)
