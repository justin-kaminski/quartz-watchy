// FakeBoardIo: buttons, USB/charge pins, battery ADC pin, vibration motor.
#include "qz/hal/board_io.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

namespace qz::testkit {
namespace {

constexpr std::uint8_t kMenu = hal::kButtonBitMenu;
constexpr std::uint8_t kBack = hal::kButtonBitBack;
constexpr std::uint8_t kUp = hal::kButtonBitUp;
constexpr std::uint8_t kDown = hal::kButtonBitDown;

// --- buttons ------------------------------------------------------------------------------------

TEST(FakeBoardIo, StartsIdle) {
    const FakeBoardIo io;
    EXPECT_EQ(io.pressed_buttons(), 0);
    EXPECT_FALSE(io.usb_present());
    EXPECT_FALSE(io.charging());
    EXPECT_FALSE(io.vibrating());
    EXPECT_EQ(io.vibration_pulses(), 0U);
    EXPECT_EQ(io.vibration_ms_total(), 0U);
    EXPECT_EQ(io.adc_reads(), 0U);
}

TEST(FakeBoardIo, PressAndReleaseSetAndClearBitsIndependently) {
    FakeBoardIo io;
    io.press(kMenu);
    EXPECT_EQ(io.pressed_buttons(), kMenu);
    io.press(kDown);
    EXPECT_EQ(io.pressed_buttons(), kMenu | kDown);
    io.press(kDown); // pressing a held button changes nothing
    EXPECT_EQ(io.pressed_buttons(), kMenu | kDown);
    io.release(kMenu);
    EXPECT_EQ(io.pressed_buttons(), kDown);
    io.release(kUp); // releasing an idle button changes nothing
    EXPECT_EQ(io.pressed_buttons(), kDown);
    io.release(kDown);
    EXPECT_EQ(io.pressed_buttons(), 0);
}

TEST(FakeBoardIo, MasksCanHoldSeveralButtonsAtOnce) {
    FakeBoardIo io;
    io.press(kBack | kUp);
    EXPECT_EQ(io.pressed_buttons(), kBack | kUp);
    io.press(kMenu | kDown);
    EXPECT_EQ(io.pressed_buttons(), 0x0F);
    io.release(kBack | kUp | kMenu);
    EXPECT_EQ(io.pressed_buttons(), kDown);
    io.release(0);
    io.press(0);
    EXPECT_EQ(io.pressed_buttons(), kDown);
}

TEST(FakeBoardIo, BitsMatchTheHalButtonPositions) {
    FakeBoardIo io;
    io.press(1U << 0U);
    io.press(1U << 3U);
    EXPECT_EQ(io.pressed_buttons(), kMenu | kDown);
}

TEST(FakeBoardIoDeathTest, MaskBitsBeyondTheFourButtonsAreRejected) {
    FakeBoardIo io;
    EXPECT_DEATH(io.press(0x10), "QZ_ASSERT");
    EXPECT_DEATH(io.press(0x80), "QZ_ASSERT");
    EXPECT_DEATH(io.release(0x20), "QZ_ASSERT");
}

// --- USB and charge pins ------------------------------------------------------------------------

TEST(FakeBoardIo, UsbAndChargeStateAreSetTogether) {
    FakeBoardIo io;
    io.set_usb(true, false);
    EXPECT_TRUE(io.usb_present());
    EXPECT_FALSE(io.charging());
    io.set_usb(true, true);
    EXPECT_TRUE(io.usb_present());
    EXPECT_TRUE(io.charging());
    io.set_usb(false, false);
    EXPECT_FALSE(io.usb_present());
    EXPECT_FALSE(io.charging());
}

TEST(FakeBoardIoDeathTest, ChargingWithoutUsbIsImpossible) {
    FakeBoardIo io;
    EXPECT_DEATH(io.set_usb(false, true), "QZ_ASSERT.*present");
}

// --- battery ADC pin ----------------------------------------------------------------------------

TEST(FakeBoardIo, AdcReadsTheScriptedPinVoltage) {
    FakeBoardIo io;
    hal::Adc& adc = io;
    const Result<std::uint16_t> initial = adc.read_pin_mv();
    ASSERT_TRUE(initial);
    EXPECT_EQ(*initial, 2800) << "documented default";

    io.set_pin_mv(3012);
    const Result<std::uint16_t> scripted = adc.read_pin_mv();
    ASSERT_TRUE(scripted);
    EXPECT_EQ(*scripted, 3012);

    io.set_pin_mv(0);
    EXPECT_EQ(adc.read_pin_mv().value_or(1), 0);
}

TEST(FakeBoardIo, AdcFailureIsInjectedAndRecoverable) {
    FakeBoardIo io;
    io.set_pin_mv(2900);
    io.fail_adc(true);
    const Result<std::uint16_t> failed = io.read_pin_mv();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, Errc::kIo);

