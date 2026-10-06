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

namespace qz::platform {

/// Owns all IDF-backed HAL objects (static storage). Created once in app_main.
class IdfPlatform {
public:
    /// Captures boot RTC time, reset reason and wake sources first, then configures GPIO, SPI,
    /// I2C, ADC (lazily where possible). NVS is initialized lazily by the KvStore.
    static Result<IdfPlatform*> init() noexcept;

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
    hal::ConsolePort& console() noexcept; ///< esp_console over USB-Serial-JTAG

private:
    IdfPlatform() = default;
};

} // namespace qz::platform
