// Clocks, RTC memory, sleep control and system services.
#pragma once

#include "qz/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::hal {

/// Raw RTC time base (ARCHITECTURE.md section 8.1).
class Clock {
public:
    virtual ~Clock() = default;
    /// Microseconds since power-on from the RTC timer (esp_rtc_get_time_us on target).
    /// Monotonic; survives deep sleep and software resets; restarts at 0 after power loss.
    [[nodiscard]] virtual std::int64_t rtc_us() const = 0;
    /// Pushes corrected UTC into the libc clock (settimeofday) for TLS/SNTP consumers.
    virtual void set_system_utc_us(std::int64_t utc_us) = 0;
};

/// Byte regions in RTC memory (RTC_NOINIT on target). Contents are untrusted until the owner
/// validates magic/version/CRC (app::RtcStore).
class RtcMemory {
public:
    virtual ~RtcMemory() = default;
    [[nodiscard]] virtual std::span<std::uint8_t> state_region() = 0; ///< >= sizeof(app::RtcState)
    [[nodiscard]] virtual std::span<std::uint8_t>
    frame_region() = 0; ///< >= sizeof(app::FrameShadow)
};

/// Wake sources for the next sleep. timer_us < 0 = no timer wake.
struct SleepPlan {
    std::int64_t timer_us = -1; ///< relative duration in raw RTC microseconds
    bool wake_on_buttons = true;
    bool wake_on_accel = false;
    bool wake_on_usb = true;
    bool wake_on_epd_idle = false; ///< light sleep only: BUSY falling edge
};

enum class LightSleepWake : std::uint8_t { kTimer, kButton, kAccel, kUsb, kEpdIdle, kOther };

class SleepControl {
public:
    virtual ~SleepControl() = default;
    /// Arms wake sources (EXT1 buttons/accel, EXT0 USB, timer), parks GPIOs per
    /// ARCHITECTURE.md section 5 and enters deep sleep. Never returns on target; fakes record
    /// the plan and return.
    virtual void deep_sleep(const SleepPlan& plan) = 0;
    /// Light sleep until the timer or a selected source fires. Tethered implementations wait
    /// without light sleep (USB stays up). Returns the cause.
    virtual LightSleepWake light_sleep(const SleepPlan& plan) = 0;
};

enum class ResetReason : std::uint8_t {
    kPowerOn,
    kBrownout,
    kDeepSleep,
    kSoftware,
    kPanic,
    kWatchdog,
    kUsbJtag,
    kOther
};

struct WakeSources {
    bool timer = false;
    bool ext0 = false;           ///< USB detect
    bool ext1 = false;           ///< buttons / accel INT
    std::uint64_t ext1_pins = 0; ///< GPIO bit mask that triggered EXT1
};

struct SlowClockInfo {
    bool external_crystal = false; ///< false = IDF fell back to an internal oscillator
    std::uint32_t measured_hz = 0; ///< from the IDF calibration value (0 = unknown)
};

struct FirmwareInfo {
    std::string_view version;  ///< semver from esp_app_desc / host constant
    std::string_view git_hash; ///< short hash, "-dirty" suffix allowed
    std::string_view idf_version;
};

struct HeapInfo {
    std::uint32_t free_bytes = 0;
    std::uint32_t min_free_bytes = 0;
};

class System {
public:
    virtual ~System() = default;
    [[nodiscard]] virtual ResetReason reset_reason() const = 0;
    [[nodiscard]] virtual WakeSources wake_sources() const = 0;
    /// RTC time captured as early as possible in app_main (wake latency measurement).
    [[nodiscard]] virtual std::int64_t boot_rtc_us() const = 0;
    [[nodiscard]] virtual SlowClockInfo slow_clock() const = 0;
    [[nodiscard]] virtual FirmwareInfo firmware() const = 0;
    [[nodiscard]] virtual std::uint64_t chip_id() const = 0; ///< base MAC as integer
    /// Hardware RNG (true random only while RF or the bootloader entropy source is on;
    /// adequate for jitter; provisioning passwords are generated with the radio on).
    virtual std::uint32_t random_u32() = 0;
    [[nodiscard]] virtual HeapInfo heap() const = 0;
    /// Software restart (esp_restart). Never returns on target.
    virtual void restart() = 0;
};

/// Line-oriented console transport (USB-Serial-JTAG on target, stdin/stdout in the simulator).
/// receive_line may be called from the app task only; send_line is thread-safe (one mutex
/// shared with log output) and writes the line plus '\n' atomically.
class ConsolePort {
public:
    virtual ~ConsolePort() = default;
    virtual Status start() = 0; ///< only ever called by the tether policy (USB present)
    virtual void stop() = 0;
    /// Next complete request line (without '\n'); 0 = timeout. kNoSpace if the line was too long
    /// (it is discarded).
    virtual Result<std::size_t> receive_line(std::span<char> out, std::uint32_t timeout_ms) = 0;
    virtual void send_line(std::string_view line) = 0;
};

} // namespace qz::hal
