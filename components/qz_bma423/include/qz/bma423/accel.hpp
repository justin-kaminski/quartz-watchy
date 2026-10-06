// BMA423 wrapper over the vendored Bosch BMA423 SensorAPI (third_party/bosch, BSD-3).
// Pure: the Bosch C API is driven through hal::I2cDevice + hal::Delay callbacks.
// Not thread-safe (app task).
#pragma once

#include "qz/core/result.hpp"
#include "qz/hal/delay.hpp"
#include "qz/hal/i2c.hpp"

#include <cstdint>

namespace qz::bma423 {

inline constexpr std::uint8_t kChipId = 0x13; ///< [R1/bma423.md]

/// Axis remap for the watch's mounting orientation [R1 / bring-up B7]. Values follow the Bosch
/// API remap fields (source axis 0=x 1=y 2=z, sign 0=positive 1=negative).
struct AxisRemap {
    std::uint8_t x_axis = 0, x_sign = 0;
    std::uint8_t y_axis = 1, y_sign = 0;
    std::uint8_t z_axis = 2, z_sign = 0;
};

struct Config {
    AxisRemap remap{};
    bool tap_interrupt = false;  ///< double-tap on INT1 (tap_wake setting)
    bool int1_active_low = true; ///< must match board::kAccelIntActive
};

enum class IntStatus : std::uint8_t { kNone = 0, kDoubleTap = 1U << 0U, kOther = 1U << 7U };

class Accelerometer {
public:
    Accelerometer(hal::I2cDevice& dev, hal::Delay& delay) noexcept;

    /// Cold init: soft reset, chip id check (kNotFound if wrong), config blob upload, feature
    /// enable (step counter), remap, INT1 config. About 330 ms (10 ms reset wait + 96 config bursts
    /// + the 150 ms init wait). Errc::kIo/kCorrupt on failure. Nothing is written to a device
    /// whose chip id is wrong.
    Status init(const Config& config) noexcept;
    /// Warm attach after deep sleep: no reset, only verifies chip id (fast path, < 2 ms).
    Status attach() noexcept;
    [[nodiscard]] Result<std::uint8_t> chip_id() noexcept;
    /// Hardware step counter (never reset in normal operation, ARCHITECTURE.md section 10).
    Result<std::uint32_t> step_count() noexcept;
    Status reset_step_counter() noexcept;
    Status set_tap_interrupt(bool enabled) noexcept;
    /// Reads and clears the feature interrupt status.
    Result<std::uint8_t> read_int_status() noexcept;
    /// Self-test support: feature-config load status from INTERNAL_STATUS.
    Result<bool> feature_engine_ok() noexcept;

private:
    hal::I2cDevice& dev_;
    hal::Delay& delay_;
};

} // namespace qz::bma423
