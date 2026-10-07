// BoardIo over GPIO. Pin numbers, polarities and the pin-state rules come from qz_board
// (watchy_v3.hpp) and ARCHITECTURE.md section 5.
#include "driver/gpio.h"
#include "platform_impl.hpp"

#include <array>

namespace qz::platform {
namespace {

using board::Level;

/// Vibration motor is an active-high NPN drive without base pull-down [R1 s9]: HIGH = running.
/// [ASSUMED] confirm in HARDWARE_BRINGUP.md (vibration step).
constexpr std::uint32_t kVibrationOnLevel = 1;
constexpr std::uint32_t kVibrationOffLevel = 0;

constexpr gpio_num_t kVibrationGpio = static_cast<gpio_num_t>(board::kVibration);

constexpr std::uint64_t pin_bit(std::uint8_t gpio) noexcept {
    return std::uint64_t{1} << gpio;
}

constexpr std::uint32_t level_value(Level level) noexcept {
    return level == Level::kHigh ? 1U : 0U;
}

/// Plain input, no internal pulls: buttons have external 100 k pull-ups [R1 s6], USB detect and
/// charge status are driven by resistor networks [R1 s5]. ARCHITECTURE.md section 5 forbids
/// internal pulls on GPIO0 (strapping).
Status configure_inputs(std::uint64_t mask) noexcept {
    gpio_config_t cfg{};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    // gpio_config also deinitializes the RTC mux of RTC-capable pads, handing them back to the
    // digital GPIO matrix [IDF:components/esp_driver_gpio/src/gpio.c gpio_config].
    const esp_err_t err = gpio_config(&cfg);
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Status configure_vibration_off() noexcept {
    // Order matters (glitch-free release of a deep-sleep pad hold) [IDF:esp_driver_gpio/include/
    // driver/gpio.h gpio_hold_dis docs]: make the pad an output at the known level while it is
    // still held, then drop the hold. GPIO17 is an RTC pad; gpio_hold_dis routes to
    // rtc_gpio_hold_dis [IDF:components/esp_driver_gpio/src/gpio.c gpio_hold_dis].
    gpio_config_t cfg{};
    cfg.pin_bit_mask = pin_bit(board::kVibration);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return to_error(err);
    }
    err = gpio_set_level(kVibrationGpio, kVibrationOffLevel);
    if (err != ESP_OK) {
        return to_error(err);
    }
    err = gpio_hold_dis(kVibrationGpio);
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

} // namespace

IdfBoardIo::~IdfBoardIo() {
    // Never leave the motor running if the object goes away. Failure cannot be reported from a
    // destructor and the pad stays at its last level, so the result is intentionally ignored.
    (void)gpio_set_level(kVibrationGpio, kVibrationOffLevel);
}

Status IdfBoardIo::init() noexcept {
    // Vibration first: floating = motor may run (ARCHITECTURE.md section 5).
    if (const Status s = configure_vibration_off(); !s) {
        return s;
    }
    std::uint64_t inputs = pin_bit(board::kUsbDetect) | pin_bit(board::kChargeStatus);
    for (const std::uint8_t pin : board::kButtonPins) {
        inputs |= pin_bit(pin);
    }
    // [ASSUMED] pads of EXT1 wake pins carry no pad hold after wake (IDF only latches the
    // RTC mux config); verify with HARDWARE_BRINGUP.md B4.
    return configure_inputs(inputs);
}

std::uint8_t IdfBoardIo::pressed_buttons() const {
    std::uint8_t mask = 0;
    for (std::size_t i = 0; i < board::kButtonPins.size(); ++i) {
        const int level = gpio_get_level(static_cast<gpio_num_t>(board::kButtonPins[i]));
        if (static_cast<std::uint32_t>(level) == level_value(board::kButtonActive)) {
            mask = static_cast<std::uint8_t>(mask | (1U << i)); // bit order == kButtonBit*
        }
    }
    return mask;
}

std::uint8_t IdfBoardIo::take_latched_buttons() {
    const std::uint8_t latched = latched_;
    latched_ = 0;
    return latched;
}

void IdfBoardIo::latch_buttons() noexcept {
    latched_ = static_cast<std::uint8_t>(latched_ | pressed_buttons());
}

bool IdfBoardIo::usb_present() const {
    const int level = gpio_get_level(static_cast<gpio_num_t>(board::kUsbDetect));
    return static_cast<std::uint32_t>(level) == level_value(board::kUsbDetectActive);
}

bool IdfBoardIo::charging() const {
    const int level = gpio_get_level(static_cast<gpio_num_t>(board::kChargeStatus));
    return static_cast<std::uint32_t>(level) == level_value(board::kChargeStatusActive);
}

void IdfBoardIo::set_vibration(bool on) {
    // gpio_set_level only fails for an invalid pad; the pad is a compile-time constant.
    (void)gpio_set_level(kVibrationGpio, on ? kVibrationOnLevel : kVibrationOffLevel);
}

} // namespace qz::platform
