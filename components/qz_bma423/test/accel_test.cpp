// Accelerometer wrapper + vendored Bosch API against the FakeBma423 register model.
#include "qz/bma423/accel.hpp"
#include "qz/core/crc32.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

// The Bosch feature-config blob (third_party/bosch/bma423.c, 6144 bytes).
extern "C" const std::uint8_t bma423_config_file[]; // NOLINT(readability-identifier-naming)

namespace qz::bma423 {
namespace {

constexpr std::size_t kBlobBytes = 6144;
/// CRC-32 of the decoded blob; its SHA-256 (112f81c8...) is in third_party/bosch/PROVENANCE.md.
constexpr std::uint32_t kBlobCrc32 = 0xe08e8a46U;

constexpr std::uint8_t kRegAccConf = 0x40;
constexpr std::uint8_t kRegInt1Io = 0x53;
constexpr std::uint8_t kRegLatch = 0x55;
constexpr std::uint8_t kRegInt1Map = 0x56;
constexpr std::uint8_t kRegPwrConf = 0x7C;
constexpr std::uint8_t kRegPwrCtrl = 0x7D;
constexpr std::uint8_t kRegEvent = 0x1B;
constexpr std::size_t kStepCtrlByte = 0x3B;
constexpr std::size_t kDoubleTapByte = 0x3E;
constexpr std::size_t kRemapByte = 0x44;

constexpr std::uint8_t kTap = static_cast<std::uint8_t>(IntStatus::kDoubleTap);
constexpr std::uint8_t kOther = static_cast<std::uint8_t>(IntStatus::kOther);

class AccelTest : public ::testing::Test {
protected:
    testkit::VirtualClock clock_;
    testkit::FakeBma423 fake_{clock_};
    Accelerometer acc_{fake_, clock_};

