// SSD1681 / GDEY0154D67 driver logic written from the datasheet (docs/research/ssd1681.md).
// Pure: talks only to hal::EpdBus. Not thread-safe (app task).
#pragma once

#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/epd_bus.hpp"

#include <cstdint>

namespace qz::ssd1681 {

enum class UpdateMode : std::uint8_t {
    kFull,    ///< full waveform from OTP LUT (flashes), clears ghosting
    kPartial, ///< differential update, old image in RAM 0x26, no flashing
};

struct Timeouts {
    std::uint32_t reset_ms = 200;     ///< BUSY after SW reset 0x12 [ssd1681.md s9.1]
    std::uint32_t full_ms = 10'000;   ///< full refresh, typ ~2 s at 25 C [TUNE]
    std::uint32_t partial_ms = 5'000; ///< partial refresh, typ 0.26 s at 25 C [ssd1681.md s9.3]
};

class Panel {
public:
    Panel(hal::EpdBus& bus, Timeouts timeouts = {}) noexcept;

    /// HW reset + SW reset + driver output, data entry mode, RAM window 200x200, border, internal
    /// temperature sensor. Required after every MCU wake (panel was in deep sleep).
    Status init() noexcept;
    /// Writes `previous` to the old-image RAM and `next` to the new-image RAM, then triggers the
    /// update (master activation). Does NOT wait: call finish() (lets the app light-sleep).
    Status begin_update(const gfx::Framebuffer& previous,
                        const gfx::Framebuffer& next,
                        UpdateMode mode) noexcept;
    /// Waits for BUSY low (bus decides how), then enters deep sleep mode 1.
    Status finish(UpdateMode mode) noexcept;
    /// Convenience: begin_update + finish.
    Status update(const gfx::Framebuffer& previous,
                  const gfx::Framebuffer& next,
                  UpdateMode mode) noexcept;
    /// Deep sleep mode 1 (requires HW reset to wake).
    Status sleep() noexcept;
    /// Panel temperature in deci-degrees C from the internal sensor (valid after init()).
    /// Needs EpdBus::read(); kUnsupported otherwise.
    Result<std::int16_t> temperature_dc() noexcept;
    /// Bring-up experiment E1 (ssd1681.md s11): reads OTP waveform-setting bits to decide whether a
    /// display-mode-2 (partial) waveform exists. kUnsupported without EpdBus::read().
    Result<bool> probe_mode2_waveform() noexcept;

private:
    hal::EpdBus& bus_;
    Timeouts timeouts_;
};

} // namespace qz::ssd1681
