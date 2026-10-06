// FakeEpdPanel: a command-protocol model of the SSD1681 behind hal::EpdBus.
// Rules are those of docs/research/ssd1681.md section 10 (R1); section numbers in comments refer to
// it. Tags: [ASSUMED] where R1 itself assumes. Deterministic; time comes from VirtualClock.
#include "qz/gfx/framebuffer.hpp"
#include "qz/testkit/fakes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace qz::testkit {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kUsPerMs = 1000;
constexpr std::int64_t kResetPulseUs = 20'000; ///< RES# low 10 ms + settle 10 ms [R1 s2, ASSUMED]
constexpr std::int64_t kSoftResetUs = 10'000;  ///< BUSY after 0x12 [ASSUMED]
constexpr std::int64_t kLoadOnlyUs = 100'000;  ///< temperature/LUT load, refused update [ASSUMED]
constexpr std::int64_t kAutoFillUs = 5'000;    ///< 0x46/0x47 fill [ASSUMED]
constexpr std::int16_t kMinOperatingDc = 0;    ///< panel rated 0..50 C [R1 s1 risk 3]
constexpr std::int16_t kMaxOperatingDc = 500;
constexpr std::size_t kRamX = 25;     ///< bytes per RAM row [R1 s3]
constexpr std::uint16_t kMaxX = 0x18; ///< [R1 s10 rule 6]
constexpr std::uint16_t kMaxY = 0xC7;
constexpr std::size_t kLogParamCap = 160;
constexpr std::uint8_t kSensorExternal = 0x48; ///< 0x18 POR [R1 s4]
constexpr std::uint8_t kSensorInternal = 0x80;
constexpr std::uint8_t kCmdStatusRead = 0x2F;

enum class Kind : std::uint8_t { kParams, kImmediate, kRamStream, kRead };

struct CmdInfo {
    std::uint8_t cmd;
    Kind kind;
    std::uint16_t count; ///< parameter bytes, or bytes returned by a read
};

/// Parameter counts from R1 s10 rule 5; 24/26 are streams; reads from s4.
constexpr std::array<CmdInfo, 28> kCommands{{
    {0x01, Kind::kParams, 3},    {0x03, Kind::kParams, 1},    {0x04, Kind::kParams, 3},
    {0x0C, Kind::kParams, 4},    {0x10, Kind::kParams, 1},    {0x11, Kind::kParams, 1},
    {0x12, Kind::kImmediate, 0}, {0x18, Kind::kParams, 1},    {0x1A, Kind::kParams, 2},
    {0x1B, Kind::kRead, 2},      {0x20, Kind::kImmediate, 0}, {0x21, Kind::kParams, 2},
    {0x22, Kind::kParams, 1},    {0x24, Kind::kRamStream, 0}, {0x26, Kind::kRamStream, 0},
    {0x2C, Kind::kParams, 1},    {0x2D, Kind::kRead, 11},     {0x2F, Kind::kRead, 1},
    {0x32, Kind::kParams, 153},  {0x3C, Kind::kParams, 1},    {0x3F, Kind::kParams, 1},
    {0x44, Kind::kParams, 2},    {0x45, Kind::kParams, 4},    {0x46, Kind::kParams, 1},
    {0x47, Kind::kParams, 1},    {0x4E, Kind::kParams, 1},    {0x4F, Kind::kParams, 2},
    {0x7F, Kind::kImmediate, 0},
}};

const CmdInfo* find_command(std::uint8_t cmd) noexcept {
    for (const CmdInfo& c : kCommands) {
        if (c.cmd == cmd) {
            return &c;
        }
    }
    return nullptr;
}

/// OTP-programming commands: never allowed on a production panel [R1 s4, s10 rule 9].
bool is_otp_program(std::uint8_t cmd) noexcept {
    return cmd == 0x08 || cmd == 0x09 || cmd == 0x0A || cmd == 0x2A || cmd == 0x30 || cmd == 0x36 ||
           cmd == 0x39;
}

