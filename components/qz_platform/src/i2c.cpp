// I2cDevice over the new `i2c_master` driver [IDF:components/esp_driver_i2c/include/driver/
// i2c_master.h]. BMA423 at 7-bit 0x18 (SDO to GND) [R1 s7]; the board has 10 k pull-ups on SDA/SCL
// so the internal pull-ups stay off. Nothing here has run on hardware (HARDWARE_BRINGUP.md B3).
#include "driver/i2c_master.h"
#include "platform_impl.hpp"

namespace qz::platform {
namespace {

/// Maps a driver error to a qz::Error per hal::I2cDevice: kTimeout on bus timeout, kIo otherwise
/// (NACK = ESP_ERR_INVALID_RESPONSE, bus error, ...). detail = low 16 bits of the esp_err_t.
Error map_i2c_error(esp_err_t err) noexcept {
    const auto detail = static_cast<std::uint16_t>(static_cast<std::uint32_t>(err) & 0xFFFFU);
    if (err == ESP_ERR_TIMEOUT) {
        return Error{Errc::kTimeout, detail};
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return Error{Errc::kBadArgs, detail};
    }
    return Error{Errc::kIo, detail};
}

} // namespace

IdfI2cDevice::~IdfI2cDevice() {
    // Failures cannot be reported from a destructor.
    if (dev_ != nullptr) {
        (void)i2c_master_bus_rm_device(dev_);
    }
    if (bus_ != nullptr) {
        (void)i2c_del_master_bus(bus_);
    }
}

Status IdfI2cDevice::init() noexcept {
    if (bus_ != nullptr) {
        return Error{Errc::kInvalidState};
    }
    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.i2c_port = -1; // auto-select a free HP I2C port
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(board::kI2cSda);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(board::kI2cScl);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7; // typical value [IDF:i2c_master.h i2c_master_bus_config_t]
    bus_cfg.intr_priority = 0;     // 0 = driver default
    bus_cfg.trans_queue_depth = 0; // synchronous transactions only
    bus_cfg.flags.enable_internal_pullup = 0; // external 10 k pull-ups [R1 s7]
    bus_cfg.flags.allow_pd = 0;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_);
    if (err != ESP_OK) {
        bus_ = nullptr;
        return map_i2c_error(err);
    }
    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = board::kBma423Address;
    dev_cfg.scl_speed_hz = board::kI2cHz;
    dev_cfg.scl_wait_us = 0;             // 0 = hardware default clock-stretch timeout
    dev_cfg.flags.disable_ack_check = 0; // NACK must surface as an error
    err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        dev_ = nullptr;
        (void)i2c_del_master_bus(bus_);
        bus_ = nullptr;
        return map_i2c_error(err);
    }
    return Status{};
}

Status IdfI2cDevice::finish(esp_err_t err) noexcept {
    if (err == ESP_OK) {
        return Status{};
    }
    if (err == ESP_ERR_TIMEOUT && bus_ != nullptr) {
        // A timed-out transaction can leave the bus/FSM wedged; reset it so the next call starts
        // clean [IDF:i2c_master.h i2c_master_bus_reset]. The reset result is secondary to the
        // timeout.
        (void)i2c_master_bus_reset(bus_);
    }
    return map_i2c_error(err);
}

Status IdfI2cDevice::read_registers(std::uint8_t reg, std::span<std::uint8_t> out) {
    if (dev_ == nullptr) {
        return Error{Errc::kInvalidState};
    }
    if (out.empty()) {
        return Status{}; // the driver rejects zero-length reads; nothing to read
    }
    // START, addr+W, reg, RESTART, addr+R, data... NACK last, STOP; the register address
    // auto-increments in the device [IDF:i2c_master.c i2c_master_transmit_receive].
    return finish(
        i2c_master_transmit_receive(dev_, &reg, 1, out.data(), out.size(), kXferTimeoutMs));
}

Status IdfI2cDevice::write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) {
    if (dev_ == nullptr) {
        return Error{Errc::kInvalidState};
    }
    if (data.size() > kMaxWriteBytes) {
        return Error{Errc::kBadArgs};
    }
    // Two buffers (register byte, payload) in ONE transaction: no copy and no length limit from the
    // 32-byte FIFO, the driver refills it from the ISR for long writes [IDF:components/
    // esp_driver_i2c/i2c_master.c i2c_master_multi_buffer_transmit; zero-size buffers are skipped].
    i2c_master_transmit_multi_buffer_info_t bufs[2] = {
        {.write_buffer = &reg, .buffer_size = 1},
        {.write_buffer = data.data(), .buffer_size = data.size()},
    };
    return finish(i2c_master_multi_buffer_transmit(dev_, bufs, 2, kXferTimeoutMs));
}

} // namespace qz::platform
