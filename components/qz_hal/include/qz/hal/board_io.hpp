// Board-level digital I/O with polarity already resolved by the implementation (qz_board).
#pragma once

#include "qz/core/result.hpp"

#include <cstdint>

namespace qz::hal {

/// Button bit positions in masks; model::Button uses the same values.
inline constexpr std::uint8_t kButtonBitMenu = 1U << 0U;
inline constexpr std::uint8_t kButtonBitBack = 1U << 1U;
inline constexpr std::uint8_t kButtonBitUp = 1U << 2U;
inline constexpr std::uint8_t kButtonBitDown = 1U << 3U;

/// Implemented by qz_platform (GPIO/RTC-GPIO) and qz_testkit (scriptable). Not thread-safe
/// except pressed_buttons()/usb_present(), which only read pin levels.
class BoardIo {
public:
    virtual ~BoardIo() = default;
    /// Currently pressed buttons (bit per kButtonBit*), true = pressed.
    [[nodiscard]] virtual std::uint8_t pressed_buttons() const = 0;
    /// Buttons seen pressed since the last call (bit per kButtonBit*), then cleared. Catches taps
    /// that start and end between two pressed_buttons() samples, e.g. during a panel refresh.
    [[nodiscard]] virtual std::uint8_t take_latched_buttons() = 0;
    /// USB VBUS detected (USB detect pin).
    [[nodiscard]] virtual bool usb_present() const = 0;
    /// STAT pin (GPIO10). HIGH whenever USB is present, charging or full [R1 s5]: diagnostics only.
    [[nodiscard]] virtual bool charging() const = 0;
    /// Vibration motor on/off. Implementations guarantee off before deep sleep.
    virtual void set_vibration(bool on) = 0;
};

/// Calibrated ADC channel for the battery sense pin.
class Adc {
public:
    virtual ~Adc() = default;
    /// One calibrated reading at the pin in millivolts (esp_adc oneshot + calibration on target).
    /// Divider scaling is NOT applied here (power service applies the board divider).
    virtual Result<std::uint16_t> read_pin_mv() = 0;
};

} // namespace qz::hal