/// The 12 documented 0x22 values [R1 s5].
bool is_documented_ctrl2(std::uint8_t v) noexcept {
    constexpr std::array<std::uint8_t, 12> kValues{
        0xF7, 0xFF, 0xC7, 0xCF, 0xB1, 0xB9, 0x91, 0x99, 0x80, 0xC0, 0x01, 0x03};
    return std::ranges::find(kValues, v) != kValues.end();
}

// 0x22 bit-field decode [R1 s5, inference].
constexpr std::uint8_t kCtrlLoadTemp = 0x20;
constexpr std::uint8_t kCtrlLoadLut = 0x10;
constexpr std::uint8_t kCtrlMode2 = 0x08;
constexpr std::uint8_t kCtrlDisplay = 0x04;

std::uint16_t encode_temp12(std::int16_t dc) noexcept {
    const std::int32_t t = dc;
    const std::int32_t sixteenths = ((t * 16) + (t >= 0 ? 5 : -5)) / 10;
    return static_cast<std::uint16_t>(sixteenths & 0x0FFF);
}

std::int32_t decode_temp12_dc(std::uint16_t raw) noexcept {
    std::int32_t v = raw & 0x0FFF;
    if ((raw & 0x0800U) != 0) {
        v -= 0x1000;
    }
    return (v * 10) / 16;
}

std::uint16_t axis(std::uint8_t lo, std::uint8_t hi) noexcept {
    return static_cast<std::uint16_t>(lo | ((hi & 1U) << 8U));
}

gfx::Framebuffer inverted(std::span<const std::uint8_t> ram) noexcept {
    gfx::Framebuffer fb;
    for (std::size_t i = 0; i < gfx::kFrameBytes; ++i) {
        fb.bits[i] = static_cast<std::uint8_t>(~ram[i]); // RAM 1 = white, fb 1 = black
    }
    return fb;
}

} // namespace

FakeEpdPanel::FakeEpdPanel(VirtualClock& clock) : clock_(&clock) {
    por_registers();
}

void FakeEpdPanel::por_registers() {
    window_ = Window{0, 0x15, 0, 0x127}; // POR [R1 s3]
    entry_mode_ = 0x03;
    counter_x_set_ = false;
    counter_y_set_ = false;
    sensor_select_ = kSensorExternal;
    update_ctrl2_ = 0xFF;
    ctrl2_written_ = false;
    temp_register_ = 0x7FF; // 127.9 C [R1 s4]
    temperature_ready_ = false;
    lut_ready_ = false;
    lut_manual_ = false;
    pending_cmd_ = -1;
    stream_started_ = false;
    stream_count_ = 0;
    stream_limit_ = 0;
    params_.clear();
}

void FakeEpdPanel::settle() const {
    if (pending_commit_ && clock_->elapsed_us() >= busy_until_us_) {
        displayed_ = pending_image_;
        if (pending_is_full_) {
            ++full_updates_;
        } else {
            ++partial_updates_;
        }
        pending_commit_ = false;
    }
}

Status FakeEpdPanel::violate(EpdViolation v) {
    ++violations_;
    last_violation_ = v;
    return Error{Errc::kInvalidState, static_cast<std::uint16_t>(v)};
}

void FakeEpdPanel::begin_busy(std::int64_t duration_us) {
    busy_until_us_ = stick_next_busy_ ? kInt64Max : clock_->elapsed_us() + duration_us;
    stick_next_busy_ = false;
}

Status FakeEpdPanel::hardware_reset() {
    settle();
    if (pending_commit_) {
        ++aborted_; // reset while updating: the image is left as it was [R1 s8]
        pending_commit_ = false;
    }
    busy_until_us_ = 0;
    asleep_ = false;
    por_registers();
    // RAM contents are UNDEFINED after RES# [R1 s10 rule 3]: scramble so nothing can rely on them.
    for (auto& plane : ram_) {
        for (std::uint8_t& b : plane) {
            scramble_state_ ^= scramble_state_ << 13U;
            scramble_state_ ^= scramble_state_ >> 17U;
            scramble_state_ ^= scramble_state_ << 5U;
            b = static_cast<std::uint8_t>(scramble_state_ >> 24U);
        }
    }
    ram_written_ = {false, false};
    clock_->advance_us(kResetPulseUs);
    ++hw_resets_;
    log_.push_back(EpdLogEntry{kEpdLogHardwareReset, {}, 0});
    return ok();
}

