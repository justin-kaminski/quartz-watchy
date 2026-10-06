// BMA423 register model. Behaviour follows docs/research/bma423.md [R1]; anything the research
// could not confirm is tagged [ASSUMED]. It models the register protocol the Bosch SensorAPI uses:
// CHIP_ID, soft reset, PWR_CONF adv_power_save, the INIT_CTRL / ASIC address (0x5B/0x5C) /
// FEATURES_IN (0x5E "address trap") config-image upload, INTERNAL_STATUS, the 70-byte feature
// block, the step counter, INT pin registers and the latched feature interrupt status.
#include "qz/core/crc32.hpp"
#include "qz/testkit/fakes.hpp"

#include <algorithm>

namespace qz::testkit {

namespace {

constexpr std::uint8_t kRegChipId = 0x00;
constexpr std::uint8_t kRegEvent = 0x1B;
constexpr std::uint8_t kRegIntStatus0 = 0x1C;
constexpr std::uint8_t kRegIntStatus1 = 0x1D;
constexpr std::uint8_t kRegStep0 = 0x1E; ///< STEP_COUNTER_0..3, byte 0 least significant [ASSUMED]
constexpr std::uint8_t kRegStep3 = 0x21;
constexpr std::uint8_t kRegInternalStatus = 0x2A;
constexpr std::uint8_t kRegAccConf = 0x40;
constexpr std::uint8_t kRegAccRange = 0x41;
constexpr std::uint8_t kRegInt1IoCtrl = 0x53;
constexpr std::uint8_t kRegInt1Map = 0x56;
constexpr std::uint8_t kRegInitCtrl = 0x59;
constexpr std::uint8_t kRegAsicLsb = 0x5B;
constexpr std::uint8_t kRegAsicMsb = 0x5C;
constexpr std::uint8_t kRegFeatures = 0x5E;
constexpr std::uint8_t kRegPwrConf = 0x7C;
constexpr std::uint8_t kRegCmd = 0x7E;
constexpr std::uint8_t kCmdSoftReset = 0xB6;

constexpr std::uint8_t kStatusInitOk = 0x01;
constexpr std::uint8_t kStatusInitError = 0x02;
constexpr std::uint8_t kPwrConfAdvPowerSave = 0x01;
constexpr std::uint8_t kInt1OutputEn = 0x08;
constexpr std::uint8_t kInt1LevelHigh = 0x02;

constexpr std::size_t kStepCtrlByte = 0x3B; ///< step-counter feature byte 1 [R1 s5]
constexpr std::uint8_t kStepCounterEnable = 0x10;
constexpr std::uint8_t kStepCounterReset = 0x04;
constexpr std::size_t kDoubleTapByte = 0x3E;
constexpr std::uint8_t kDoubleTapEnable = 0x01;
constexpr std::uint8_t kDoubleTapInt = 0x10;

constexpr std::int64_t kInitDurationUs = 140'000; ///< init_ok after INIT_CTRL=1 [R1 s3]
constexpr std::int64_t kLowPowerIdleUs = 450; ///< write-to-access idle, adv_power_save on [R1 s11]

/// FEATURES_IN byte offset selected by the ASIC address registers (16-bit words; LSB register
/// holds the low 4 bits, MSB register the rest) [R1 s3, API stream_transfer_write].
std::size_t asic_byte_offset(std::uint8_t lsb, std::uint8_t msb) {
    return 2U * ((static_cast<std::size_t>(msb) << 4U) | (lsb & 0x0FU));
}

} // namespace

FakeBma423::FakeBma423() {
    power_on_reset();
}

FakeBma423::FakeBma423(const VirtualClock& clock) : clock_(&clock) {
    power_on_reset();
}

void FakeBma423::power_on_reset() {
    regs_.fill(0);
    regs_[kRegPwrConf] = 0x03; // adv_power_save + fifo_self_wakeup [R1 s8]
    regs_[kRegAccConf] = 0xA8;
    regs_[kRegAccRange] = 0x01;
    regs_[kRegEvent] = 0x01; // por_detected [R1 s4]
    config_.fill(0);
    word_loaded_.fill(false);
    features_.fill(0);
    internal_status_ = 0;
    init_ready_us_ = 0;
    counter_ = 0;
    engine_ = false;
    loading_ = true; // INIT_CTRL resets to 0: the part accepts a config image
}

void FakeBma423::check_idle(bool is_write) {
    if (clock_ == nullptr) {
        return;
    }
    const std::int64_t now = clock_->elapsed_us();
    if ((regs_[kRegPwrConf] & kPwrConfAdvPowerSave) != 0U && last_write_us_ >= 0 &&
        now - last_write_us_ < kLowPowerIdleUs) {
        ++violations_;
    }
    if (is_write) {
        last_write_us_ = now;
    }
}

void FakeBma423::read_feature(std::span<std::uint8_t> out) {
    // Address trap: consecutive feature bytes from the ASIC address; needs the engine and APS off.
    const bool allowed = engine_ && (regs_[kRegPwrConf] & kPwrConfAdvPowerSave) == 0U;
    if (!allowed) {
        ++violations_;
    }
    const std::size_t base = asic_byte_offset(regs_[kRegAsicLsb], regs_[kRegAsicMsb]);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = allowed && base + i < features_.size() ? features_[base + i] : 0U;
    }
}

