// E-paper bus: SPI + DC/CS/RST/BUSY of the SSD1681 panel.
// Implemented by qz_platform (IDF SPI master) and qz_testkit (FakeEpdPanel models the controller).
// Not thread-safe: used by the app task only.
#pragma once

#include "qz/core/result.hpp"
#include "qz/hal/delay.hpp"

#include <cstdint>
#include <span>

namespace qz::hal {

class EpdBus {
public:
    virtual ~EpdBus() = default;

    /// Hardware reset pulse on RST with the datasheet timings (R1/ssd1681.md); blocks.
    virtual Status hardware_reset() = 0;
    /// One command byte (DC low), CS framed.
    virtual Status command(std::uint8_t cmd) = 0;
    /// Parameter/RAM bytes (DC high), CS framed; any length (frames are 5000 bytes).
    virtual Status data(std::span<const std::uint8_t> bytes) = 0;
    /// Reads parameter bytes after a read command using the 3-wire turnaround on SDA at <= 2.5 MHz
    /// [ssd1681.md s2]. Optional: kUnsupported if the implementation cannot read.
    virtual Status read(std::span<std::uint8_t> out) = 0;
    /// False when read() can only return kUnsupported (lets the driver skip the temperature
    /// sequence instead of paying a BUSY cycle for a read that cannot succeed).
    [[nodiscard]] virtual bool supports_read() const { return true; }
    /// Current BUSY level, true = controller busy.
    [[nodiscard]] virtual bool busy() const = 0;
    /// Blocks until BUSY is low. Battery: light sleep with GPIO wake on BUSY; tethered: polls.
    /// Errc::kTimeout if still busy after timeout_ms.
    virtual Status wait_idle(std::uint32_t timeout_ms) = 0;
};

} // namespace qz::hal