Status FakeEpdPanel::finish_pending() {
    if (pending_cmd_ < 0) {
        return ok();
    }
    const CmdInfo* info = find_command(static_cast<std::uint8_t>(pending_cmd_));
    const bool bad_params = info->kind == Kind::kParams && params_.size() != info->count;
    const bool bad_stream =
        info->kind == Kind::kRamStream && (!stream_started_ || stream_count_ != stream_limit_);
    pending_cmd_ = -1;
    if (bad_params) {
        return violate(EpdViolation::kWrongParamCount);
    }
    if (bad_stream) {
        return violate(EpdViolation::kRamWriteIncomplete);
    }
    return ok();
}

Status FakeEpdPanel::command(std::uint8_t cmd) {
    settle();
    if (asleep_) {
        return violate(EpdViolation::kWhileAsleep);
    }
    if (clock_->elapsed_us() < busy_until_us_ && cmd != kCmdStatusRead) {
        return violate(EpdViolation::kWhileBusy);
    }
    if (is_otp_program(cmd)) {
        return violate(EpdViolation::kOtpProgramCommand);
    }
    const CmdInfo* info = find_command(cmd);
    if (info == nullptr) {
        return violate(EpdViolation::kUnknownCommand);
    }
    if (const Status s = finish_pending(); !s) {
        return s;
    }
    log_.push_back(EpdLogEntry{cmd, {}, 0});
    pending_cmd_ = cmd;
    params_.clear();
    switch (info->kind) {
        case Kind::kRamStream:
            stream_plane_ = cmd == 0x24 ? 0U : 1U;
            stream_started_ = false;
            stream_count_ = 0;
            stream_limit_ = 0;
            break;
        case Kind::kImmediate:
            pending_cmd_ = -1;
            if (cmd == 0x12) {
                // SW reset: POR registers, RAM kept [R1 s4, s10 rule 4].
                por_registers();
                ++sw_resets_;
                begin_busy(kSoftResetUs);
            } else if (cmd == 0x20) {
                if (const Status s = activate(); !s) {
                    log_.pop_back();
                    return s;
                }
            }
            break; // 0x7F NOP just ends the stream (finish_pending checked it)
        case Kind::kParams:
        case Kind::kRead:
            break;
    }
    return ok();
}

Status FakeEpdPanel::start_ram_stream() {
    if (!counter_x_set_ || !counter_y_set_) {
        return violate(EpdViolation::kCounterNotSet);
    }
    const bool x_inc = (entry_mode_ & 1U) != 0;
    const bool y_inc = (entry_mode_ & 2U) != 0;
    const Window& w = window_;
    if (w.xsa > kMaxX || w.xea > kMaxX || w.ysa > kMaxY || w.yea > kMaxY ||
        (x_inc ? w.xsa > w.xea : w.xsa < w.xea) || (y_inc ? w.ysa > w.yea : w.ysa < w.yea)) {
        return violate(EpdViolation::kBadParameter); // window unset, off-RAM or against 0x11 ID
    }
    if (counter_x_ != w.xsa || counter_y_ != w.ysa) {
        return violate(EpdViolation::kCounterNotAtWindowStart);
    }
    const std::size_t cols =
        static_cast<std::size_t>(std::max(w.xsa, w.xea) - std::min(w.xsa, w.xea)) + 1U;
    const std::size_t rows =
        static_cast<std::size_t>(std::max(w.ysa, w.yea) - std::min(w.ysa, w.yea)) + 1U;
    stream_limit_ = cols * rows;
    stream_started_ = true;
    return ok();
}

