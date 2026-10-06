// ESP-IDF implementations of the qz_hal interfaces (IDF-only component). This header stays
// free of IDF includes so main/ and docs tooling can parse it anywhere.
#pragma once

#include "qz/core/result.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/hal/delay.hpp"
#include "qz/hal/epd_bus.hpp"
#include "qz/hal/i2c.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/system.hpp"

#include <cstddef>

namespace qz::platform {

/// Sizes of the RTC_NOINIT regions. The platform cannot include qz_app (layering), so they are
/// fixed here; main/ static_asserts sizeof(app::RtcState) / sizeof(app::FrameShadow) against
/// them (a growing struct fails the build there, not silently at run time). Budget
/// (ARCHITECTURE.md section 6): total RTC use <= 7.5 KiB of the 8 KiB RTC slow memory.
inline constexpr std::size_t kRtcStateRegionBytes = 1024; ///< sizeof(app::RtcState)
inline constexpr std::size_t kRtcFrameRegionBytes = 5008; ///< sizeof(app::FrameShadow) = 8 + 5000
static_assert(kRtcStateRegionBytes + kRtcFrameRegionBytes <= 7680, "RTC slow memory budget");

/// Owns all IDF-backed HAL objects (static storage). Created once in app_main.
class IdfPlatform {
public:
    /// Constructs the static HAL objects (the IdfSystem constructor captures the boot RTC time,
    /// reset reason and wake sources before anything else), then, in this order: board GPIO
    /// (vibration forced off), EPD bus (drops every deep-sleep pad hold via
    /// IdfSleep::release_holds, then brings up SPI2), accelerometer I2C. ADC and NVS initialize
    /// lazily on first use. A failed board-IO or EPD bring-up is returned as an error; a failed
    /// I2C bring-up is only logged (accel() then answers kInvalidState; the face still works).
    /// Idempotent. Call first thing in app_main.
    static Result<IdfPlatform*> init() noexcept;
    /// The static instance, valid even when init() failed (hardware may then be partly
    /// uninitialized). Lets app_main reach sleep() for a safe retry sleep after a failed init.
    static IdfPlatform& instance() noexcept;

    hal::EpdBus& epd() noexcept;
    hal::I2cDevice& accel_i2c() noexcept;
    hal::Delay& delay() noexcept;
    hal::BoardIo& io() noexcept;
    hal::Adc& battery_adc() noexcept;
    hal::KvStore& kv() noexcept;
    hal::RtcMemory& rtc_memory() noexcept;
    hal::Clock& clock() noexcept;
    hal::SleepControl& sleep() noexcept;
    hal::System& system() noexcept;
    /// USB-Serial-JTAG driver (not esp_console), line buffered. start() installs the driver and
    /// switches IdfSleep to tethered mode (no light sleep); stop() undoes both. Never started on
    /// battery: only the tether policy calls start().
    hal::ConsolePort& console() noexcept;

private:
    IdfPlatform() = default;
};

} // namespace qz::platform
