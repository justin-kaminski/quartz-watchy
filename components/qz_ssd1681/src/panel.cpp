// SSD1681 driver logic. Written from docs/research/ssd1681.md (R1) only; section numbers in
// comments refer to it. Nothing here is hardware-verified until docs/HARDWARE_BRINGUP.md is run.
#include "qz/ssd1681/panel.hpp"

#include "tuning.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace qz::ssd1681 {
namespace {

using namespace tuning;

/// Sends one command with its parameter bytes (DC low, then DC high) [R1 s2].
Status send(hal::EpdBus& bus, std::uint8_t cmd, std::initializer_list<std::uint8_t> params = {}) {
    QZ_RETURN_IF_ERROR(bus.command(cmd));
    if (params.size() != 0) {
        QZ_RETURN_IF_ERROR(bus.data(std::span<const std::uint8_t>(params.begin(), params.size())));
    }
    return ok();
}

/// One command with up to four parameter bytes, for fixed sequences written as tables.
struct Step {
    std::uint8_t cmd;
    std::array<std::uint8_t, 4> params;
    std::uint8_t count;
};

Status run_steps(hal::EpdBus& bus, std::span<const Step> steps) {
    for (const Step& st : steps) {
        QZ_RETURN_IF_ERROR(bus.command(st.cmd));
        if (st.count != 0) {
            QZ_RETURN_IF_ERROR(bus.data(std::span<const std::uint8_t>(st.params).first(st.count)));
        }
    }
    return ok();
}

// R1 s9.1 steps 3-5 (after HW reset, SW reset and its BUSY wait).
constexpr std::array<Step, 6> kCommonStart{{
    {kCmdDriverOutput, {kGateCountMinus1, 0x00, 0x00, 0}, 3},                 // 200 gates [s4]
    {kCmdDataEntry, {kDataEntryXincYinc, 0, 0, 0}, 1},                        // [s3]
    {kCmdRamXWindow, {0x00, kRamXLast, 0, 0}, 2},                             // [s3]
    {kCmdRamYWindow, {0x00, 0x00, kRamYLastLo, 0x00}, 4},                     // [s3]
    {kCmdTempSensor, {kTempSensorInternal, 0, 0, 0}, 1},                      // [s6]
    {kCmdSoftStart, {kSoftStart0, kSoftStart1, kSoftStart2, kSoftStart3}, 4}, // [s4]
}};
// R1 s9.2 steps 2 and 5, s9.3 steps 2 and 5.
constexpr std::array<Step, 1> kFullBorder{{{kCmdBorder, {kBorderFull, 0, 0, 0}, 1}}};
constexpr std::array<Step, 2> kFullRun{{
    {kCmdUpdateControl2, {kUpdateFull, 0, 0, 0}, 1},
    {kCmdActivate, {}, 0},
}};
constexpr std::array<Step, 1> kPartialBorder{{{kCmdBorder, {kBorderPartial, 0, 0, 0}, 1}}};
constexpr std::array<Step, 2> kPartialRunOtp{{
    {kCmdUpdateControl2, {kUpdatePartialOtp, 0, 0, 0}, 1},
    {kCmdActivate, {}, 0},
}};
constexpr std::array<Step, 2> kPartialRunCustom{{
    {kCmdUpdateControl2, {kUpdatePartialCustom, 0, 0, 0}, 1},
    {kCmdActivate, {}, 0},
}};
constexpr std::array<Step, 2> kLoadTemperature{{
    {kCmdUpdateControl2, {kUpdateLoadTemperature, 0, 0, 0}, 1},
    {kCmdActivate, {}, 0},
}};

/// Sign-extends the 12-bit temperature register (1/16 C per LSB) to deci-degrees [R1 s4 0x1A].
std::int16_t temp12_to_dc(std::uint16_t raw12) noexcept {
    std::int32_t v = raw12 & kTemp12Mask;
    if ((raw12 & kTemp12Sign) != 0) {
        v -= kTemp12Range;
    }
    return static_cast<std::int16_t>((v * kDeciPerSixteenth) / kSixteenths);
}

} // namespace

Status to_ram_polarity(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) noexcept {
    if (in.size() != out.size()) {
        return Errc::kBadArgs;
    }
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(~in[i]); // 1 = black ink -> 0 = black in RAM
    }
    return ok();
}

