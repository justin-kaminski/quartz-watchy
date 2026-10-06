// Register-oriented I2C device (one 7-bit address). Implemented by qz_platform (IDF i2c_master,
// 400 kHz) and qz_testkit (FakeBma423 register model). Not thread-safe.
#pragma once

#include "qz/core/result.hpp"

#include <cstdint>
#include <span>

namespace qz::hal {

class I2cDevice {
public:
    virtual ~I2cDevice() = default;
    /// Burst read starting at `reg` (register address auto-increment by the device).
    virtual Status read_registers(std::uint8_t reg, std::span<std::uint8_t> out) = 0;
    /// Burst write starting at `reg`. Errc::kIo on NACK/bus error, kTimeout on bus timeout.
    virtual Status write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) = 0;
};

} // namespace qz::hal