std::uint8_t FakeBma423::read_one(std::uint8_t address) {
    if (address == kRegChipId) {
        return chip_id_;
    }
    if (address >= kRegStep0 && address <= kRegStep3) {
        return static_cast<std::uint8_t>(counter_ >> (8U * (address - kRegStep0)));
    }
    if (address == kRegInternalStatus) {
        const bool ready = clock_ == nullptr || clock_->elapsed_us() >= init_ready_us_;
        return ready ? internal_status_ : std::uint8_t{0};
    }
    const std::uint8_t value = regs_[address];
    if (address == kRegEvent || address == kRegIntStatus0 || address == kRegIntStatus1) {
        regs_[address] = 0; // clear on read [R1 s4, s6]
    }
    return value;
}

Status FakeBma423::read_registers(std::uint8_t reg, std::span<std::uint8_t> out) {
    ++reads_;
    if (fail_io_) {
        return Errc::kIo;
    }
    check_idle(false);
    if (reg == kRegFeatures) {
        read_feature(out);
        return ok();
    }
    if (static_cast<std::size_t>(reg) + out.size() > regs_.size()) {
        return Errc::kBadArgs;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = read_one(static_cast<std::uint8_t>(reg + i));
    }
    return ok();
}

void FakeBma423::write_feature(std::span<const std::uint8_t> data) {
    if ((regs_[kRegPwrConf] & kPwrConfAdvPowerSave) != 0U) { // feature access needs APS off [R1 s3]
        ++violations_;
        return;
    }
    const std::size_t base = asic_byte_offset(regs_[kRegAsicLsb], regs_[kRegAsicMsb]);
    if (loading_) { // config image: even-length bursts at the ASIC word address [R1 s3 step 5]
        if (data.size() % 2U != 0U || base + data.size() > config_.size()) {
            ++violations_;
            return;
        }
        std::ranges::copy(data, config_.begin() + static_cast<std::ptrdiff_t>(base));
        std::fill_n(
            word_loaded_.begin() + static_cast<std::ptrdiff_t>(base / 2U), data.size() / 2U, true);
        ++chunks_;
        return;
    }
    if (!engine_ || data.size() % 2U != 0U || base + data.size() > features_.size()) {
        ++violations_;
        return;
    }
    std::ranges::copy(data, features_.begin() + static_cast<std::ptrdiff_t>(base));
    if ((features_[kStepCtrlByte] & kStepCounterReset) != 0U) { // strobe: clears itself [R1 s5]
        counter_ = 0;
        features_[kStepCtrlByte] =
            static_cast<std::uint8_t>(features_[kStepCtrlByte] & ~kStepCounterReset);
    }
}

