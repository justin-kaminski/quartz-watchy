// SleepControl: wake-source arming, deep-sleep pin parking/holds, light sleep with GPIO wake.
// Pin states and rationale: ARCHITECTURE.md section 5 (table "Pin states during deep sleep").
// Nothing here has run on hardware: HARDWARE_BRINGUP.md B4 (wake/vibration/GPIO0), B8 (timer),
// B9 (sleep floor) confirm the [ASSUMED] items listed in docs/STATUS.md.
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_impl.hpp"

#include <algorithm>
#include <array>

namespace qz::platform {
namespace {

using board::Level;

constexpr gpio_num_t gpio(std::uint8_t pin) noexcept {
    return static_cast<gpio_num_t>(pin);
}

constexpr std::uint64_t pin_bit(std::uint8_t pin) noexcept {
    return std::uint64_t{1} << pin;
}

constexpr std::uint32_t level_value(Level level) noexcept {
    return level == Level::kHigh ? 1U : 0U;
}

/// Fallback timer wake used only if arming a wake source failed, so the watch can never sleep
/// with no way back (battery-only device, no recovery without the reset chord) [TUNE].
constexpr std::uint64_t kFallbackWakeUs = 60ULL * 1000ULL * 1000ULL;

/// Output pad parked during deep sleep: pin and the level it is held at.
struct ParkedOutput {
    std::uint8_t pin;
    std::uint32_t level;
};

/// Vibration: output low + RTC pad hold. GPIO17 is an RTC pad (0-21): gpio_hold_en routes to
/// rtc_gpio_hold_en [IDF:components/esp_driver_gpio/src/gpio.c gpio_hold_en]. Active-high NPN
/// without base pull-down: floating = motor may run [R1 s9].
constexpr ParkedOutput kVibrationPark{board::kVibration, 0};

/// EPD pads are digital (33-36, 47, 48 are not RTC pads): CS/RST high (deselected, RES# has its own
/// pull-up), DC/MOSI/SCK low. Digital pad holds exist for GPIO22+ on ESP32-S3
/// [IDF:components/soc/esp32s3/gpio_periph.c GPIO_HOLD_MASK].
constexpr std::array<ParkedOutput, 5> kEpdPark = {{
    {board::kEpdCs, 1},
    {board::kEpdReset, 1},
    {board::kEpdDc, 0},
    {board::kSpiMosi, 0},
    {board::kSpiSck, 0},
}};

/// RTC pads with no external driver that must not float into an enabled input buffer: INT2 is
/// never configured on the BMA423 (no board pull [R1 s7]); INT1 is isolated unless it is a wake
/// source; battery ADC divider sits at 2-3 V mid-rail and would shoot through an input buffer.
constexpr std::uint8_t kIsolateAlways[] = {board::kBatteryAdc, board::kAccelInt2};

constexpr std::array<std::uint8_t, 4> kButtonPins = board::kButtonPins;

static_assert(board::is_rtc_gpio(board::kVibration) && board::is_rtc_gpio(board::kBatteryAdc) &&
                  board::is_rtc_gpio(board::kAccelInt1) && board::is_rtc_gpio(board::kAccelInt2),
              "isolate/hold helpers below are RTC-pad APIs");
static_assert(!board::is_rtc_gpio(board::kEpdCs) && !board::is_rtc_gpio(board::kEpdDc) &&
                  !board::is_rtc_gpio(board::kEpdReset) && !board::is_rtc_gpio(board::kEpdBusy) &&
                  !board::is_rtc_gpio(board::kSpiMosi) && !board::is_rtc_gpio(board::kSpiSck),
              "EPD pads use the digital hold path");

bool at_level(std::uint8_t pin, Level level) noexcept {
    return static_cast<std::uint32_t>(gpio_get_level(gpio(pin))) == level_value(level);
}

/// Plain input without internal pulls: buttons/INT have external pull-ups or push-pull drivers
/// [R1 s6, s7], USB detect and STAT are resistor networks [R1 s5]. NEVER pulls on GPIO0 (ARCH s5).
/// For RTC pads gpio_config -> gpio_pullup_dis/pulldown_dis -> rtc_gpio_pullup_dis/pulldown_dis, so
/// the RTC pull registers (the ones that matter in sleep) are cleared too
/// [IDF:components/esp_driver_gpio/src/gpio.c gpio_pullup_dis].
void configure_inputs(std::uint64_t mask) noexcept {
    if (mask == 0) {
        return;
    }
    gpio_config_t cfg{};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    // Only fails on an invalid mask; all masks here come from compile-time pin constants.
    (void)gpio_config(&cfg);
}

constexpr esp_sleep_ext1_wakeup_mode_t ext1_mode(Level level) noexcept {
    // [IDF:components/esp_hw_support/include/esp_sleep.h esp_sleep_ext1_wakeup_mode_t]: ESP32-S3
    // has ANY_LOW and ANY_HIGH only, one mode for the whole mask.
    return level == Level::kHigh ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW;
}

/// EXT0/EXT1 plan after applying the SleepPlan and the safety exclusions.
struct DeepWakeArm {
    std::uint64_t ext1_mask = 0;
    Level ext1_level = Level::kLow;
    bool ext0 = false;
    std::uint8_t ext0_gpio = 0;
    Level ext0_level = Level::kHigh;
};

/// Derives the arm set from board::wake_config and the plan. Pins that are ALREADY at their wake
/// level are excluded: both EXT0/EXT1 are level sensed, so arming them would re-wake the chip
/// immediately (a held button; USB already present; a latched, un-cleared accelerometer INT).
/// ARCHITECTURE.md section 5: "the app waits for release (or excludes held pins from the mask)".
DeepWakeArm build_deep_arm(const hal::SleepPlan& plan) noexcept {
    const board::WakeConfig cfg = board::wake_config(plan.wake_on_accel, plan.wake_on_usb);
    std::uint64_t candidates = cfg.ext1_mask;
    if (!plan.wake_on_buttons) {
        for (const std::uint8_t pin : kButtonPins) {
            candidates &= ~pin_bit(pin);
        }
    }
    // Inputs must be configured before they are sampled (GPIO0 etc. were set by BoardIo::init,
    // INT1 and USB may not have been).
    configure_inputs(candidates);
    if (cfg.ext0_enabled) {
        configure_inputs(pin_bit(cfg.ext0_gpio));
    }

    DeepWakeArm arm{};
    arm.ext1_level = cfg.ext1_level;
    for (std::uint8_t pin = 0; pin < 64 && candidates != 0; ++pin) {
        const std::uint64_t bit = pin_bit(pin);
        if ((candidates & bit) == 0) {
            continue;
        }
        candidates &= ~bit;
        if (!at_level(pin, cfg.ext1_level)) {
            arm.ext1_mask |= bit;
        }
    }
    if (cfg.ext0_enabled && !at_level(cfg.ext0_gpio, cfg.ext0_level)) {
        arm.ext0 = true;
        arm.ext0_gpio = cfg.ext0_gpio;
        arm.ext0_level = cfg.ext0_level;
    }
    return arm;
}

/// Holds one parked output: level first, then direction (output), then the hold. The hold freezes
/// the pad immediately, so this must be the LAST thing done to the pin before sleep.
void park_output(const ParkedOutput& p) noexcept {
    // Order: write the output register while the matrix may still route a peripheral signal to the
    // pad (the register has no effect until the pad is switched to plain GPIO), then switch the pad
    // to a simple GPIO output; gpio_output_enable resets the matrix routing to the GPIO output
    // signal [IDF:components/esp_driver_gpio/src/gpio.c gpio_output_enable]. This avoids a glitch
    // on pads the SPI driver owned (it leaves its idle level == our parked level: CS high, SCK/MOSI
    // low).
    (void)gpio_set_level(gpio(p.pin), p.level);
    (void)gpio_set_direction(gpio(p.pin), GPIO_MODE_OUTPUT);
    (void)gpio_hold_en(gpio(p.pin));
}

} // namespace

Status IdfSleep::release_holds() noexcept {
    esp_err_t first = ESP_OK;
    const auto note = [&first](esp_err_t err) {
        if (first == ESP_OK) {
            first = err;
        }
    };
    // Outputs: re-establish the parked level as a plain output while the hold still latches the pad
    // (gpio_hold_dis docs: otherwise the pad falls back to its default level) then drop the hold
    // [IDF:components/esp_driver_gpio/include/driver/gpio.h gpio_hold_dis].
    const auto release_output = [&note](const ParkedOutput& p) {
        note(gpio_set_level(gpio(p.pin), p.level));
        note(gpio_set_direction(gpio(p.pin), GPIO_MODE_OUTPUT));
        note(gpio_hold_dis(gpio(p.pin)));
    };
    release_output(kVibrationPark);
    for (const ParkedOutput& p : kEpdPark) {
        release_output(p);
    }
    // Clears RTC_CNTL DG_PAD_AUTOHOLD_EN [IDF:components/esp_hal_gpio/esp32s3/include/hal/gpio_ll.h
    // _gpio_ll_deep_sleep_hold_dis]. Per-pad digital holds were dropped above.
    gpio_deep_sleep_hold_dis();

    // Battery ADC pad: back to analog (input buffer off, no pulls) while still isolated, then
    // unhold.
    // (gpio_config_as_analog is an esp_private API, so the public equivalent is spelled out:
    // [IDF:components/esp_driver_gpio/src/gpio.c gpio_config_as_analog] = input/output disable + no
    // pulls.)
    note(gpio_set_direction(gpio(board::kBatteryAdc), GPIO_MODE_DISABLE));
    note(gpio_pullup_dis(gpio(board::kBatteryAdc)));
    note(gpio_pulldown_dis(gpio(board::kBatteryAdc)));
    note(rtc_gpio_hold_dis(gpio(board::kBatteryAdc)));
    // INT2 / INT1: unhold, then plain inputs without pulls (INT1 is driven push-pull by the
    // BMA423).
    note(rtc_gpio_hold_dis(gpio(board::kAccelInt2)));
    note(rtc_gpio_hold_dis(gpio(board::kAccelInt1)));
    configure_inputs(pin_bit(board::kAccelInt1) | pin_bit(board::kAccelInt2));
    // EXT1/EXT0 pad holds (buttons, INT1, USB) are released by the IDF at startup after a
    // deep-sleep wake [IDF:components/esp_hw_support/sleep_gpio.c esp_deep_sleep_wakeup_io_reset,
    // docs/en/api-reference/system/sleep_modes.rst "For Deep-sleep wakeup, this is already being
    // handled at the application startup stage"]. [ASSUMED] confirm in B4 (all four buttons work
    // after the first deep-sleep wake).
    return first == ESP_OK ? Status{} : Status{to_error(first)};
}

void IdfSleep::deep_sleep(const hal::SleepPlan& plan) {
    // ---- 1. wake sources --------------------------------------------------------------------
    // Clear anything armed earlier (e.g. a previous light sleep). ESP_SLEEP_WAKEUP_ALL only clears
    // the trigger bits; the EXT1 mask is cleared separately
    // [IDF:components/esp_hw_support/sleep_modes.c esp_sleep_disable_wakeup_source /
    // _ext1_wakeup_io].
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    (void)esp_sleep_disable_ext1_wakeup_io(0);

    const DeepWakeArm arm = build_deep_arm(plan);
    bool armed_ok = true;
    bool timer_armed = false;
    if (arm.ext1_mask != 0) {
        armed_ok &=
            esp_sleep_enable_ext1_wakeup_io(arm.ext1_mask, ext1_mode(arm.ext1_level)) == ESP_OK;
    }
    if (arm.ext0) {
        // EXT0 forces RTC_PERIPH on (RTC pull/pad state kept) [IDF:sleep_modes.c ~line 2906].
        armed_ok &=
            esp_sleep_enable_ext0_wakeup(gpio(arm.ext0_gpio),
                                         static_cast<int>(level_value(arm.ext0_level))) == ESP_OK;
    }
    if (plan.timer_us >= 0) {
        const auto us = static_cast<std::uint64_t>(std::max(plan.timer_us, kMinDeepSleepTimerUs));
        const bool ok = esp_sleep_enable_timer_wakeup(us) == ESP_OK;
        armed_ok &= ok;
        timer_armed = ok;
    }
    if (!armed_ok && !timer_armed) {
        (void)esp_sleep_enable_timer_wakeup(kFallbackWakeUs);
    }

    // ---- 2. pin parking (ARCHITECTURE.md section 5 table) -----------------------------------
    // Buttons, USB detect, INT1 (when armed): RTC inputs without pulls, configured above; the IDF
    // routes them to RTC and latches them at sleep entry [IDF:sleep_modes.c ext1_wakeup_prepare].
    // GPIO0: only ever an input here; no pull of any kind is enabled.
    // I2C 11/12, STAT 10: plain inputs, no pulls, externally driven. BUSY 36: input, no pull; the
    // IDF isolates every un-held digital pad when the deep-sleep autohold is on
    // [IDF:sleep_gpio.c esp_sleep_isolate_digital_gpio] ([ASSUMED] sleep floor, B9).
    // GPIO46 is never touched by Quartz (the IDF isolation loop may still disable its default
    // pull).
    configure_inputs(pin_bit(board::kI2cSda) | pin_bit(board::kI2cScl) |
                     pin_bit(board::kChargeStatus));

    for (const std::uint8_t pin : kIsolateAlways) {
        (void)rtc_gpio_isolate(gpio(pin)); // input/output/pulls off + hold [IDF rtc_io.h]
    }
    if ((arm.ext1_mask & pin_bit(board::kAccelInt1)) == 0) {
        (void)rtc_gpio_isolate(gpio(board::kAccelInt1));
    }

    // Outputs last: the holds freeze the pads.
    park_output(kVibrationPark);
    for (const ParkedOutput& p : kEpdPark) {
        park_output(p);
    }
    gpio_deep_sleep_hold_en(); // digital pad holds persist through deep sleep [IDF gpio_ll.h]

    // ---- 3. sleep ---------------------------------------------------------------------------
    // Does not return unless the request is rejected (wake source already pending).
    (void)esp_deep_sleep_try_to_start();

    // Rejected: undo everything that was latched and restart cleanly (reset reason = software;
    // the next boot re-runs the full init and re-plans). Never continue with parked pads.
    (void)release_holds();
    esp_restart();
}

namespace {

/// One light-sleep wake candidate.
struct LightWake {
    std::uint8_t pin;
    Level wake_level;
    hal::LightSleepWake cause;
};

constexpr std::size_t kMaxLightWakes = 7; // 4 buttons + INT1 + USB + BUSY

struct LightArm {
    std::array<LightWake, kMaxLightWakes> wakes{};
    std::size_t count = 0;
    bool epd_idle_now = false; ///< BUSY already low and the plan asked for it
    void add(std::uint8_t pin, Level level, hal::LightSleepWake cause) noexcept {
        wakes[count++] = LightWake{pin, level, cause};
    }
};

/// Same exclusion rule as deep sleep: only pins currently NOT at their wake level are armed, so a
/// held button / present USB / asserted INT cannot cause a wake loop. BUSY is armed on its low
/// level (idle) only while it is high; if it is already low the wait is over.
LightArm build_light_arm(const hal::SleepPlan& plan) noexcept {
    LightArm arm{};
    std::uint64_t inputs = 0;
    if (plan.wake_on_buttons) {
        for (const std::uint8_t pin : kButtonPins) {
            inputs |= pin_bit(pin);
        }
    }
    if (plan.wake_on_accel) {
        inputs |= pin_bit(board::kAccelInt1);
    }
    if (plan.wake_on_usb) {
        inputs |= pin_bit(board::kUsbDetect);
    }
    if (plan.wake_on_epd_idle) {
        inputs |= pin_bit(board::kEpdBusy);
    }
    configure_inputs(inputs);

    if (plan.wake_on_epd_idle) {
        // SSD1681 BUSY: HIGH = busy [R1 s8, ssd1681.md s2].
        if (at_level(board::kEpdBusy, Level::kLow)) {
            arm.epd_idle_now = true;
        } else {
            arm.add(board::kEpdBusy, Level::kLow, hal::LightSleepWake::kEpdIdle);
        }
    }
    if (plan.wake_on_buttons) {
        for (const std::uint8_t pin : kButtonPins) {
            if (!at_level(pin, board::kButtonActive)) {
                arm.add(pin, board::kButtonActive, hal::LightSleepWake::kButton);
            }
        }
    }
    if (plan.wake_on_accel && !at_level(board::kAccelInt1, board::kAccelIntActive)) {
        arm.add(board::kAccelInt1, board::kAccelIntActive, hal::LightSleepWake::kAccel);
    }
    if (plan.wake_on_usb && !at_level(board::kUsbDetect, board::kUsbDetectActive)) {
        arm.add(board::kUsbDetect, board::kUsbDetectActive, hal::LightSleepWake::kUsb);
    }
    return arm;
}

/// Which armed pin is at its wake level now; kOther if none (e.g. the pulse ended before sampling).
hal::LightSleepWake classify_gpio(const LightArm& arm) noexcept {
    for (std::size_t i = 0; i < arm.count; ++i) {
        if (at_level(arm.wakes[i].pin, arm.wakes[i].wake_level)) {
            return arm.wakes[i].cause;
        }
    }
    return hal::LightSleepWake::kOther;
}

/// Polling wait (tethered, or sleeps too short for the sleep entry overhead): 1 tick granularity.
hal::LightSleepWake poll_until(std::int64_t deadline_us, bool has_deadline, const LightArm& arm) {
    for (;;) {
        const hal::LightSleepWake gpio_cause = classify_gpio(arm);
        if (gpio_cause != hal::LightSleepWake::kOther) {
            return gpio_cause;
        }
        if (has_deadline && esp_rtc_get_time_us() >= static_cast<std::int64_t>(deadline_us)) {
            return hal::LightSleepWake::kTimer;
        }
        vTaskDelay(1);
    }
}

constexpr std::uint32_t cause_bit(esp_sleep_source_t source) noexcept {
    return std::uint32_t{1} << static_cast<std::uint32_t>(source);
}

constexpr gpio_int_type_t wake_intr(Level level) noexcept {
    return level == Level::kHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL;
}

} // namespace

hal::LightSleepWake IdfSleep::light_sleep(const hal::SleepPlan& plan) {
    if (!outputs_kept_in_light_sleep_) {
        // CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=y (ESP32-S3 default, verified in the generated
        // sdkconfig) isolates every GPIO in light sleep through the SLP_SEL bit
        // [IDF:components/esp_hw_support/sleep_gpio.c esp_sleep_config_gpio_isolate /
        // esp_sleep_enable_gpio_switch]. Our driven outputs must keep their active configuration:
        // GPIO17 floating would let the motor run; CS/DC/RST/SCK/MOSI floating mid-waveform is
        // undefined for the panel. gpio_sleep_sel_dis = keep the normal-mode pad config in sleep
        // [IDF:components/esp_driver_gpio/src/gpio.c gpio_sleep_sel_dis]. [ASSUMED] the SPI
        // peripheral keeps CS/SCK/MOSI at idle levels while clock-gated in light sleep (B5, B9).
        (void)gpio_sleep_sel_dis(gpio(kVibrationPark.pin));
        for (const ParkedOutput& p : kEpdPark) {
            (void)gpio_sleep_sel_dis(gpio(p.pin));
        }
        outputs_kept_in_light_sleep_ = true;
    }

    const LightArm arm = build_light_arm(plan);
    if (arm.epd_idle_now) {
        return hal::LightSleepWake::kEpdIdle;
    }
    const bool has_timer = plan.timer_us >= 0;
    if (arm.count == 0 && !has_timer) {
        return hal::LightSleepWake::kOther; // nothing could ever wake us: never block forever
    }
    const std::int64_t deadline_us = has_timer ? esp_rtc_get_time_us() + plan.timer_us : 0;

    const bool poll_only = tethered_ || (has_timer && plan.timer_us < kMinLightSleepUs);
    if (poll_only) {
        return poll_until(deadline_us, has_timer, arm);
    }

    // GPIO wake in light sleep: gpio_wakeup_enable(level) per pin + esp_sleep_enable_gpio_wakeup()
    // [IDF:components/esp_driver_gpio/src/gpio.c gpio_wakeup_enable; esp_sleep.h
    // esp_sleep_enable_gpio_wakeup]. It works for RTC and digital pads (BUSY is GPIO36) and keeps
    // the pad's active config (sleep_sel_dis inside) so no pad holds are created, unlike EXT1 which
    // latches holds that must be released after a light sleep. CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_
    // LIGHT_SLEEP is not set (it would make this API unavailable) [IDF:esp_pm/Kconfig].
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    for (std::size_t i = 0; i < arm.count; ++i) {
        (void)gpio_wakeup_enable(gpio(arm.wakes[i].pin), wake_intr(arm.wakes[i].wake_level));
    }
    if (arm.count != 0) {
        (void)esp_sleep_enable_gpio_wakeup();
    }
    if (has_timer) {
        (void)esp_sleep_enable_timer_wakeup(static_cast<std::uint64_t>(plan.timer_us));
    }

    const esp_err_t err = esp_light_sleep_start();
    const std::uint32_t causes = esp_sleep_get_wakeup_causes();

    for (std::size_t i = 0; i < arm.count; ++i) {
        (void)gpio_wakeup_disable(gpio(arm.wakes[i].pin)); // also re-enables SLP_SEL isolation
    }
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    if (err == ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION) {
        // Remaining time below the chip's minimum sleep: finish by polling.
        return poll_until(deadline_us, has_timer, arm);
    }
    // ESP_ERR_SLEEP_REJECT (a GPIO level already satisfied) and ESP_OK are both classified by the
    // pin levels; sampling after wake can miss a pulse shorter than the wake latency -> kOther.
    const hal::LightSleepWake gpio_cause = classify_gpio(arm);
    if (gpio_cause != hal::LightSleepWake::kOther) {
        return gpio_cause;
    }
    if ((causes & cause_bit(ESP_SLEEP_WAKEUP_TIMER)) != 0) {
        return hal::LightSleepWake::kTimer;
    }
    return hal::LightSleepWake::kOther;
}

} // namespace qz::platform