void FakeEpdPanel::log_param_bytes(std::span<const std::uint8_t> bytes) {
    EpdLogEntry& e = log_.back();
    e.data_len += static_cast<std::uint32_t>(bytes.size());
    for (const std::uint8_t b : bytes) {
        if (e.params.size() < kLogParamCap) {
            e.params.push_back(b);
        }
    }
}

Status FakeEpdPanel::data(std::span<const std::uint8_t> bytes) {
    settle();
    if (asleep_) {
        return violate(EpdViolation::kWhileAsleep);
    }
    if (clock_->elapsed_us() < busy_until_us_) {
        return violate(EpdViolation::kWhileBusy);
    }
    const CmdInfo* info =
        pending_cmd_ < 0 ? nullptr : find_command(static_cast<std::uint8_t>(pending_cmd_));
    if (info == nullptr || info->kind == Kind::kRead || info->kind == Kind::kImmediate) {
        return violate(EpdViolation::kDataWithoutCommand);
    }
    if (info->kind == Kind::kParams) {
        if (params_.size() + bytes.size() > info->count) {
            return violate(EpdViolation::kWrongParamCount);
        }
        params_.insert(params_.end(), bytes.begin(), bytes.end());
        log_param_bytes(bytes);
        return params_.size() == info->count ? apply_params() : ok();
    }
    // RAM stream: bytes land at consecutive window addresses [R1 s3].
    if (!stream_started_) {
        if (const Status s = start_ram_stream(); !s) {
            return s;
        }
    }
    if (stream_count_ + bytes.size() > stream_limit_) {
        return violate(EpdViolation::kRamWriteOverflow); // wrap behaviour UNSPECIFIED [R1 s3]
    }
    const Window& w = window_;
    const bool x_inc = (entry_mode_ & 1U) != 0;
    const bool y_inc = (entry_mode_ & 2U) != 0;
    const bool y_first = (entry_mode_ & 4U) != 0;
    const std::size_t cols =
        static_cast<std::size_t>(std::max(w.xsa, w.xea) - std::min(w.xsa, w.xea)) + 1U;
    const std::size_t rows =
        static_cast<std::size_t>(std::max(w.ysa, w.yea) - std::min(w.ysa, w.yea)) + 1U;
    for (const std::uint8_t b : bytes) {
        const std::size_t i = stream_count_++;
        const std::size_t col = y_first ? i / rows : i % cols;
        const std::size_t row = y_first ? i % rows : i / cols;
        const std::size_t x = x_inc ? w.xsa + col : w.xsa - col;
        const std::size_t y = y_inc ? w.ysa + row : w.ysa - row;
        ram_[stream_plane_][(y * kRamX) + x] = b;
    }
    log_.back().data_len += static_cast<std::uint32_t>(bytes.size()); // stream bytes are not stored
    if (stream_count_ == stream_limit_) {
        ram_written_[stream_plane_] = true;
        counter_x_set_ = false; // the counters ran off the window end: a new stream needs 4E/4F
        counter_y_set_ = false;
    }
    return ok();
}