void FakeBma423::write_register(std::uint8_t address, std::uint8_t value) {
    if (address == kRegChipId || (address >= kRegEvent && address <= kRegInternalStatus)) {
        return; // read-only
    }
    regs_[address] = value;
    if (address == kRegInitCtrl) {
        if ((value & 1U) == 0U) { // accept a configuration image
            loading_ = true;
            engine_ = false;
            internal_status_ = 0;
            return;
        }
        if (engine_) { // "once per POR or soft reset" [R1 s3 step 6]
            ++violations_;
            return;
        }
        const bool complete = std::ranges::all_of(word_loaded_, [](bool b) { return b; });
        loading_ = false;
        features_.fill(0);
        regs_[kRegAsicLsb] = 0;
        regs_[kRegAsicMsb] = 0;
        engine_ = complete && !fail_load_;
        internal_status_ = engine_ ? kStatusInitOk : kStatusInitError;
        uploads_ += engine_ ? 1U : 0U;
        init_ready_us_ = clock_ != nullptr ? clock_->elapsed_us() + kInitDurationUs : 0;
    } else if (address == kRegCmd && value == kCmdSoftReset) {
        ++soft_resets_;
        power_on_reset();
    }
}

Status FakeBma423::write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) {
    ++writes_;
    if (fail_io_) {
        return Errc::kIo;
    }
    check_idle(true);
    if (reg == kRegFeatures) {
        write_feature(data);
        return ok();
    }
    if (static_cast<std::size_t>(reg) + data.size() > regs_.size()) {
        return Errc::kBadArgs;
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        write_register(static_cast<std::uint8_t>(reg + i), data[i]);
    }
    return ok();
}

void FakeBma423::add_steps(std::uint32_t n) {
    if (engine_ && (features_[kStepCtrlByte] & kStepCounterEnable) != 0U) {
        counter_ += n;
    }
}

void FakeBma423::sensor_reset() {
    power_on_reset();
    last_write_us_ = -1;
}

void FakeBma423::trigger_double_tap() {
    if (engine_ && (features_[kDoubleTapByte] & kDoubleTapEnable) != 0U) {
        raise_feature_interrupt(kDoubleTapInt);
    }
}

void FakeBma423::raise_feature_interrupt(std::uint8_t status_0_bits) {
    regs_[kRegIntStatus0] = static_cast<std::uint8_t>(regs_[kRegIntStatus0] | status_0_bits);
}

void FakeBma423::fail_io(bool fail) {
    fail_io_ = fail;
}

void FakeBma423::set_chip_id(std::uint8_t id) {
    chip_id_ = id;
}

void FakeBma423::fail_config_load(bool fail) {
    fail_load_ = fail;
}

std::uint8_t FakeBma423::reg(std::uint8_t address) const {
    return address < regs_.size() ? regs_[address] : std::uint8_t{0};
}

std::uint8_t FakeBma423::feature_byte(std::size_t offset) const {
    return offset < features_.size() ? features_[offset] : std::uint8_t{0};
}

std::uint32_t FakeBma423::read_calls() const {
    return reads_;
}

std::uint32_t FakeBma423::write_calls() const {
    return writes_;
}

std::uint32_t FakeBma423::config_uploads() const {
    return uploads_;
}

std::uint32_t FakeBma423::config_chunks() const {
    return chunks_;
}

std::size_t FakeBma423::config_bytes_received() const {
    return 2U * static_cast<std::size_t>(std::ranges::count(word_loaded_, true));
}

std::uint32_t FakeBma423::config_crc32() const {
    return crc32(config_);
}

bool FakeBma423::engine_running() const {
    return engine_;
}

std::uint32_t FakeBma423::soft_resets() const {
    return soft_resets_;
}

std::uint32_t FakeBma423::step_counter() const {
    return counter_;
}

std::uint32_t FakeBma423::protocol_violations() const {
    return violations_;
}

bool FakeBma423::int1_active() const {
    return (regs_[kRegInt1IoCtrl] & kInt1OutputEn) != 0U &&
           (regs_[kRegIntStatus0] & regs_[kRegInt1Map]) != 0U;
}

bool FakeBma423::int1_level_high() const {
    if ((regs_[kRegInt1IoCtrl] & kInt1OutputEn) == 0U) {
        return false;
    }
    const bool active_high = (regs_[kRegInt1IoCtrl] & kInt1LevelHigh) != 0U;
    return active_high == int1_active();
}

} // namespace qz::testkit