Panel::Panel(hal::EpdBus& bus, Timeouts timeouts, const Waveform* partial_waveform) noexcept
    : bus_(bus), timeouts_(timeouts), partial_waveform_(partial_waveform) {}

Error Panel::fail(Error error) noexcept {
    state_ = State::kUnknown; // the controller state is no longer known: init() again
    return error;
}

Status Panel::wait_busy(std::uint32_t timeout_ms) noexcept {
    const Status waited = bus_.wait_idle(timeout_ms);
    if (waited) {
        return ok();
    }
    if (waited.error().code == Errc::kTimeout) {
        // Fail-safe [R1 s9.5, ARCH s14]: reset the panel; the caller marks the frame invalid.
        (void)bus_.hardware_reset(); // best effort: the timeout is the error we report
    }
    return fail(waited.error());
}

Status Panel::init() noexcept {
    if (state_ == State::kUpdating) {
        return Errc::kInvalidState; // never interrupt an update [R1 s8]
    }
    state_ = State::kUnknown;
    // 9.1 step 1: RES# pulse (timing is the bus's job) [R1 s9.1].
    if (const Status s = bus_.hardware_reset(); !s) {
        return fail(s.error());
    }
    // Step 2: SW reset, then wait for BUSY low.
    if (const Status s = send(bus_, kCmdSoftReset); !s) {
        return fail(s.error());
    }
    QZ_RETURN_IF_ERROR(wait_busy(timeouts_.reset_ms));
    // Steps 3-5: driver output, data entry, window, internal sensor, soft start.
    if (const Status s = run_steps(bus_, kCommonStart); !s) {
        return fail(s.error());
    }
    state_ = State::kReady;
    return ok();
}

Status Panel::send_plane(std::uint8_t ram_command, const gfx::Framebuffer& image) noexcept {
    QZ_RETURN_IF_ERROR(send(bus_, kCmdRamXCounter, {0x00}));
    QZ_RETURN_IF_ERROR(send(bus_, kCmdRamYCounter, {0x00, 0x00}));
    QZ_RETURN_IF_ERROR(bus_.command(ram_command));
    std::array<std::uint8_t, kRamChunkBytes> chunk{};
    const std::span<const std::uint8_t> all = image.bytes();
    for (std::size_t off = 0; off < all.size(); off += kRamChunkBytes) {
        const std::size_t n = std::min(kRamChunkBytes, all.size() - off);
        QZ_RETURN_IF_ERROR(to_ram_polarity(all.subspan(off, n), std::span(chunk).first(n)));
        QZ_RETURN_IF_ERROR(bus_.data(std::span<const std::uint8_t>(chunk).first(n)));
    }
    return ok();
}

Status Panel::write_waveform() noexcept {
    const Waveform& w = *partial_waveform_;
    QZ_RETURN_IF_ERROR(bus_.command(kCmdWriteLut));
    QZ_RETURN_IF_ERROR(bus_.data(w.lut));
    QZ_RETURN_IF_ERROR(send(bus_, kCmdEndOption, {w.eopt}));
    QZ_RETURN_IF_ERROR(send(bus_, kCmdGateVoltage, {w.vgh}));
    QZ_RETURN_IF_ERROR(send(bus_, kCmdSourceVoltage, {w.vsh_vsl[0], w.vsh_vsl[1], w.vsh_vsl[2]}));
    return send(bus_, kCmdWriteVcom, {w.vcom});
}

Status Panel::begin_full(const gfx::Framebuffer& next) noexcept {
    // 9.2: border follows white LUT; RED RAM gets the same image (base for the next partial).
    QZ_RETURN_IF_ERROR(run_steps(bus_, kFullBorder));
    QZ_RETURN_IF_ERROR(send_plane(kCmdWriteRamBw, next));
    QZ_RETURN_IF_ERROR(send_plane(kCmdWriteRamRed, next));
    return run_steps(bus_, kFullRun);
}

Status Panel::begin_partial(const gfx::Framebuffer& previous,
                            const gfx::Framebuffer& next,
                            bool custom) noexcept {
    // 9.3: border held at VCOM; RED = previous image [ASSUMED], BW = new image.
    QZ_RETURN_IF_ERROR(run_steps(bus_, kPartialBorder));
    if (custom) {
        QZ_RETURN_IF_ERROR(write_waveform());
    }
    QZ_RETURN_IF_ERROR(send_plane(kCmdWriteRamRed, previous));
    QZ_RETURN_IF_ERROR(send_plane(kCmdWriteRamBw, next));
    return run_steps(bus_,
                     custom ? std::span<const Step>(kPartialRunCustom)
                            : std::span<const Step>(kPartialRunOtp));
}