// Flat per-command validation table: one case per command, complexity is the case count.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Status FakeEpdPanel::apply_params() {
    const std::vector<std::uint8_t>& p = params_;
    switch (pending_cmd_) {
        case 0x01: // driver output: 200 gates only [R1 s4 "send C7 00 00"]
            if (axis(p[0], p[1]) != 0xC7 || (p[1] & 0xFEU) != 0 || (p[2] & 0xF8U) != 0) {
                return violate(EpdViolation::kBadParameter);
            }
            break;
        case 0x03:
            if (p[0] > 0x1F) {
                return violate(EpdViolation::kBadParameter);
            }
            break;
        case 0x10: // deep sleep: 00 normal, 01 mode 1, 11 mode 2 [R1 s4]
            if (p[0] != 0x00 && p[0] != 0x01 && p[0] != 0x03) {
                return violate(EpdViolation::kBadParameter);
            }
            if (p[0] != 0) {
                asleep_ = true; // BUSY stays high until a hardware reset [R1 s2]
                ++sleeps_;
            }
            break;
        case 0x11:
            if (p[0] > 0x07) {
                return violate(EpdViolation::kBadParameter);
            }
            entry_mode_ = p[0];
            break;
        case 0x18:
            if (p[0] == kSensorExternal) {
                return violate(EpdViolation::kExternalSensor); // TSCL/TSDA unconnected [R1 s6]
            }
            if (p[0] != kSensorInternal) {
                return violate(EpdViolation::kBadParameter);
            }
            sensor_select_ = p[0];
            break;
        case 0x1A:
            temp_register_ = static_cast<std::uint16_t>((p[0] << 4U) | (p[1] >> 4U));
            temperature_ready_ = true;
            break;
        case 0x21: // RAM bypass/invert options are not modelled: POR only
            if (p[0] != 0 || p[1] != 0) {
                return violate(EpdViolation::kBadParameter);
            }
            break;
        case 0x22:
            if (!is_documented_ctrl2(p[0])) {
                return violate(EpdViolation::kUndocumentedUpdateValue);
            }
            update_ctrl2_ = p[0];
            ctrl2_written_ = true;
            break;
        case 0x32:
            lut_ready_ = true;
            lut_manual_ = true;
            break;
        case 0x3C: // bit 3 is reserved [R1 s6]
            if ((p[0] & 0x08U) != 0) {
                return violate(EpdViolation::kBadParameter);
            }
            break;
        case 0x44:
            if (p[0] > kMaxX || p[1] > kMaxX) {
                return violate(EpdViolation::kBadParameter);
            }
            window_.xsa = p[0];
            window_.xea = p[1];
            counter_x_set_ = false;
            counter_y_set_ = false;
            break;
        case 0x45: {
            const std::uint16_t ysa = axis(p[0], p[1]);
            const std::uint16_t yea = axis(p[2], p[3]);
            if (ysa > kMaxY || yea > kMaxY || (p[1] & 0xFEU) != 0 || (p[3] & 0xFEU) != 0) {
                return violate(EpdViolation::kBadParameter);
            }
            window_.ysa = ysa;
            window_.yea = yea;
            counter_x_set_ = false;
            counter_y_set_ = false;
            break;
        }
        case 0x46:
        case 0x47: // auto-fill with 1s (white): F7 only [R1 s4]
            if (p[0] != 0xF7) {
                return violate(EpdViolation::kBadParameter);
            }
            ram_[pending_cmd_ == 0x47 ? 0U : 1U].fill(0xFF);
            ram_written_[pending_cmd_ == 0x47 ? 0U : 1U] = true;
            begin_busy(kAutoFillUs);
            break;
        case 0x4E:
            if (p[0] > kMaxX) {
                return violate(EpdViolation::kBadParameter);
            }
            counter_x_ = p[0];
            counter_x_set_ = true;
            break;
        case 0x4F: {
            const std::uint16_t y = axis(p[0], p[1]);
            if (y > kMaxY || (p[1] & 0xFEU) != 0) {
                return violate(EpdViolation::kBadParameter);
            }
            counter_y_ = y;
            counter_y_set_ = true;
            break;
        }
        default: // 04, 0C, 2C, 3F: stored by the real chip, no modelled effect
            break;
    }
    return ok();
}

Status FakeEpdPanel::check_activation(std::uint8_t v) {
    const bool load_temp = (v & kCtrlLoadTemp) != 0;
    const bool load_lut = (v & kCtrlLoadLut) != 0;
    const bool mode2 = (v & kCtrlMode2) != 0;
    const bool display = (v & kCtrlDisplay) != 0;
    if (load_temp) {
        if (sensor_select_ != kSensorInternal) {
            return violate(sensor_select_ == kSensorExternal ? EpdViolation::kExternalSensor
                                                             : EpdViolation::kNoTemperatureSource);
        }
    } else if ((display || load_lut) && !temperature_ready_ && (load_lut || !lut_manual_)) {
        return violate(EpdViolation::kNoTemperatureSource); // would use the 127.9 C POR value
    }
    if (display && !load_lut && !lut_ready_) {
        return violate(EpdViolation::kNoLut);
    }
    if (display && (!ram_written_[0] || (mode2 && !ram_written_[1]))) {
        return violate(EpdViolation::kRamNotWritten);
    }
    return ok();
}

