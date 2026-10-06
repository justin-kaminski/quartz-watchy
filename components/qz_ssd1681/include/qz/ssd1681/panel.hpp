// SSD1681 / GDEY0154D67 driver logic written from the datasheet (docs/research/ssd1681.md).
// Pure: talks only to hal::EpdBus. Not thread-safe (app task).
#pragma once

#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/epd_bus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qz::ssd1681 {

/// Bytes of the controller's waveform look-up table written with command 0x32 [ssd1681.md s6].
inline constexpr std::size_t kLutBytes = 153;

/// Optional custom partial-update waveform (hook for owner decision Q-12; NO vendor bytes ship in
/// this repository). When a Panel is given one, partial updates write 0x32/0x3F/0x03/0x04/0x2C and
/// run 0x22=CF instead of using the OTP display-mode-2 waveform [ssd1681.md s9.3 fallback].
/// `lut` must stay valid for the lifetime of the Panel and be exactly kLutBytes long.
struct Waveform {
    std::span<const std::uint8_t> lut;     ///< 0x32 payload, kLutBytes bytes
    std::uint8_t eopt = 0x02;              ///< 0x3F end option [ssd1681.md s4]
    std::uint8_t vgh = 0x00;               ///< 0x03 gate voltage [TUNE]
    std::array<std::uint8_t, 3> vsh_vsl{}; ///< 0x04 VSH1, VSH2, VSL [TUNE]
    std::uint8_t vcom = 0x00;              ///< 0x2C VCOM [TUNE]
};

/// Converts framebuffer polarity (1 = black ink, MSB = leftmost) to controller RAM polarity
/// (BW RAM: 1 = white, 0 = black [ssd1681.md s3]; D7 = leftmost [ASSUMED], bring-up E3).
/// `in` and `out` must have equal length, else kBadArgs.
Status to_ram_polarity(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) noexcept;

enum class UpdateMode : std::uint8_t {
    kFull,    ///< full waveform from OTP LUT (flashes), clears ghosting
    kPartial, ///< differential update, old image in RAM 0x26, no flashing
};

struct Timeouts {
    std::uint32_t reset_ms = 200;     ///< BUSY after SW reset 0x12 [ssd1681.md s9.1]
    std::uint32_t full_ms = 10'000;   ///< full refresh, typ ~2 s at 25 C [TUNE]
    std::uint32_t partial_ms = 5'000; ///< partial refresh, typ 0.26 s at 25 C [ssd1681.md s9.3]
    std::uint32_t sense_ms = 1'000;   ///< temperature load 0x22=B1 [ASSUMED, TUNE]
};

class Panel {
public:
    /// `partial_waveform` (optional, may be null) overrides the OTP partial waveform; see Waveform.
    explicit Panel(hal::EpdBus& bus,
                   Timeouts timeouts = {},
                   const Waveform* partial_waveform = nullptr) noexcept;

    /// HW reset + SW reset + driver output, data entry mode, RAM window 200x200, internal
    /// temperature sensor, soft start. (The border is per-update-mode: set in begin_update.)
    /// Required after every MCU wake (panel was in deep sleep). kInvalidState while an update is
    /// in flight. Any bus error or BUSY timeout leaves the Panel needing init() again.
    Status init() noexcept;
    /// Writes `previous` to the old-image RAM and `next` to the new-image RAM, then triggers the
    /// update (master activation). Does NOT wait: call finish() (lets the app light-sleep).
    /// kFull ignores `previous` content for the image but still loads `next` into both planes.
    /// kInvalidState unless init() succeeded and no update is in flight.
    Status begin_update(const gfx::Framebuffer& previous,
                        const gfx::Framebuffer& next,
                        UpdateMode mode) noexcept;
    /// Waits for BUSY low (bus decides how), then enters deep sleep mode 1. On BUSY timeout the
    /// panel is hard-reset, kTimeout is returned and the caller must treat the frame as invalid
    /// (next update is full) [ARCHITECTURE s14]. `mode` must match begin_update (else
    /// kInvalidState).
    Status finish(UpdateMode mode) noexcept;
    /// Convenience: begin_update + finish.
    Status update(const gfx::Framebuffer& previous,
                  const gfx::Framebuffer& next,
                  UpdateMode mode) noexcept;
    /// Deep sleep mode 1 (requires HW reset to wake). Valid after init() when idle; a no-op when
    /// already asleep; kInvalidState while an update is in flight (use finish()).
    Status sleep() noexcept;
    /// Panel temperature in deci-degrees C from the internal sensor (valid after init()).
    /// Needs EpdBus::read(); kUnsupported otherwise.
    Result<std::int16_t> temperature_dc() noexcept;
    /// Bring-up experiment E1 (ssd1681.md s11): reads OTP waveform-setting bits to decide whether a
    /// display-mode-2 (partial) waveform exists. kUnsupported without EpdBus::read().
    Result<bool> probe_mode2_waveform() noexcept;

private:
    enum class State : std::uint8_t {
        kUnknown,  ///< needs init(): fresh, or the last operation failed
        kReady,    ///< initialised, idle, awake
        kUpdating, ///< 0x20 issued, finish() pending
        kAsleep,   ///< 0x10 issued; only a hardware reset wakes the chip
    };

    Error fail(Error error) noexcept; ///< marks the controller state unknown, returns `error`
    Status wait_busy(std::uint32_t timeout_ms) noexcept;
    Status send_plane(std::uint8_t ram_command, const gfx::Framebuffer& image) noexcept;
    Status write_waveform() noexcept;
    Status begin_full(const gfx::Framebuffer& next) noexcept;
    Status begin_partial(const gfx::Framebuffer& previous,
                         const gfx::Framebuffer& next,
                         bool custom) noexcept;

    hal::EpdBus& bus_;
    Timeouts timeouts_;
    const Waveform* partial_waveform_;
    State state_ = State::kUnknown;
    UpdateMode mode_ = UpdateMode::kFull;
};

} // namespace qz::ssd1681
