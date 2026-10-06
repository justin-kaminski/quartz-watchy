// EpdBus over SPI2 (write only) + GPIO DC/RST/BUSY. Pins: qz_board (watchy_v3.hpp); protocol and
// timings: docs/research/ssd1681.md s2 (R1). Nothing here has run on hardware (HARDWARE_BRINGUP.md
// B5).
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rtc_time.h"
#include "freertos/FreeRTOS.h"
#include "platform_impl.hpp"

#include <algorithm>

namespace qz::platform {
namespace {

constexpr spi_host_device_t kSpiHost = SPI2_HOST; // [IDF:components/hal/include/hal/spi_types.h]

constexpr gpio_num_t gpio(std::uint8_t pin) noexcept {
    return static_cast<gpio_num_t>(pin);
}

/// Bounce buffer in internal DRAM (.bss is internal): the SPI DMA cannot read flash (.rodata) or
/// RTC slow memory (the frame shadow lives there), and the driver would otherwise malloc a
/// temporary DMA buffer per transfer when handed a non-DMA-capable pointer
/// [IDF:components/esp_driver_spi/include/driver/spi_master.h SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL].
/// One IdfEpdBus instance exists (platform singleton), so the buffer is shared static storage.
alignas(4) std::uint8_t s_bounce[IdfEpdBus::kChunkBytes];

} // namespace

IdfEpdBus::~IdfEpdBus() {
    if (dev_ != nullptr) {
        (void)spi_bus_remove_device(dev_); // failure cannot be reported from a destructor
    }
    if (bus_ready_) {
        (void)spi_bus_free(kSpiHost);
    }
}

Status IdfEpdBus::init() noexcept {
    // Pads held through deep sleep must be released (and re-driven at the held level) BEFORE the
    // SPI driver claims them; also leaves RST high and DC low as plain outputs.
    if (const Status s = IdfSleep::release_holds(); !s) {
        return s;
    }
    esp_err_t err = gpio_set_level(gpio(board::kEpdReset), 1);
    if (err != ESP_OK) {
        return to_error(err);
    }
    err = gpio_set_level(gpio(board::kEpdDc), 0);
    if (err != ESP_OK) {
        return to_error(err);
    }
    // BUSY: input, no pull (the panel drives it) [ARCH s5].
    gpio_config_t busy_cfg{};
    busy_cfg.pin_bit_mask = std::uint64_t{1} << board::kEpdBusy;
    busy_cfg.mode = GPIO_MODE_INPUT;
    busy_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    busy_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    busy_cfg.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&busy_cfg);
    if (err != ESP_OK) {
        return to_error(err);
    }

    spi_bus_config_t bus{};
    bus.mosi_io_num = board::kSpiMosi;
    bus.miso_io_num = -1; // GPIO46 is a no-connect strapping pin: never configured [R1 s6]
    bus.sclk_io_num = board::kSpiSck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data_io_default_level = false; // MOSI idles low, matches the deep-sleep park level
    bus.max_transfer_sz = static_cast<int>(kChunkBytes);
    bus.flags = SPICOMMON_BUSFLAG_MASTER;
    // SPI_DMA_CH_AUTO [IDF:components/esp_driver_spi/include/driver/spi_common.h]
    err = spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return to_error(err);
    }
    bus_ready_ = true;

    spi_device_interface_config_t dev{};
    dev.mode = 0; // CPOL 0 / CPHA 0 [ssd1681.md s2]
    dev.clock_speed_hz = static_cast<int>(board::kSpiHz);
    dev.spics_io_num = board::kEpdCs; // hardware CS: one frame per command()/data() call
    dev.queue_size = 1;
    // Transmit only; half duplex + NO_DUMMY because there is no MISO phase (no timing dummy bits).
    dev.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
    err = spi_bus_add_device(kSpiHost, &dev, &dev_);
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Status IdfEpdBus::hardware_reset() {
    // RES# low >= 10 ms, then high and >= 10 ms before the next command [ASSUMED, ssd1681.md s2:
    // pulse width and post-reset time unspecified; generous values]. RES# also has a 100 k pull-up.
    esp_err_t err = gpio_set_level(gpio(board::kEpdReset), 0);
    if (err != ESP_OK) {
        return to_error(err);
    }
    delay_.delay_ms(kResetLowMs);
    err = gpio_set_level(gpio(board::kEpdReset), 1);
    if (err != ESP_OK) {
        return to_error(err);
    }
    delay_.delay_ms(kResetHighMs);
    return Status{};
}