Status FakeEpdPanel::activate() {
    if (!ctrl2_written_) {
        return violate(EpdViolation::kNoUpdateControl); // never run on the POR value [R1 s5]
    }
    const std::uint8_t v = update_ctrl2_;
    if (const Status s = check_activation(v); !s) {
        return s;
    }
    const bool load_temp = (v & kCtrlLoadTemp) != 0;
    const bool load_lut = (v & kCtrlLoadLut) != 0;
    const bool mode2 = (v & kCtrlMode2) != 0;
    const bool display = (v & kCtrlDisplay) != 0;
    ctrl2_written_ = false;
    if (load_temp) {
        temp_register_ = encode_temp12(sensed_dc_);
        temperature_ready_ = true;
    }
    if (load_lut) {
        lut_ready_ = true;
        lut_manual_ = false;
    }
    if (!display) {
        begin_busy(kLoadOnlyUs);
        return ok();
    }
    const std::int32_t dc = decode_temp12_dc(temp_register_);
    if (!lut_manual_ && (dc < kMinOperatingDc || dc > kMaxOperatingDc)) {
        ++refused_; // no OTP waveform range matches: the panel does not update [R1 s6]
        begin_busy(kLoadOnlyUs);
        return ok();
    }
    for (std::size_t i = 0; i < gfx::kFrameBytes; ++i) {
        const std::uint8_t nw = ram_[0][i]; // new image, 1 = white
        if (!mode2) {
            pending_image_.bits[i] = static_cast<std::uint8_t>(~nw); // full: BW RAM exactly
            continue;
        }
        // Partial: pixels where old (RED) != new (BW) take the new value, others keep their state.
        const auto changed = static_cast<std::uint8_t>(ram_[1][i] ^ nw);
        const auto cur_white = static_cast<std::uint8_t>(~displayed_.bits[i]);
        const auto out_white = static_cast<std::uint8_t>((changed & nw) | (~changed & cur_white));
        pending_image_.bits[i] = static_cast<std::uint8_t>(~out_white);
    }
    pending_commit_ = true;
    pending_is_full_ = !mode2;
    begin_busy(mode2 ? partial_us_ : full_us_);
    return ok();
}

Status FakeEpdPanel::read(std::span<std::uint8_t> out) {
    settle();
    if (asleep_) {
        return violate(EpdViolation::kWhileAsleep);
    }
    const CmdInfo* info =
        pending_cmd_ < 0 ? nullptr : find_command(static_cast<std::uint8_t>(pending_cmd_));
    if (info == nullptr || info->kind != Kind::kRead) {
        return violate(EpdViolation::kReadWithoutCommand);
    }
    if (clock_->elapsed_us() < busy_until_us_ && info->cmd != kCmdStatusRead) {
        return violate(EpdViolation::kWhileBusy);
    }
    if (out.size() != info->count) {
        return violate(EpdViolation::kBadReadLength);
    }
    switch (pending_cmd_) {
        case 0x1B: // [A11:4], [A3:0]<<4, no dummy byte [ASSUMED, R1 s11 E6]
            out[0] = static_cast<std::uint8_t>(temp_register_ >> 4U);
            out[1] = static_cast<std::uint8_t>((temp_register_ & 0x0FU) << 4U);
            break;
        case 0x2D:
            std::ranges::copy(otp_option_, out.begin());
            break;
        default: // 0x2F: A[2] busy flag, A[1:0] chip id 01 [R1 s4]
            out[0] =
                static_cast<std::uint8_t>((clock_->elapsed_us() < busy_until_us_ ? 4U : 0U) | 1U);
            break;
    }
    pending_cmd_ = -1;
    return ok();
}

