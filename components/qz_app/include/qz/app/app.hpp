// Application core (ARCHITECTURE.md sections 3, 4, 17): wake dispatcher + tether policy.
// Pure: identical on target, in the simulator and in virtual-time tests.
#pragma once

#include "qz/console/device_api.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/hal/epd_bus.hpp"
#include "qz/hal/i2c.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/net.hpp"
#include "qz/hal/system.hpp"
#include "qz/model/types.hpp"

#include <cstdint>

namespace qz::app {

/// Build-time options, filled in main/ from Kconfig (pure code never reads CONFIG_*).
struct BuildFeatures {
    bool radio = true;    ///< CONFIG_QZ_RADIO
    bool usb_wake = true; ///< CONFIG_QZ_USB_WAKE (EXT0 on USB detect)
    bool selftest_interactive = true;
};

/// Every hardware dependency, owned by main/ (or the simulator / tests).
struct Platform {
    hal::EpdBus& epd;
    hal::I2cDevice& accel;
    hal::Delay& delay;
    hal::BoardIo& io;
    hal::Adc& battery_adc;
    hal::KvStore& kv;
    hal::RtcMemory& rtc_memory;
    hal::Clock& clock;
    hal::SleepControl& sleep;
    hal::System& system;
    hal::ConsolePort& console;
    hal::NetStack* net;              ///< nullptr when the radio is compiled out
    hal::ProvisioningPortal* portal; ///< nullptr when the radio is compiled out
};

/// Pure state machine deciding whether the console may run (ARCHITECTURE.md section 17).
enum class TetherState : std::uint8_t { kUntethered = 0, kTethered, kSleepOnce, kDetaching };

class TetherPolicy {
public:
    /// Evaluated at every wake; USB must read present twice (10 ms apart) to tether.
    TetherState on_wake(bool usb_first_read, bool usb_second_read) noexcept;
    /// Periodic poll while tethered (1 s); two consecutive absent polls -> kDetaching.
    TetherState on_poll(bool usb_present) noexcept;
    /// Console `sleep <s>`: one deep sleep even though tethered.
    void request_sleep() noexcept;
    void on_detached() noexcept; ///< console stopped -> kUntethered
    [[nodiscard]] TetherState state() const noexcept { return state_; }
    /// The only gate for ConsolePort::start(). False in every untethered state.
    [[nodiscard]] bool console_allowed() const noexcept { return state_ == TetherState::kTethered; }
    [[nodiscard]] bool may_deep_sleep() const noexcept { return state_ != TetherState::kTethered; }

private:
    TetherState state_ = TetherState::kUntethered;
    std::uint8_t absent_polls_ = 0;
};

/// The application. Owns services, UI, console registry and the RTC state working copy.
/// Not thread-safe: all calls on the app task.
class App {
public:
    App(Platform& platform, const BuildFeatures& features) noexcept;
    /// Runs one complete wake (incl. interactive session) and returns the plan for deep sleep.
    /// In the tethered state it returns only when the device should sleep (USB gone or `sleep`).
    hal::SleepPlan run_wake() noexcept;
    /// Console + inspection surface (also used by the simulator and integration tests).
    [[nodiscard]] console::DeviceApi& device_api() noexcept;
    [[nodiscard]] const TetherPolicy& tether() const noexcept;

private:
    Platform& platform_;
    BuildFeatures features_;
    TetherPolicy tether_;
    // Services, UI, registry, RtcState copy: added by the app work package (fixed storage).
};

} // namespace qz::app
