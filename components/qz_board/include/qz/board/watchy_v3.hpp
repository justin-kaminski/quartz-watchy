// Watchy v3 board description (pure constants + constexpr wake-mask helpers).
// Pin numbers: docs/SPEC.md, verified against the v3.0 schematic in docs/research/hardware.md (R1).
// Polarities/divider from R1's schematic reading; confirm on hardware (HARDWARE_BRINGUP.md B3-B6).
#pragma once

#include <array>
#include <cstdint>

namespace qz::board {

enum class Level : std::uint8_t { kLow = 0, kHigh = 1 };

// I2C (BMA423)
inline constexpr std::uint8_t kI2cSda = 12;
inline constexpr std::uint8_t kI2cScl = 11;
inline constexpr std::uint8_t kBma423Address = 0x18; ///< SDO tied to GND [R1 s7]
inline constexpr std::uint32_t kI2cHz = 400'000;
// Buttons, index order == hal::kButtonBit* order (Menu, Back, Up, Down)
inline constexpr std::array<std::uint8_t, 4> kButtonPins = {7, 6, 0, 8};
inline constexpr Level kButtonActive =
    Level::kLow; ///< external 100k pull-ups, switch to GND [R1 s6]
// Accelerometer interrupts
inline constexpr std::uint8_t kAccelInt1 = 14;
inline constexpr std::uint8_t kAccelInt2 = 13;
inline constexpr Level kAccelIntActive = kButtonActive; ///< BMA423 INT1 configured to match
// Display (SSD1681 over SPI2)
inline constexpr std::uint8_t kEpdCs = 33;
inline constexpr std::uint8_t kEpdDc = 34;
inline constexpr std::uint8_t kEpdReset = 35;
inline constexpr std::uint8_t kEpdBusy = 36;
inline constexpr std::uint8_t kSpiMosi = 48;
inline constexpr std::uint8_t kSpiSck = 47;
inline constexpr std::uint8_t kSpiMisoUnused =
    46; ///< strapping pin, no-connect on the board [R1 s6]: never configure
inline constexpr std::uint32_t kSpiHz =
    10'000'000; ///< half the SSD1681 20 MHz write limit [ssd1681.md s2]
// Power
inline constexpr std::uint8_t kVibration = 17;
inline constexpr std::uint8_t kBatteryAdc = 9;
inline constexpr std::uint8_t kChargeStatus = 10;
inline constexpr std::uint8_t kUsbDetect = 21;
inline constexpr Level kUsbDetectActive = Level::kHigh; ///< VBUS divider 0.662 x VBUS [R1 s5]
/// GPIO10 is HIGH whenever USB is present, charging or full [R1 s5]; it is NOT a charge-state
/// signal.
inline constexpr Level kChargeStatusActive = Level::kHigh;
inline constexpr std::uint16_t kBatteryDividerNum = 460; ///< battery mV = pin mV * Num / Den
inline constexpr std::uint16_t kBatteryDividerDen = 360; ///< R8 100k / R9 360k [R1 s4]
inline constexpr std::uint16_t kAdcCalibratedMaxPinMv =
    2900; ///< 12 dB range end = 3706 mV battery [R1 s4]
inline constexpr std::uint16_t kBatteryCapacityMah =
    170; ///< cell minimum (180 typ; marketing 200) [R1 s3]

/// ESP32-S3: RTC GPIOs are 0..21 (soc_caps.h SOC_RTCIO_PIN_COUNT 22).
constexpr bool is_rtc_gpio(std::uint8_t gpio) noexcept {
    return gpio <= 21;
}
/// ESP32-S3 strapping pins.
constexpr bool is_strapping(std::uint8_t gpio) noexcept {
    return gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46;
}

struct WakeConfig {
    std::uint64_t ext1_mask = 0;
    Level ext1_level =
        Level::kLow; ///< EXT1 mode: ANY_LOW or ANY_HIGH (one mode for all pins on S3)
    bool ext0_enabled = false;
    std::uint8_t ext0_gpio = 0;
    Level ext0_level = Level::kHigh;
};

/// Derives EXT0/EXT1 wake configuration from the pin polarities (ARCHITECTURE.md section 5):
/// buttons (+ INT1 when enabled) on EXT1; USB detect joins EXT1 when its polarity matches,
/// otherwise uses EXT0.
constexpr WakeConfig wake_config(bool accel_wake, bool usb_wake) noexcept {
    WakeConfig c{};
    c.ext1_level = kButtonActive;
    for (const std::uint8_t pin : kButtonPins) {
        c.ext1_mask |= (std::uint64_t{1} << pin);
    }
    if (accel_wake) {
        c.ext1_mask |= (std::uint64_t{1} << kAccelInt1);
    }
    if (usb_wake) {
        if (kUsbDetectActive == kButtonActive) {
            c.ext1_mask |= (std::uint64_t{1} << kUsbDetect);
        } else {
            c.ext0_enabled = true;
            c.ext0_gpio = kUsbDetect;
            c.ext0_level = kUsbDetectActive;
        }
    }
    return c;
}

static_assert(kAccelIntActive == kButtonActive, "EXT1 has a single polarity on ESP32-S3");
static_assert(is_rtc_gpio(kButtonPins[0]) && is_rtc_gpio(kButtonPins[1]) &&
              is_rtc_gpio(kButtonPins[2]) && is_rtc_gpio(kButtonPins[3]));
static_assert(is_rtc_gpio(kAccelInt1) && is_rtc_gpio(kUsbDetect));
/// BACK+UP held ~4 s resets the chip (SR2 smart-reset IC) [R1 s6]; never use it as a UI chord.
inline constexpr std::uint8_t kResetChordMask = (1U << 1U) | (1U << 2U);

static_assert(!is_strapping(kVibration) && !is_strapping(kEpdCs) && !is_strapping(kEpdDc) &&
                  !is_strapping(kEpdReset) && !is_strapping(kSpiMosi) && !is_strapping(kSpiSck),
              "outputs must not sit on strapping pins");

} // namespace qz::board