    io.fail_adc(false);
    const Result<std::uint16_t> recovered = io.read_pin_mv();
    ASSERT_TRUE(recovered);
    EXPECT_EQ(*recovered, 2900);
}

TEST(FakeBoardIo, EveryAdcReadIsCountedIncludingFailedOnes) {
    FakeBoardIo io;
    EXPECT_TRUE(io.read_pin_mv());
    EXPECT_TRUE(io.read_pin_mv());
    io.fail_adc(true);
    EXPECT_FALSE(io.read_pin_mv());
    EXPECT_EQ(io.adc_reads(), 3U);
}

// --- vibration motor ----------------------------------------------------------------------------

TEST(FakeBoardIoVibration, WithoutAClockPulsesAreCountedButHaveNoDuration) {
    FakeBoardIo io;
    io.set_vibration(true);
    EXPECT_TRUE(io.vibrating());
    io.set_vibration(false);
    EXPECT_FALSE(io.vibrating());
    io.set_vibration(true);
    io.set_vibration(false);
    EXPECT_EQ(io.vibration_pulses(), 2U);
    EXPECT_EQ(io.vibration_ms_total(), 0U);
}

TEST(FakeBoardIoVibration, WithAClockPulsesAreTimedInVirtualTime) {
    VirtualClock clock;
    FakeBoardIo io(clock);
    hal::BoardIo& board = io;

    board.set_vibration(true);
    clock.delay_ms(40);
    board.set_vibration(false);
    EXPECT_EQ(io.vibration_ms_total(), 40U);

    clock.delay_ms(500); // motor off: does not count
    EXPECT_EQ(io.vibration_ms_total(), 40U);

    board.set_vibration(true);
    clock.delay_ms(15);
    board.set_vibration(false);
    EXPECT_EQ(io.vibration_ms_total(), 55U);
    EXPECT_EQ(io.vibration_pulses(), 2U);
}

TEST(FakeBoardIoVibration, ARunningPulseCountsUpToNow) {
    VirtualClock clock;
    FakeBoardIo io(clock);
    io.set_vibration(true);
    EXPECT_EQ(io.vibration_ms_total(), 0U);
    clock.delay_ms(25);
    EXPECT_TRUE(io.vibrating());
    EXPECT_EQ(io.vibration_ms_total(), 25U);
    clock.delay_ms(5);
    EXPECT_EQ(io.vibration_ms_total(), 30U);
    io.set_vibration(false);
    clock.delay_ms(100);
    EXPECT_EQ(io.vibration_ms_total(), 30U);
}

TEST(FakeBoardIoVibration, RepeatedLevelsAreNotNewEdges) {
    VirtualClock clock;
    FakeBoardIo io(clock);
    io.set_vibration(true);
    clock.delay_ms(10);
    io.set_vibration(true); // already on: the pulse neither restarts nor counts again
    clock.delay_ms(20);
    io.set_vibration(false);
    io.set_vibration(false);
    EXPECT_EQ(io.vibration_pulses(), 1U);
    EXPECT_EQ(io.vibration_ms_total(), 30U);
}

TEST(FakeBoardIoVibration, DurationsAreTruncatedToWholeMilliseconds) {
    VirtualClock clock;
    FakeBoardIo io(clock);
    io.set_vibration(true);
    clock.delay_us(1500);
    io.set_vibration(false);
    EXPECT_EQ(io.vibration_ms_total(), 1U);
    io.set_vibration(true);
    clock.delay_us(999);
    io.set_vibration(false);
    EXPECT_EQ(io.vibration_ms_total(), 2U) << "1500 us + 999 us = 2499 us";
}

TEST(FakeBoardIoVibration, IsMeasuredInIdealTimeNotInRtcOrTruthTime) {
    // A slow crystal, a jump of the ground-truth clock and a power loss in mid-pulse must not
    // change an 80 ms pulse.
    VirtualClock clock;
    FakeBoardIo io(clock);
    clock.set_crystal_error_ppb(-50'000);
    io.set_vibration(true);
    clock.delay_ms(30);
    clock.set_true_utc_us(1'700'000'000'000'000);
    clock.delay_ms(30);
    clock.power_loss();
    clock.delay_ms(20);
    io.set_vibration(false);
    EXPECT_EQ(io.vibration_ms_total(), 80U);
}

TEST(FakeBoardIo, ClockCannotBeATemporary) {
    static_assert(std::is_constructible_v<FakeBoardIo, VirtualClock&>);
    static_assert(std::is_constructible_v<FakeBoardIo, const VirtualClock&>);
    static_assert(!std::is_constructible_v<FakeBoardIo, VirtualClock>, "would dangle");
    static_assert(std::is_default_constructible_v<FakeBoardIo>);
    SUCCEED();
}

} // namespace
} // namespace qz::testkit
