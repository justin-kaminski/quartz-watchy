#include "qz/bma423/accel.hpp"

#include "bosch.hpp"
#include "qz/core/log.hpp"
#include "tuning.hpp"

#include <array>
#include <span>

// Wrapper over the vendored Bosch SensorAPI. The API is stateless between calls apart from a few
// fields of struct bma4_dev that bma423_init() derives from constants, so every public method
// builds a Session (a fully populated bma4_dev on the stack) instead of keeping API state. That
// makes attach() after deep sleep trivially free of writes and the object cheap to construct.

namespace qz::bma423 {

namespace {

constexpr const char* kTag = "bma423";

// The config blob is defined (non-static) in third_party/bosch/bma423.c.
static_assert(tuning::kReadWriteLenBytes % 2U == 0U &&
                  tuning::kReadWriteLenBytes <= BMA423_FEATURE_SIZE &&
                  6144U % tuning::kReadWriteLenBytes == 0U,
              "config bursts must be even, <= 70 bytes and divide the 6144-byte blob");

/// What the Bosch callbacks need; also remembers the HAL error behind a generic API failure.
struct Bus {
    hal::I2cDevice* dev = nullptr;
    hal::Delay* delay = nullptr;
    Error io_error{Errc::kIo};
    bool failed = false; ///< a callback saw a HAL error since the Session was built
};

std::int8_t bus_read(std::uint8_t reg, std::uint8_t* data, std::uint32_t len, void* intf) noexcept {
    auto& bus = *static_cast<Bus*>(intf);
    const Status st = bus.dev->read_registers(reg, std::span<std::uint8_t>(data, len));
    if (!st) {
        bus.io_error = st.error();
        bus.failed = true;
        return 1;
    }
    return BMA4_INTF_RET_SUCCESS;
}

std::int8_t
bus_write(std::uint8_t reg, const std::uint8_t* data, std::uint32_t len, void* intf) noexcept {
    auto& bus = *static_cast<Bus*>(intf);
    const Status st = bus.dev->write_registers(reg, std::span<const std::uint8_t>(data, len));
    if (!st) {
        bus.io_error = st.error();
        bus.failed = true;
        return 1;
    }
    if (reg == tuning::kRegPwrConf) {
        // An adv_power_save switch needs >= 450 us before the next access whatever the API's
        // perf_mode_status makes it wait (it can ask for only 2 us during the config upload).
        bus.delay->delay_us(tuning::kLowPowerIdleUs);
    }
    return BMA4_INTF_RET_SUCCESS;
}

void bus_delay(std::uint32_t period_us, void* intf) noexcept {
    auto& bus = *static_cast<Bus*>(intf);
    std::uint32_t us =
        period_us == tuning::kBoschLowPowerIdleUs ? tuning::kLowPowerIdleUs : period_us;
    if (us >= 1000U) {
        bus.delay->delay_ms(us / 1000U);
        us %= 1000U;
    }
    if (us != 0U) {
        bus.delay->delay_us(us);
    }
}

/// A populated bma4_dev bound to the HAL. Not copyable: intf_ptr points into the object.
class Session {
public:
    Session(hal::I2cDevice& dev, hal::Delay& delay) noexcept {
        bus_.dev = &dev;
        bus_.delay = &delay;
        api_.intf = BMA4_I2C_INTF;
        api_.variant = BMA42X_VARIANT;
        api_.intf_ptr = &bus_;
        api_.bus_read = bus_read;
        api_.bus_write = bus_write;
        api_.delay_us = bus_delay;
        api_.read_write_len = tuning::kReadWriteLenBytes;
        // What bma423_init() would derive after a successful chip-id check; init() still calls it.
        api_.chip_id = kChipId;
        api_.resolution = 12;
        api_.feature_len = BMA423_FEATURE_SIZE;
        api_.config_size = 6144;
        api_.config_file_ptr = bma423_config_file;
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
    ~Session() = default;

    bma4_dev& api() noexcept { return api_; }

    /// Maps a Bosch return code to a Status. A HAL error always wins: the vendor code both masks
    /// bus errors behind other codes (feature reads report "bad length") and ignores a failed
    /// config burst when a later one succeeds.
    [[nodiscard]] Status check(std::int8_t rslt) const noexcept {
        if (bus_.failed) {
            return bus_.io_error;
        }
        switch (rslt) {
            case BMA4_OK:
                return ok();
            case BMA4_E_COM_FAIL:
                return bus_.io_error;
            case BMA4_E_INVALID_SENSOR:
            case BMA4_E_DEV_NOT_FOUND:
                return Error{Errc::kNotFound};
            case BMA4_E_CONFIG_STREAM_ERROR:
                return Error{Errc::kCorrupt};
            default:
                return Error{Errc::kInternal, static_cast<std::uint16_t>(-static_cast<int>(rslt))};
        }
    }

private:
    Bus bus_{};
    // Every field, including the enums without a zero value, is assigned by the constructor.
    bma4_dev api_{}; // NOLINT(bugprone-invalid-enum-default-initialization)
};

Result<std::uint8_t> read_register(hal::I2cDevice& dev, std::uint8_t reg) noexcept {
    std::array<std::uint8_t, 1> value{};
    const Status st = dev.read_registers(reg, value);
    if (!st) {
        return st.error();
    }
    return value[0];
}

/// Reads CHIP_ID, retrying while the part NACKs (still booting) [R1 s4]. kNotFound if it answers
/// with another id: nothing may be written to a device that is not a BMA423.
Status verify_chip_id(hal::I2cDevice& dev, hal::Delay& delay) noexcept {
    Error last{Errc::kIo};
    for (std::uint8_t attempt = 0; attempt < tuning::kChipIdAttempts; ++attempt) {
        if (attempt != 0U) {
            delay.delay_ms(tuning::kChipIdRetryMs);
        }
        const Result<std::uint8_t> id = read_register(dev, tuning::kRegChipId);
        if (id) {
            return *id == kChipId ? ok() : Status{Error{Errc::kNotFound, *id}};
        }
        last = id.error();
    }
    return last;
}

Status set_accel(Session& s, std::uint8_t odr, std::uint8_t averaging) noexcept {
    bma4_accel_config cfg{};
    cfg.odr = odr;
    cfg.bandwidth = averaging;
    cfg.perf_mode = BMA4_DISABLE; // low-power mode: the only one that fits the sleep budget
    cfg.range = tuning::kAccelRange4g;
    return s.check(bma4_set_accel_config(&cfg, &s.api()));
}

/// Read-modify-write of INT1_IO_CTRL so the configured polarity survives (and survives attach()).
Status set_int1_output(Session& s, bool enabled) noexcept {
    bma4_int_pin_config pin{};
    QZ_RETURN_IF_ERROR(s.check(bma4_get_int_pin_config(&pin, 0, &s.api())));
    pin.edge_ctrl = BMA4_LEVEL_TRIGGER;
    pin.od = BMA4_PUSH_PULL; // no board pull on INT lines [R1 s6]
    pin.output_en = enabled ? BMA4_OUTPUT_ENABLE : BMA4_OUTPUT_DISABLE;
    pin.input_en = BMA4_INPUT_DISABLE;
    return s.check(bma4_set_int_pin_config(&pin, 0, &s.api()));
}

/// Double-tap on INT1, enable order: ODR (tap needs 200 Hz), feature, latch, map, then the pin
/// last so it never fires half-configured [R1 s6, s10].
Status enable_tap(Session& s) noexcept {
    bma4_dev& api = s.api();
    QZ_RETURN_IF_ERROR(set_accel(s, tuning::kTapOdr, tuning::kTapAveraging));
    QZ_RETURN_IF_ERROR(s.check(bma423_feature_enable(BMA423_DOUBLE_TAP, BMA4_ENABLE, &api)));
    QZ_RETURN_IF_ERROR(s.check(bma4_set_interrupt_mode(BMA4_LATCH_MODE, &api)));
    QZ_RETURN_IF_ERROR(s.check(bma423_map_interrupt(0, BMA423_DOUBLE_TAP_INT, BMA4_ENABLE, &api)));
    return set_int1_output(s, true);
}

/// The reverse of enable_tap(): pin first, then unmap, feature off, back to the cheap ODR.
Status disable_tap(Session& s) noexcept {
    bma4_dev& api = s.api();
    QZ_RETURN_IF_ERROR(set_int1_output(s, false));
    QZ_RETURN_IF_ERROR(s.check(bma423_map_interrupt(0, BMA423_DOUBLE_TAP_INT, BMA4_DISABLE, &api)));
    QZ_RETURN_IF_ERROR(s.check(bma423_feature_enable(BMA423_DOUBLE_TAP, BMA4_DISABLE, &api)));
    QZ_RETURN_IF_ERROR(set_accel(s, tuning::kStepOdr, tuning::kStepAveraging));
    std::uint8_t cleared = 0; // a latched status must not outlive the feature
    return s.check(bma4_read_regs(tuning::kRegIntStatus0, &cleared, 1, &api));
}

Status apply_tap(Session& s, bool enabled) noexcept {
    return enabled ? enable_tap(s) : disable_tap(s);
}

/// All configuration, done in configuration mode (adv_power_save = 0) [R1 s8].
Status configure(Session& s, const Config& config) noexcept {
    bma4_dev& api = s.api();
    QZ_RETURN_IF_ERROR(set_accel(s, tuning::kStepOdr, tuning::kStepAveraging));
    QZ_RETURN_IF_ERROR(s.check(bma4_set_accel_enable(BMA4_ENABLE, &api)));

    bma423_axes_remap remap{};
    remap.x_axis = config.remap.x_axis;
    remap.x_axis_sign = config.remap.x_sign;
    remap.y_axis = config.remap.y_axis;
    remap.y_axis_sign = config.remap.y_sign;
    remap.z_axis = config.remap.z_axis;
    remap.z_axis_sign = config.remap.z_sign;
    QZ_RETURN_IF_ERROR(s.check(bma423_set_remap_axes(&remap, &api)));

    QZ_RETURN_IF_ERROR(s.check(bma423_feature_enable(BMA423_STEP_CNTR, BMA4_ENABLE, &api)));
    // Watermark stays 0 and no step interrupt is mapped: Quartz reads the counter on wake [R1 s5].

    // INT1: output disabled until tap is on, but the polarity is always programmed [R1 s6].
    bma4_int_pin_config pin{};
    pin.lvl = config.int1_active_low ? BMA4_ACTIVE_LOW : BMA4_ACTIVE_HIGH;
    pin.edge_ctrl = BMA4_LEVEL_TRIGGER;
    pin.od = BMA4_PUSH_PULL;
    pin.output_en = BMA4_OUTPUT_DISABLE;
    pin.input_en = BMA4_INPUT_DISABLE;
    QZ_RETURN_IF_ERROR(s.check(bma4_set_int_pin_config(&pin, 0, &api)));
    if (config.tap_interrupt) {
        QZ_RETURN_IF_ERROR(apply_tap(s, true));
    }

    // Discard the power-up flag and any stale interrupt (both clear on read) [R1 s4].
    std::array<std::uint8_t, 2> discard{};
    QZ_RETURN_IF_ERROR(s.check(bma4_read_regs(tuning::kRegEvent, discard.data(), 1, &api)));
    return s.check(bma4_read_regs(tuning::kRegIntStatus0, discard.data(), 1, &api));
}

Status check_engine(hal::I2cDevice& dev) noexcept {
    const Result<std::uint8_t> status = read_register(dev, tuning::kRegInternalStatus);
    if (!status) {
        return status.error();
    }
    if ((*status & tuning::kInternalStatusMask) != tuning::kInternalStatusInitOk) {
        return Error{Errc::kCorrupt, *status};
    }
    if ((*status & tuning::kInternalStatusFlagMask) != 0U) {
        QZ_LOGW(kTag, "feature engine flags set: INTERNAL_STATUS=0x%02x", *status);
    }
    return ok();
}

} // namespace

Accelerometer::Accelerometer(hal::I2cDevice& dev, hal::Delay& delay) noexcept
    : dev_(dev), delay_(delay) {}

Status Accelerometer::init(const Config& config) noexcept {
    QZ_RETURN_IF_ERROR(verify_chip_id(dev_, delay_));

    Session s(dev_, delay_);
    bma4_dev& api = s.api();
    // Soft reset first: the once-per-reset INIT_CTRL rule makes a clean slate the only safe start
    // after an MCU reset that did not cut the sensor's power [R1 s3, s4].
    QZ_RETURN_IF_ERROR(s.check(bma4_set_command_register(tuning::kSoftResetCommand, &api)));
    delay_.delay_ms(tuning::kSoftResetWaitMs);
    QZ_RETURN_IF_ERROR(verify_chip_id(dev_, delay_));
    QZ_RETURN_IF_ERROR(s.check(bma423_init(&api)));

    // Config mode needs only 2 us between writes; the API selects that through perf_mode_status.
    // 288 writes at 450 us each would add 130 ms. bma4_set_accel_config() later resets the flag.
    api.perf_mode_status = BMA4_ENABLE;
    QZ_RETURN_IF_ERROR(s.check(bma423_write_config_file(&api)));
    // The API leaves adv_power_save on; all configuration is done with it off, then it goes on
    // last.
    QZ_RETURN_IF_ERROR(s.check(bma4_set_advance_power_save(BMA4_DISABLE, &api)));

    QZ_RETURN_IF_ERROR(configure(s, config));

    QZ_RETURN_IF_ERROR(s.check(bma4_set_advance_power_save(BMA4_ENABLE, &api)));
    return check_engine(dev_);
}

Status Accelerometer::attach() noexcept {
    // Read-only by design: the feature engine kept running through deep sleep [R1 s4].
    const Result<std::uint8_t> id = chip_id();
    if (!id) {
        return id.error();
    }
    return *id == kChipId ? ok() : Status{Error{Errc::kNotFound, *id}};
}

Result<std::uint8_t> Accelerometer::chip_id() noexcept {
    return read_register(dev_, tuning::kRegChipId);
}

Result<std::uint32_t> Accelerometer::step_count() noexcept {
    Session s(dev_, delay_);
    std::uint32_t steps = 0;
    QZ_RETURN_IF_ERROR(s.check(bma423_step_counter_output(&steps, &s.api())));
    return steps;
}

Status Accelerometer::reset_step_counter() noexcept {
    Session s(dev_, delay_);
    return s.check(bma423_reset_step_counter(&s.api()));
}

Status Accelerometer::set_tap_interrupt(bool enabled) noexcept {
    Session s(dev_, delay_);
    return apply_tap(s, enabled);
}

Result<std::uint8_t> Accelerometer::read_int_status() noexcept {
    const Result<std::uint8_t> raw = read_register(dev_, tuning::kRegIntStatus0);
    if (!raw) {
        return raw.error();
    }
    std::uint8_t status = 0;
    if ((*raw & tuning::kDoubleTapIntMask) != 0U) {
        status |= static_cast<std::uint8_t>(IntStatus::kDoubleTap);
    }
    if ((*raw & static_cast<std::uint8_t>(~tuning::kDoubleTapIntMask)) != 0U) {
        status |= static_cast<std::uint8_t>(IntStatus::kOther);
    }
    return status;
}

Result<bool> Accelerometer::feature_engine_ok() noexcept {
    const Result<std::uint8_t> status = read_register(dev_, tuning::kRegInternalStatus);
    if (!status) {
        return status.error();
    }
    return (*status & tuning::kInternalStatusMask) == tuning::kInternalStatusInitOk;
}

} // namespace qz::bma423
