// The FakeBma423 register model itself (raw register protocol, no wrapper involved).
#include "qz/core/crc32.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

namespace qz::testkit {
namespace {

constexpr std::uint8_t kRegChipId = 0x00;
constexpr std::uint8_t kRegEvent = 0x1B;
constexpr std::uint8_t kRegIntStatus0 = 0x1C;
constexpr std::uint8_t kRegStep0 = 0x1E;
constexpr std::uint8_t kRegInternalStatus = 0x2A;
constexpr std::uint8_t kRegAccConf = 0x40;
constexpr std::uint8_t kRegInt1Io = 0x53;
constexpr std::uint8_t kRegInt1Map = 0x56;
constexpr std::uint8_t kRegInitCtrl = 0x59;
constexpr std::uint8_t kRegAsicLsb = 0x5B;
constexpr std::uint8_t kRegAsicMsb = 0x5C;
constexpr std::uint8_t kRegFeatures = 0x5E;
constexpr std::uint8_t kRegPwrConf = 0x7C;
constexpr std::uint8_t kRegCmd = 0x7E;

std::uint8_t rd(FakeBma423& f, std::uint8_t reg) {
    std::array<std::uint8_t, 1> v{};
    EXPECT_TRUE(f.read_registers(reg, v));
    return v[0];
}

void wr(FakeBma423& f, std::uint8_t reg, std::uint8_t value) {
    const std::array<std::uint8_t, 1> v{value};
    EXPECT_TRUE(f.write_registers(reg, v));
}

/// The Bosch config sequence for a 6144-byte image, `chunks` bursts of 64 bytes.
std::array<std::uint8_t, 6144> upload(FakeBma423& f, std::size_t chunks) {
    std::array<std::uint8_t, 6144> image{};
    for (std::size_t i = 0; i < image.size(); ++i) {
        image[i] = static_cast<std::uint8_t>((i * 7U) + 3U);
    }
    wr(f, kRegPwrConf, 0x02); // adv_power_save off
    wr(f, kRegInitCtrl, 0x00);
    for (std::size_t c = 0; c < chunks; ++c) {
        const std::size_t word = c * 32U;
        wr(f, kRegAsicLsb, static_cast<std::uint8_t>(word & 0x0FU));
        wr(f, kRegAsicMsb, static_cast<std::uint8_t>(word >> 4U));
        EXPECT_TRUE(f.write_registers(kRegFeatures,
                                      std::span<const std::uint8_t>(image).subspan(c * 64U, 64U)));
    }
    wr(f, kRegInitCtrl, 0x01);
    return image;
}

/// Writes the whole 70-byte feature block (what feature_enable() does).
void write_block(FakeBma423& f, std::size_t byte, std::uint8_t value) {
    std::array<std::uint8_t, 70> block{};
    EXPECT_TRUE(f.read_registers(kRegFeatures, block));
    block[byte] = value;
    EXPECT_TRUE(f.write_registers(kRegFeatures, block));
}

TEST(FakeBma423, PowersUpWithDocumentedResetValues) {
    FakeBma423 f;
    EXPECT_EQ(rd(f, kRegChipId), 0x13);
    EXPECT_EQ(rd(f, kRegPwrConf), 0x03);
    EXPECT_EQ(rd(f, kRegAccConf), 0xA8);
    EXPECT_EQ(rd(f, kRegInternalStatus), 0x00);
    EXPECT_EQ(rd(f, kRegEvent), 0x01); // por_detected...
    EXPECT_EQ(rd(f, kRegEvent), 0x00); // ...clears on read
    EXPECT_EQ(f.read_calls(), 6U);
    f.set_chip_id(0x42);
    EXPECT_EQ(rd(f, kRegChipId), 0x42);
}

TEST(FakeBma423, ConfigUploadMakesEngineReadyAndRecordsImage) {
    FakeBma423 f;
    const auto image = upload(f, 96);
    EXPECT_EQ(rd(f, kRegInternalStatus), 0x01);
    EXPECT_TRUE(f.engine_running());
    EXPECT_EQ(f.config_uploads(), 1U);
    EXPECT_EQ(f.config_chunks(), 96U);
    EXPECT_EQ(f.config_bytes_received(), 6144U);
    EXPECT_EQ(f.config_crc32(), crc32(image));
    EXPECT_EQ(f.protocol_violations(), 0U);
}

TEST(FakeBma423, IncompleteOrForcedFailedUploadReportsInitError) {
    FakeBma423 partial;
    upload(partial, 95);
    EXPECT_EQ(rd(partial, kRegInternalStatus), 0x02);
    EXPECT_FALSE(partial.engine_running());
    EXPECT_EQ(partial.config_uploads(), 0U);

    FakeBma423 forced;
    forced.fail_config_load(true);
    upload(forced, 96);
    EXPECT_EQ(rd(forced, kRegInternalStatus), 0x02);
    EXPECT_FALSE(forced.engine_running());
}

TEST(FakeBma423, FlagsHostProtocolViolations) {
    FakeBma423 f;
    wr(f, kRegPwrConf, 0x02);
    wr(f, kRegInitCtrl, 0x00);
    const std::array<std::uint8_t, 3> odd{1, 2, 3};
    EXPECT_TRUE(f.write_registers(kRegFeatures, odd)); // odd burst
    EXPECT_EQ(f.protocol_violations(), 1U);
    wr(f, kRegAsicLsb, 0x0F);
    wr(f, kRegAsicMsb, 0xFF); // word 0xFFF -> byte 8190: overruns the image
    const std::array<std::uint8_t, 4> even{1, 2, 3, 4};
    EXPECT_TRUE(f.write_registers(kRegFeatures, even));
    EXPECT_EQ(f.protocol_violations(), 2U);
    wr(f, kRegPwrConf, 0x03); // adv_power_save on: feature access not allowed
    EXPECT_TRUE(f.write_registers(kRegFeatures, even));
    std::array<std::uint8_t, 4> sink{};
    EXPECT_TRUE(f.read_registers(kRegFeatures, sink));
    EXPECT_EQ(f.protocol_violations(), 4U);
    EXPECT_EQ(f.config_chunks(), 0U);

    FakeBma423 g;
    upload(g, 96);
    wr(g, kRegInitCtrl, 0x01); // enable features twice on a running engine
    EXPECT_EQ(g.protocol_violations(), 1U);
    EXPECT_TRUE(g.engine_running());
}

TEST(FakeBma423, InitOkAppearsOnlyAfter140MsWithAClock) {
    VirtualClock clock;
    FakeBma423 f(clock);
    upload(f, 96);
    EXPECT_EQ(rd(f, kRegInternalStatus), 0x00);
    clock.advance_us(139'999);
    EXPECT_EQ(rd(f, kRegInternalStatus), 0x00);
    clock.advance_us(1);
    EXPECT_EQ(rd(f, kRegInternalStatus), 0x01);
}

TEST(FakeBma423, StepCounterRegistersCountOnlyWhenEnabledAndResetByStrobe) {
    FakeBma423 f;
    f.add_steps(5); // engine not running: dropped
    upload(f, 96);
    wr(f, kRegPwrConf, 0x02);
    f.add_steps(5); // running but en_counter clear: dropped
    EXPECT_EQ(f.step_counter(), 0U);
    write_block(f, 0x3B, 0x10);
    f.add_steps(0x0A0B0C0DU);
    std::array<std::uint8_t, 4> bytes{};
    EXPECT_TRUE(f.read_registers(kRegStep0, bytes));
    EXPECT_EQ(bytes, (std::array<std::uint8_t, 4>{0x0D, 0x0C, 0x0B, 0x0A}));
    write_block(f, 0x3B, 0x14); // reset strobe
    EXPECT_EQ(f.step_counter(), 0U);
    EXPECT_EQ(f.feature_byte(0x3B), 0x10);
    EXPECT_EQ(f.protocol_violations(), 0U);
}

TEST(FakeBma423, FeatureInterruptsLatchMapToInt1AndClearOnRead) {
    FakeBma423 f;
    wr(f, kRegInt1Io, 0x08); // output on, active-low
    wr(f, kRegInt1Map, 0x10);
    EXPECT_TRUE(f.int1_level_high());
    f.raise_feature_interrupt(0x10);
    EXPECT_TRUE(f.int1_active());
    EXPECT_FALSE(f.int1_level_high());
    EXPECT_EQ(rd(f, kRegIntStatus0), 0x10);
    EXPECT_FALSE(f.int1_active());
    EXPECT_EQ(rd(f, kRegIntStatus0), 0x00);
    wr(f, kRegInt1Io, 0x00); // output disabled: never driven
    f.raise_feature_interrupt(0x10);
    EXPECT_FALSE(f.int1_active());
    EXPECT_FALSE(f.int1_level_high());

    FakeBma423 g; // trigger_double_tap needs a running engine with the feature on
    g.trigger_double_tap();
    EXPECT_EQ(rd(g, kRegIntStatus0), 0x00);
    upload(g, 96);
    wr(g, kRegPwrConf, 0x02);
    write_block(g, 0x3E, 0x01);
    g.trigger_double_tap();
    EXPECT_EQ(rd(g, kRegIntStatus0), 0x10);
}

TEST(FakeBma423, SoftResetAndSensorResetWipeTheFeatureEngine) {
    FakeBma423 f;
    f.set_chip_id(0x13);
    upload(f, 96);
    wr(f, kRegPwrConf, 0x02);
    write_block(f, 0x3B, 0x10);
    f.add_steps(9);
    wr(f, kRegCmd, 0xB6);
    EXPECT_EQ(f.soft_resets(), 1U);
    EXPECT_FALSE(f.engine_running());
    EXPECT_EQ(f.step_counter(), 0U);
    EXPECT_EQ(rd(f, kRegPwrConf), 0x03);
    EXPECT_EQ(rd(f, kRegChipId), 0x13);
    EXPECT_EQ(f.config_bytes_received(), 0U);

    upload(f, 96);
    EXPECT_TRUE(f.engine_running());
    f.sensor_reset();
    EXPECT_FALSE(f.engine_running());
    EXPECT_EQ(rd(f, kRegInternalStatus), 0x00);
    EXPECT_EQ(rd(f, kRegEvent), 0x01); // por_detected again
}

TEST(FakeBma423, FailIoRejectsAndCountsAccesses) {
    FakeBma423 f;
    f.fail_io(true);
    std::array<std::uint8_t, 1> v{};
    EXPECT_EQ(f.read_registers(kRegChipId, v).error().code, Errc::kIo);
    EXPECT_EQ(f.write_registers(kRegAccConf, v).error().code, Errc::kIo);
    EXPECT_EQ(f.read_calls(), 1U);
    EXPECT_EQ(f.write_calls(), 1U);
    f.fail_io(false);
    EXPECT_EQ(rd(f, kRegAccConf), 0xA8); // the failed write was not applied
}

TEST(FakeBma423, RejectsOutOfRangeRegisterAccess) {
    FakeBma423 f;
    std::array<std::uint8_t, 4> v{};
    EXPECT_EQ(f.read_registers(0x7E, v).error().code, Errc::kBadArgs);
    EXPECT_EQ(f.write_registers(0x7E, v).error().code, Errc::kBadArgs);
    EXPECT_EQ(f.reg(0xFF), 0x00);
    EXPECT_EQ(f.feature_byte(70), 0x00);
}

TEST(FakeBma423, ChecksLowPowerIdleTimeWhenGivenAClock) {
    VirtualClock clock;
    FakeBma423 f(clock);
    wr(f, kRegAccConf, 0x17); // adv_power_save is on after reset
    rd(f, kRegAccConf);       // read right after a write: too early
    EXPECT_EQ(f.protocol_violations(), 1U);
    clock.advance_us(450);
    rd(f, kRegAccConf);
    EXPECT_EQ(f.protocol_violations(), 1U);
    wr(f, kRegAccConf, 0x07);
    clock.advance_us(449);
    wr(f, kRegAccConf, 0x17); // 449 us after a write
    EXPECT_EQ(f.protocol_violations(), 2U);
    wr(f, kRegPwrConf, 0x02); // configuration mode: no idle rule
    wr(f, kRegAccConf, 0x07);
    EXPECT_EQ(f.protocol_violations(), 3U); // the PWR_CONF write itself followed too early
}

} // namespace
} // namespace qz::testkit