Status IdfEpdBus::transfer(bool dc_high, std::span<const std::uint8_t> bytes) {
    if (dev_ == nullptr) {
        return Error{Errc::kInvalidState};
    }
    if (bytes.empty()) {
        return Status{};
    }
    // DC is stable for the whole CS frame; it only changes while CS is high (between calls).
    esp_err_t err = gpio_set_level(gpio(board::kEpdDc), dc_high ? 1U : 0U);
    if (err != ESP_OK) {
        return to_error(err);
    }
    // One CS frame for the whole call: acquire the bus and keep CS active across chunks
    // [IDF:components/esp_driver_spi/include/driver/spi_master.h SPI_TRANS_CS_KEEP_ACTIVE requires
    // spi_device_acquire_bus; wait must be portMAX_DELAY].
    err = spi_device_acquire_bus(dev_, portMAX_DELAY);
    if (err != ESP_OK) {
        return to_error(err);
    }
    std::size_t offset = 0;
    while (offset < bytes.size() && err == ESP_OK) {
        const std::size_t n = std::min(kChunkBytes, bytes.size() - offset);
        std::copy_n(bytes.data() + offset, n, s_bounce);
        spi_transaction_t trans{};
        trans.length = n * 8U; // bits
        trans.tx_buffer = s_bounce;
        offset += n;
        if (offset < bytes.size()) {
            trans.flags = SPI_TRANS_CS_KEEP_ACTIVE; // more chunks follow in this frame
        }
        err = spi_device_polling_transmit(dev_, &trans);
    }
    spi_device_release_bus(dev_);
    return err == ESP_OK ? Status{} : Status{to_error(err)};
}

Status IdfEpdBus::command(std::uint8_t cmd) {
    return transfer(false, std::span<const std::uint8_t>(&cmd, 1));
}

Status IdfEpdBus::data(std::span<const std::uint8_t> bytes) {
    return transfer(true, bytes);
}

Status IdfEpdBus::read(std::span<std::uint8_t> /*out*/) {
    return Error{Errc::kUnsupported};
}

bool IdfEpdBus::busy() const {
    return gpio_get_level(gpio(board::kEpdBusy)) != 0; // HIGH = busy [ssd1681.md s2]
}

Status IdfEpdBus::wait_idle(std::uint32_t timeout_ms) {
    const std::int64_t deadline_us =
        esp_rtc_get_time_us() + static_cast<std::int64_t>(timeout_ms) * 1000;
    hal::SleepPlan plan{};
    plan.wake_on_buttons = false; // a press during the waveform is picked up after the update
    plan.wake_on_accel = false;
    plan.wake_on_usb = false;
    plan.wake_on_epd_idle = true;
    while (busy()) {
        const std::int64_t remaining_us = deadline_us - esp_rtc_get_time_us();
        if (remaining_us <= 0) {
            return Error{Errc::kTimeout};
        }
        // Light sleep with a BUSY-low GPIO wake (battery) / polling (tethered), bounded by the
        // remaining time. The loop re-checks busy(): spurious or early wakes just sleep again.
        plan.timer_us = remaining_us;
        (void)sleep_.light_sleep(plan); // the cause is irrelevant: busy() decides
    }
    return Status{};
}

} // namespace qz::platform