Status Panel::begin_update(const gfx::Framebuffer& previous,
                           const gfx::Framebuffer& next,
                           UpdateMode mode) noexcept {
    if (state_ != State::kReady) {
        return Errc::kInvalidState;
    }
    const bool custom = mode == UpdateMode::kPartial && partial_waveform_ != nullptr;
    if (custom && partial_waveform_->lut.size() != kLutBytes) {
        return Errc::kBadArgs;
    }
    const Status s =
        mode == UpdateMode::kFull ? begin_full(next) : begin_partial(previous, next, custom);
    if (!s) {
        return fail(s.error());
    }
    mode_ = mode;
    state_ = State::kUpdating;
    return ok();
}

Status Panel::finish(UpdateMode mode) noexcept {
    if (state_ != State::kUpdating || mode != mode_) {
        return Errc::kInvalidState;
    }
    QZ_RETURN_IF_ERROR(
        wait_busy(mode == UpdateMode::kFull ? timeouts_.full_ms : timeouts_.partial_ms));
    // Deep sleep right after BUSY low (~1 uA vs ~20 uA idle); BUSY then stays high, never wait on
    // it again, only a hardware reset wakes the chip [R1 s2, s7].
    if (const Status s = send(bus_, kCmdDeepSleep, {kDeepSleepMode1}); !s) {
        return fail(s.error());
    }
    state_ = State::kAsleep;
    return ok();
}

Status Panel::update(const gfx::Framebuffer& previous,
                     const gfx::Framebuffer& next,
                     UpdateMode mode) noexcept {
    QZ_RETURN_IF_ERROR(begin_update(previous, next, mode));
    return finish(mode);
}

Status Panel::sleep() noexcept {
    switch (state_) {
        case State::kAsleep:
            return ok();
        case State::kReady:
            break;
        case State::kUpdating:
        case State::kUnknown:
            return Errc::kInvalidState;
    }
    if (const Status s = send(bus_, kCmdDeepSleep, {kDeepSleepMode1}); !s) {
        return fail(s.error());
    }
    state_ = State::kAsleep;
    return ok();
}

Result<std::int16_t> Panel::temperature_dc() noexcept {
    if (state_ != State::kReady) {
        return Errc::kInvalidState;
    }
    if (!bus_.supports_read()) {
        return Errc::kUnsupported; // no BUSY cycle for a read that cannot succeed
    }
    // The register holds its POR value (127.9 C) until a sequence loads the sensor, so run the
    // documented "load temperature + LUT only, no display" sequence B1 first [R1 s5, s6].
    if (const Status loaded = run_steps(bus_, kLoadTemperature); !loaded) {
        return fail(loaded.error());
    }
    QZ_RETURN_IF_ERROR(wait_busy(timeouts_.sense_ms));
    if (const Status s = bus_.command(kCmdReadTemperature); !s) {
        return fail(s.error());
    }
    // [ASSUMED] two bytes A[11:4], A[3:0]<<4 with no dummy byte (R1 s4/s11 E6 to confirm).
    std::array<std::uint8_t, kTemperatureBytes> raw{};
    if (const Status s = bus_.read(raw); !s) {
        return s.error().code == Errc::kUnsupported ? s.error() : fail(s.error());
    }
    const auto raw12 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw[0]) << 4U) |
                                                  (static_cast<std::uint16_t>(raw[1]) >> 4U));
    return temp12_to_dc(raw12);
}

Result<bool> Panel::probe_mode2_waveform() noexcept {
    if (state_ != State::kReady) {
        return Errc::kInvalidState;
    }
    if (const Status s = bus_.command(kCmdReadOtpOption); !s) {
        return fail(s.error());
    }
    // [ASSUMED] 11 bytes, no dummy byte; bytes C..G = display-mode bits of WS0..35 (1 = mode 2).
    std::array<std::uint8_t, kOtpOptionBytes> otp{};
    if (const Status s = bus_.read(otp); !s) {
        return s.error().code == Errc::kUnsupported ? s.error() : fail(s.error());
    }
    for (std::size_t i = kOtpModeFirst; i <= kOtpModeLast; ++i) {
        if (otp[i] != 0) {
            return true;
        }
    }
    return false;
}

} // namespace qz::ssd1681