    void init_ok(const Config& config = {}) {
        const Status st = acc_.init(config);
        ASSERT_TRUE(st) << "init failed: " << static_cast<int>(st.error().code);
        ASSERT_EQ(fake_.protocol_violations(), 0U);
    }
};

TEST_F(AccelTest, BlobIsTheRecordedOne) {
    const std::span<const std::uint8_t> blob(bma423_config_file, kBlobBytes);
    EXPECT_EQ(crc32(blob), kBlobCrc32);
    EXPECT_EQ(blob[0], 0x80);
    EXPECT_EQ(blob[2], 0x38);
    EXPECT_EQ(blob[kBlobBytes - 8], 0x80);
    EXPECT_EQ(blob[kBlobBytes - 2], 0x18);
}

TEST_F(AccelTest, InitUploadsBlobAndChecksInternalStatus) {
    EXPECT_FALSE(*acc_.feature_engine_ok()); // power-up: not initialised
    init_ok();
    EXPECT_EQ(fake_.config_uploads(), 1U);
    EXPECT_EQ(fake_.config_chunks(), 96U); // 6144 / 64-byte bursts
    EXPECT_EQ(fake_.config_bytes_received(), kBlobBytes);
    EXPECT_EQ(fake_.config_crc32(), kBlobCrc32);
    EXPECT_TRUE(fake_.engine_running());
    EXPECT_TRUE(*acc_.feature_engine_ok());
    EXPECT_EQ(fake_.soft_resets(), 1U);
    // 10 ms reset wait + 150 ms init wait, nothing slow beyond that.
    EXPECT_GE(clock_.elapsed_us(), 160'000);
    EXPECT_LT(clock_.elapsed_us(), 400'000);
}

TEST_F(AccelTest, InitLeavesStepCounterOnlyLowPowerConfiguration) {
    init_ok();
    EXPECT_EQ(fake_.reg(kRegPwrConf), 0x03);     // adv_power_save on, written last
    EXPECT_EQ(fake_.reg(kRegAccConf), 0x17);     // low power, avg 2, 50 Hz
    EXPECT_NE(fake_.reg(kRegPwrCtrl) & 0x04, 0); // accelerometer enabled
    EXPECT_NE(fake_.feature_byte(kStepCtrlByte) & 0x10, 0);
    EXPECT_EQ(fake_.feature_byte(kDoubleTapByte) & 0x01, 0);
    EXPECT_EQ(fake_.reg(kRegInt1Io), 0x00); // INT1 output disabled, active-low
    EXPECT_EQ(fake_.reg(kRegInt1Map), 0x00);
    EXPECT_EQ(fake_.reg(kRegEvent), 0x00); // por_detected consumed
}

TEST_F(AccelTest, InitProgramsAxisRemap) {
    init_ok(); // default remap: x->x, y->y, z->z
    EXPECT_EQ(fake_.feature_byte(kRemapByte), 0x88);
    EXPECT_EQ(fake_.feature_byte(kRemapByte + 1), 0x00);
}

TEST_F(AccelTest, InitProgramsWatchyAxisRemap) {
    Config config;
    config.remap = {.x_axis = 1, .x_sign = 0, .y_axis = 0, .y_sign = 0, .z_axis = 2, .z_sign = 1};
    init_ok(config); // research s7 table row 5
    EXPECT_EQ(fake_.feature_byte(kRemapByte), 0x81);
    EXPECT_EQ(fake_.feature_byte(kRemapByte + 1), 0x01);
}

TEST_F(AccelTest, ChipIdMismatchIsNotFoundAndNothingIsWritten) {
    fake_.set_chip_id(0x14);
    const Status st = acc_.init({});
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code, Errc::kNotFound);
    EXPECT_EQ(st.error().detail, 0x14);
    EXPECT_EQ(fake_.write_calls(), 0U);
    EXPECT_FALSE(fake_.engine_running());
    EXPECT_EQ(acc_.attach().error().code, Errc::kNotFound);
    EXPECT_EQ(*acc_.chip_id(), 0x14);
}

TEST_F(AccelTest, ChipIdIsReadAndMatchesConstant) {
    const Result<std::uint8_t> id = acc_.chip_id();
    ASSERT_TRUE(id);
    EXPECT_EQ(*id, kChipId);
}

TEST_F(AccelTest, BusFailureRetriesChipIdThenReportsIo) {
    fake_.fail_io(true);
    const Status st = acc_.init({});
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code, Errc::kIo);
    EXPECT_EQ(fake_.read_calls(), 3U);
    EXPECT_EQ(clock_.elapsed_us(), 10'000); // two 5 ms retry waits
}

TEST_F(AccelTest, ConfigStreamErrorIsCorrupt) {
    fake_.fail_config_load(true);
    const Status st = acc_.init({});
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code, Errc::kCorrupt);
    EXPECT_FALSE(*acc_.feature_engine_ok());
}

TEST_F(AccelTest, InitTwiceSoftResetsAndStaysWithinProtocol) {
    init_ok();
    fake_.add_steps(50);
    init_ok();
    EXPECT_EQ(fake_.soft_resets(), 2U);
    EXPECT_EQ(fake_.config_uploads(), 2U);
    EXPECT_EQ(*acc_.step_count(), 0U); // re-init wipes the counter (documented hazard)
}

TEST_F(AccelTest, StepCountReadsLittleEndianCounter) {
    init_ok();
    EXPECT_EQ(*acc_.step_count(), 0U);
    fake_.add_steps(0x01020304U);
    EXPECT_EQ(*acc_.step_count(), 0x01020304U);
    fake_.add_steps(1);
    EXPECT_EQ(*acc_.step_count(), 0x01020305U);
    EXPECT_EQ(fake_.protocol_violations(), 0U);
}

TEST_F(AccelTest, ResetStepCounterClearsAndKeepsCounting) {
    init_ok();
    fake_.add_steps(500);
    ASSERT_TRUE(acc_.reset_step_counter());
    EXPECT_EQ(*acc_.step_count(), 0U);
    EXPECT_EQ(fake_.feature_byte(kStepCtrlByte) & 0x04, 0); // strobe self-cleared
    EXPECT_NE(fake_.feature_byte(kStepCtrlByte) & 0x10, 0); // counter still enabled
    fake_.add_steps(5);
    EXPECT_EQ(*acc_.step_count(), 5U);
    EXPECT_EQ(fake_.protocol_violations(), 0U);
}

TEST_F(AccelTest, PowerLossIsVisibleThroughAttachAndFeatureEngineOk) {
    init_ok();
    fake_.add_steps(100);
    fake_.sensor_reset();                    // the rail dropped while the MCU survived
    ASSERT_TRUE(acc_.attach());              // chip answers
    EXPECT_FALSE(*acc_.feature_engine_ok()); // but the engine is gone: the app must re-init
    EXPECT_EQ(*acc_.step_count(), 0U);
    init_ok();
    fake_.add_steps(7);
    EXPECT_EQ(*acc_.step_count(), 7U);
}

TEST_F(AccelTest, AttachPerformsNoWritesAndNoDelay) {
    init_ok();
    const std::uint32_t writes = fake_.write_calls();
    const std::uint32_t reads = fake_.read_calls();
    const std::int64_t before_us = clock_.elapsed_us();
    ASSERT_TRUE(acc_.attach());
    EXPECT_EQ(fake_.write_calls(), writes);
    EXPECT_EQ(fake_.read_calls(), reads + 1U); // chip id only
    EXPECT_EQ(clock_.elapsed_us(), before_us);
    EXPECT_TRUE(fake_.engine_running()); // untouched
}

TEST_F(AccelTest, AttachOnFreshWrapperKeepsRunningEngineAndCounter) {
    init_ok();
    fake_.add_steps(42);
    Accelerometer woken(fake_, clock_); // new object after deep sleep: no state carried
    const std::uint32_t writes = fake_.write_calls();
    ASSERT_TRUE(woken.attach());
    EXPECT_EQ(*woken.step_count(), 42U);
    EXPECT_EQ(fake_.write_calls(), writes);
}

TEST_F(AccelTest, InitWithTapConfiguresDoubleTapOnInt1) {
    Config config;
    config.tap_interrupt = true;
    init_ok(config);
    EXPECT_EQ(fake_.reg(kRegAccConf), 0x09); // 200 Hz, low power, no averaging (tap needs 200 Hz)
    EXPECT_NE(fake_.feature_byte(kDoubleTapByte) & 0x01, 0);
    EXPECT_EQ(fake_.reg(kRegInt1Map), 0x10); // DOUBLE_TAP_INT -> INT1
    EXPECT_EQ(fake_.reg(kRegInt1Io), 0x08);  // output on, push-pull, active-low, level
    EXPECT_EQ(fake_.reg(kRegLatch), 0x01);   // feature interrupts are latched-only
    EXPECT_FALSE(fake_.int1_active());
    EXPECT_TRUE(fake_.int1_level_high()); // idle high

    fake_.trigger_double_tap();
    EXPECT_TRUE(fake_.int1_active());
    EXPECT_FALSE(fake_.int1_level_high());
    EXPECT_EQ(*acc_.read_int_status(), kTap);
    EXPECT_FALSE(fake_.int1_active()); // reading cleared the latch and released the pin
    EXPECT_TRUE(fake_.int1_level_high());
    EXPECT_EQ(*acc_.read_int_status(), 0);
}

TEST_F(AccelTest, Int1PolarityFollowsConfig) {
    for (const bool active_low : {true, false}) {
        fake_.sensor_reset();
        Config config;
        config.tap_interrupt = true;
        config.int1_active_low = active_low;
        init_ok(config);
        EXPECT_EQ((fake_.reg(kRegInt1Io) & 0x02) != 0, !active_low);
        EXPECT_EQ(fake_.int1_level_high(), active_low); // idle level is the inactive one
        fake_.trigger_double_tap();
        EXPECT_EQ(fake_.int1_level_high(), !active_low);
        EXPECT_EQ(*acc_.read_int_status(), kTap);
    }
}

TEST_F(AccelTest, PolarityIsProgrammedEvenWithTapOff) {
    Config config;
    config.int1_active_low = false;
    init_ok(config);
    EXPECT_EQ(fake_.reg(kRegInt1Io), 0x02); // active-high, output still disabled
}

TEST_F(AccelTest, SetTapInterruptTogglesAtRuntimeAndKeepsPolarity) {
    Config config;
    config.int1_active_low = false;
    init_ok(config);

    ASSERT_TRUE(acc_.set_tap_interrupt(true));
    EXPECT_EQ(fake_.reg(kRegInt1Io), 0x0A); // output on, polarity preserved
    EXPECT_EQ(fake_.reg(kRegAccConf), 0x09);
    EXPECT_EQ(fake_.reg(kRegInt1Map), 0x10);
    fake_.trigger_double_tap();
    EXPECT_TRUE(fake_.int1_level_high()); // active-high
    EXPECT_EQ(*acc_.read_int_status(), kTap);

    ASSERT_TRUE(acc_.set_tap_interrupt(false));
    EXPECT_EQ(fake_.reg(kRegInt1Io), 0x02);
    EXPECT_EQ(fake_.reg(kRegInt1Map), 0x00);
    EXPECT_EQ(fake_.reg(kRegAccConf), 0x17); // back to the 18 uA step-counter mode
    EXPECT_EQ(fake_.feature_byte(kDoubleTapByte) & 0x01, 0);
    fake_.trigger_double_tap(); // feature is off: ignored
    EXPECT_EQ(*acc_.read_int_status(), 0);
    EXPECT_EQ(fake_.protocol_violations(), 0U); // 1 ms write spacing in low-power mode honoured
    EXPECT_NE(fake_.feature_byte(kStepCtrlByte) & 0x10, 0); // step counter untouched
}

TEST_F(AccelTest, ReadIntStatusClassifiesOtherInterrupts) {
    init_ok();
    fake_.raise_feature_interrupt(0x20); // any-motion
    EXPECT_EQ(*acc_.read_int_status(), kOther);
    fake_.raise_feature_interrupt(0x30); // double tap + any-motion
    EXPECT_EQ(*acc_.read_int_status(), kTap | kOther);
    EXPECT_EQ(*acc_.read_int_status(), 0);
}

TEST_F(AccelTest, EveryOperationReportsBusErrors) {
    init_ok();
    fake_.fail_io(true);
    EXPECT_EQ(acc_.chip_id().error().code, Errc::kIo);
    EXPECT_EQ(acc_.attach().error().code, Errc::kIo);
    EXPECT_EQ(acc_.step_count().error().code, Errc::kIo);
    EXPECT_EQ(acc_.reset_step_counter().error().code, Errc::kIo);
    EXPECT_EQ(acc_.set_tap_interrupt(true).error().code, Errc::kIo);
    EXPECT_EQ(acc_.read_int_status().error().code, Errc::kIo);
    EXPECT_EQ(acc_.feature_engine_ok().error().code, Errc::kIo);
    fake_.fail_io(false);
    EXPECT_TRUE(acc_.attach());
}

} // namespace
} // namespace qz::bma423
