// FakeBoardIo: scriptable buttons, USB/charge pins, battery ADC pin and vibration motor.
#include "qz/core/assert.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/testkit/fakes.hpp"

#include <cstdint>

namespace qz::testkit {
namespace {

constexpr std::uint8_t kAllButtons =
    hal::kButtonBitMenu | hal::kButtonBitBack | hal::kButtonBitUp | hal::kButtonBitDown;
constexpr std::int64_t kUsPerMs = 1000;

} // namespace

std::uint8_t FakeBoardIo::pressed_buttons() const {
    return pressed_;
}

std::uint8_t FakeBoardIo::take_latched_buttons() {
    const std::uint8_t latched = latched_;
    latched_ = 0;
    return latched;
}

bool FakeBoardIo::usb_present() const {
    return usb_;
}

bool FakeBoardIo::charging() const {
    return charging_;
}

void FakeBoardIo::set_vibration(bool on) {
    if (on == vibrating_) {
        return; // only edges matter: a repeated "on" neither restarts nor double-counts a pulse
    }
    const std::int64_t now = now_us();
    vibrating_ = on;
    if (on) {
        vibration_started_us_ = now;
        ++vibration_pulses_;
    } else {
        vibration_total_us_ += now - vibration_started_us_;
    }
}

Result<std::uint16_t> FakeBoardIo::read_pin_mv() {
    ++adc_reads_;
    if (adc_fails_) {
        return Errc::kIo;
    }
    return pin_mv_;
}

void FakeBoardIo::press(std::uint8_t mask) {
    QZ_ASSERT((mask & ~kAllButtons) == 0);
    pressed_ = static_cast<std::uint8_t>(pressed_ | mask);
    latched_ = static_cast<std::uint8_t>(latched_ | mask);
}

void FakeBoardIo::release(std::uint8_t mask) {
    QZ_ASSERT((mask & ~kAllButtons) == 0);
    pressed_ = static_cast<std::uint8_t>(pressed_ & ~mask);
}

void FakeBoardIo::set_usb(bool present, bool is_charging) {
    QZ_ASSERT(present || !is_charging); // a cell cannot charge without VBUS
    usb_ = present;
    charging_ = is_charging;
}

void FakeBoardIo::set_pin_mv(std::uint16_t mv) {
    pin_mv_ = mv;
}

std::uint32_t FakeBoardIo::vibration_ms_total() const {
    std::int64_t total_us = vibration_total_us_;
    if (vibrating_) {
        total_us += now_us() - vibration_started_us_;
    }
    return static_cast<std::uint32_t>(total_us / kUsPerMs);
}

bool FakeBoardIo::vibrating() const {
    return vibrating_;
}

std::uint32_t FakeBoardIo::vibration_pulses() const {
    return vibration_pulses_;
}

void FakeBoardIo::fail_adc(bool fail) {
    adc_fails_ = fail;
}

std::uint32_t FakeBoardIo::adc_reads() const {
    return adc_reads_;
}

std::int64_t FakeBoardIo::now_us() const {
    return clock_ != nullptr ? clock_->elapsed_us() : 0;
}

} // namespace qz::testkit