bool FakeEpdPanel::busy() const {
    settle();
    return asleep_ || clock_->elapsed_us() < busy_until_us_; // BUSY stays high in deep sleep
}

Status FakeEpdPanel::wait_idle(std::uint32_t timeout_ms) {
    settle();
    const std::int64_t timeout_us = static_cast<std::int64_t>(timeout_ms) * kUsPerMs;
    if (asleep_) {
        clock_->advance_us(timeout_us);
        (void)violate(EpdViolation::kBusyWaitAfterSleep); // recorded; the caller sees the timeout
        return Errc::kTimeout;
    }
    const std::int64_t remaining = busy_until_us_ - clock_->elapsed_us();
    if (remaining <= 0) {
        return ok();
    }
    if (remaining <= timeout_us) {
        clock_->advance_us(remaining);
        settle();
        return ok();
    }
    clock_->advance_us(timeout_us);
    settle();
    return Errc::kTimeout;
}

void FakeEpdPanel::fail_next_busy_wait() {
    if (clock_->elapsed_us() < busy_until_us_) {
        busy_until_us_ = kInt64Max;
    } else {
        stick_next_busy_ = true;
    }
}

const gfx::Framebuffer& FakeEpdPanel::displayed() const {
    settle();
    return displayed_;
}

std::uint32_t FakeEpdPanel::full_updates() const {
    settle();
    return full_updates_;
}

std::uint32_t FakeEpdPanel::partial_updates() const {
    settle();
    return partial_updates_;
}

bool FakeEpdPanel::in_deep_sleep() const {
    return asleep_;
}

std::span<const std::uint8_t> FakeEpdPanel::ram_bw() const {
    return ram_[0];
}

std::span<const std::uint8_t> FakeEpdPanel::ram_red() const {
    return ram_[1];
}

gfx::Framebuffer FakeEpdPanel::ram_bw_image() const {
    return inverted(ram_[0]);
}

gfx::Framebuffer FakeEpdPanel::ram_red_image() const {
    return inverted(ram_[1]);
}

Status FakeEpdPanel::encode_displayed_png(gfx::ByteSink& out) const {
    settle();
    return gfx::encode_png(displayed_, out);
}

Status FakeEpdPanel::encode_ram_bw_png(gfx::ByteSink& out) const {
    return gfx::encode_png(ram_bw_image(), out);
}

const std::vector<EpdLogEntry>& FakeEpdPanel::log() const {
    return log_;
}

void FakeEpdPanel::clear_log() {
    log_.clear();
}

std::uint32_t FakeEpdPanel::violation_count() const {
    return violations_;
}

EpdViolation FakeEpdPanel::last_violation() const {
    return last_violation_;
}

std::uint32_t FakeEpdPanel::hardware_resets() const {
    return hw_resets_;
}

std::uint32_t FakeEpdPanel::soft_resets() const {
    return sw_resets_;
}

std::uint32_t FakeEpdPanel::deep_sleep_entries() const {
    return sleeps_;
}

std::uint32_t FakeEpdPanel::refused_updates() const {
    return refused_;
}

std::uint32_t FakeEpdPanel::aborted_updates() const {
    return aborted_;
}

void FakeEpdPanel::set_temperature_dc(std::int16_t temp_dc) {
    sensed_dc_ = temp_dc;
}

std::uint16_t FakeEpdPanel::temperature_register() const {
    return temp_register_;
}

void FakeEpdPanel::set_otp_display_option(const std::array<std::uint8_t, 11>& bytes) {
    otp_option_ = bytes;
}

void FakeEpdPanel::set_busy_durations_us(std::int64_t full_us, std::int64_t partial_us) {
    full_us_ = full_us;
    partial_us_ = partial_us;
}

} // namespace qz::testkit
