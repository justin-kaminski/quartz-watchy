// Test doubles for every HAL interface (host tests, simulator, virtual-time sim).
// Never linked into firmware. Deterministic; time comes from VirtualClock.
#pragma once

#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/hal/epd_bus.hpp"
#include "qz/hal/i2c.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/net.hpp"
#include "qz/hal/system.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qz::testkit {

/// Virtual RTC + libc clock. advance() moves time; drift_ppb simulates a fast/slow crystal.
///
/// Time model (all integer, exact): true time T passes in whole microseconds via advance_us()
/// (also delay_us/delay_ms). The RTC counts T * (1e9 + ppb) / 1e9 microseconds; the sub-microsecond
/// remainder is carried between calls, so the RTC total depends only on the total true time, not
/// on how it was chopped into steps. The libc clock (set_system_utc_us) ticks with the RTC.
class VirtualClock final : public hal::Clock, public hal::Delay {
public:
    [[nodiscard]] std::int64_t rtc_us() const override;
    void set_system_utc_us(std::int64_t utc_us) override;
    void delay_us(std::uint32_t us) override;
    void delay_ms(std::uint32_t ms) override;
    void advance_us(std::int64_t us);
    void set_true_utc_us(std::int64_t utc_us);      ///< ground truth for SNTP fakes
    [[nodiscard]] std::int64_t true_utc_us() const; ///< ground truth now
    void set_crystal_error_ppb(std::int32_t ppb);   ///< RTC runs fast (+) / slow (-)
    void power_loss();                              ///< rtc_us restarts at 0

    // Additions (WP-01): what sleep fakes, device models and TimeKeeper tests need on top of this.
    /// Advances true time by the fewest whole microseconds after which the RTC has counted at least
    /// `rtc_delta_us` more microseconds. This is how a SleepPlan::timer_us (raw RTC time) becomes
    /// elapsed virtual time under crystal error.
    void advance_rtc_us(std::int64_t rtc_delta_us);
    /// Ideal time since construction: unaffected by set_true_utc_us(), power_loss() and crystal
    /// error. Device models (BUSY, vibration) measure durations with it.
    [[nodiscard]] std::int64_t elapsed_us() const;
    /// What the libc clock reads now: the last set_system_utc_us() value plus the RTC time since
    /// then (0 + RTC time since power-on before the first set, like an unset device).
    [[nodiscard]] std::int64_t system_utc_us() const;

private:
    std::int64_t rtc_us_ = 0;
    std::int64_t true_utc_us_ = 0;
    std::int64_t system_utc_us_ = 0;
    std::int32_t error_ppb_ = 0;
    std::int64_t elapsed_us_ = 0;
    std::int64_t rtc_remainder_ = 0; ///< sub-microsecond RTC remainder in 1e-9 us, in [0, 1e9)
};

/// SSD1681 model: decodes the command protocol, keeps both RAM planes, BUSY timing, deep-sleep
/// state and an update log; displayed() is what a viewer would see after the last update.
class FakeEpdPanel final : public hal::EpdBus {
public:
    explicit FakeEpdPanel(VirtualClock& clock);
    Status hardware_reset() override;
    Status command(std::uint8_t cmd) override;
    Status data(std::span<const std::uint8_t> bytes) override;
    Status read(std::span<std::uint8_t> out) override;
    [[nodiscard]] bool busy() const override;
    Status wait_idle(std::uint32_t timeout_ms) override;
    [[nodiscard]] const gfx::Framebuffer& displayed() const;
    [[nodiscard]] std::uint32_t full_updates() const;
    [[nodiscard]] std::uint32_t partial_updates() const;
    [[nodiscard]] bool in_deep_sleep() const;
    void fail_next_busy_wait(); ///< error injection
};

/// BMA423 register model: chip id, config-blob upload sequence, INTERNAL_STATUS, step counter
/// registers, interrupt status; steps added by tests.
class FakeBma423 final : public hal::I2cDevice {
public:
    Status read_registers(std::uint8_t reg, std::span<std::uint8_t> out) override;
    Status write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) override;
    void add_steps(std::uint32_t n);
    void sensor_reset(); ///< power loss: counter -> 0, config lost
    void trigger_double_tap();
    void fail_io(bool fail);
};

/// Scriptable buttons, USB/charge pins, battery ADC pin and vibration motor. Starts with no button
/// pressed, USB absent and the ADC pin at 2800 mV (a mid-charge cell behind the divider).
/// press()/release() take hal::kButtonBit* masks; set_usb() rejects "charging without USB".
class FakeBoardIo final : public hal::BoardIo, public hal::Adc {
public:
    FakeBoardIo() = default;
    /// With a clock, vibration pulses are timed in virtual time (vibration_ms_total()); without one
    /// the pulses are counted but have no duration.
    explicit FakeBoardIo(const VirtualClock& clock) : clock_(&clock) {}
    explicit FakeBoardIo(const VirtualClock&&) = delete; // would dangle
    [[nodiscard]] std::uint8_t pressed_buttons() const override;
    [[nodiscard]] bool usb_present() const override;
    [[nodiscard]] bool charging() const override;
    void set_vibration(bool on) override;
    Result<std::uint16_t> read_pin_mv() override;
    void press(std::uint8_t mask);
    void release(std::uint8_t mask);
    void set_usb(bool present, bool charging);
    void set_pin_mv(std::uint16_t mv);
    /// Milliseconds the motor has been on (a pulse still running counts up to now), truncated.
    [[nodiscard]] std::uint32_t vibration_ms_total() const;

    // Additions (WP-01).
    [[nodiscard]] bool vibrating() const; ///< motor on now (it must be off before sleeping)
    [[nodiscard]] std::uint32_t vibration_pulses() const; ///< off -> on transitions
    void fail_adc(bool fail);                             ///< read_pin_mv() returns kIo while true
    [[nodiscard]] std::uint32_t adc_reads() const; ///< read_pin_mv() calls, failed ones included

private:
    [[nodiscard]] std::int64_t now_us() const;

    const VirtualClock* clock_ = nullptr;
    std::uint8_t pressed_ = 0;
    bool usb_ = false;
    bool charging_ = false;
    bool vibrating_ = false;
    std::int64_t vibration_started_us_ = 0;
    std::int64_t vibration_total_us_ = 0;
    std::uint32_t vibration_pulses_ = 0;
    std::uint16_t pin_mv_ = 2800;
    bool adc_fails_ = false;
    std::uint32_t adc_reads_ = 0;
};

/// In-memory NVS with per-namespace write counters (flash-wear assertions).
class FakeKvStore final : public hal::KvStore {
public:
    Result<std::uint32_t> get_u32(std::string_view ns, std::string_view key) override;
    Status set_u32(std::string_view ns, std::string_view key, std::uint32_t value) override;
    Result<std::int32_t> get_i32(std::string_view ns, std::string_view key) override;
    Status set_i32(std::string_view ns, std::string_view key, std::int32_t value) override;
    Result<std::int64_t> get_i64(std::string_view ns, std::string_view key) override;
    Status set_i64(std::string_view ns, std::string_view key, std::int64_t value) override;
    Result<std::size_t>
    get_str(std::string_view ns, std::string_view key, std::span<char> out) override;
    Status set_str(std::string_view ns, std::string_view key, std::string_view value) override;
    Result<std::size_t>
    get_blob(std::string_view ns, std::string_view key, std::span<std::uint8_t> out) override;
    Status set_blob(std::string_view ns,
                    std::string_view key,
                    std::span<const std::uint8_t> value) override;
    Status erase_key(std::string_view ns, std::string_view key) override;
    Status erase_namespace(std::string_view ns) override;
    Status commit() override;
    [[nodiscard]] std::uint32_t write_count() const;
    [[nodiscard]] bool contains_text(std::string_view needle) const; ///< credential-leak checks

private:
    std::map<std::string, std::vector<std::uint8_t>> entries_; // "ns/key" -> typed bytes
    std::uint32_t writes_ = 0;
};

/// Two fixed byte regions that start zeroed and keep their content across "wakes" (the object
/// lives as long as the test). scramble() models a power loss: deterministic noise in both.
/// The arrays are 16-byte aligned like RTC_NOINIT objects, so code may view them as structs.
class FakeRtcMemory final : public hal::RtcMemory {
public:
    std::span<std::uint8_t> state_region() override;
    std::span<std::uint8_t> frame_region() override;
    void scramble(); ///< power loss: random bytes

private:
    alignas(16) std::array<std::uint8_t, 2048> state_{};
    alignas(16) std::array<std::uint8_t, 5120> frame_{};
    std::uint64_t scramble_state_ = 0; ///< splitmix64 state: successive scrambles differ
};

/// Records sleep plans; deep_sleep() returns (the harness advances VirtualClock to the next
/// wake and reports the cause). light_sleep() advances virtual time to the earliest event.
///
/// Semantics (WP-01):
///  - deep_sleep() only records the plan (and rejects a plan with no wake source at all, which
///    would hang the watch). It does not move time and does not change reset_reason(): the
///    harness advances the clock (VirtualClock::advance_rtc_us(plan.timer_us)) and then calls
///    set_wake(), which also latches boot_rtc_us() from the clock (that is where the harness
///    models boot latency: advance first, then set_wake).
///  - light_sleep() has one event source, the timer: it advances virtual time until the RTC has
///    counted plan.timer_us and returns kTimer. A plan without a timer would sleep forever in a
///    fake with no scripted input, so it is rejected (QZ_ASSERT); scripted button/USB events
///    belong to the simulation harness, which wraps the sleep control.
///  - restart() only counts; the harness performs the "reboot" (usually set_wake(kSoftware, {})).
///  - Before the first set_wake(): reset reason kPowerOn, no wake source, boot_rtc_us() = the
///    clock's RTC at construction.
class FakeSleepSystem final : public hal::SleepControl, public hal::System {
public:
    explicit FakeSleepSystem(VirtualClock& clock);
    explicit FakeSleepSystem(VirtualClock&&) = delete; // would dangle
    void deep_sleep(const hal::SleepPlan& plan) override;
    hal::LightSleepWake light_sleep(const hal::SleepPlan& plan) override;
    [[nodiscard]] hal::ResetReason reset_reason() const override;
    [[nodiscard]] hal::WakeSources wake_sources() const override;
    [[nodiscard]] std::int64_t boot_rtc_us() const override;
    [[nodiscard]] hal::SlowClockInfo slow_clock() const override; ///< external crystal, 32768 Hz
    [[nodiscard]] hal::FirmwareInfo firmware() const override;    ///< "0.0.0-host", "host"
    [[nodiscard]] std::uint64_t chip_id() const override;
    std::uint32_t random_u32() override;               ///< deterministic xorshift
    [[nodiscard]] hal::HeapInfo heap() const override; ///< 300000 free, 290000 low-water
    void restart() override;
    [[nodiscard]] std::optional<hal::SleepPlan> last_plan() const; ///< of the last deep_sleep()
    void set_wake(hal::ResetReason reason, hal::WakeSources sources);

    // Additions (WP-01).
    [[nodiscard]] std::optional<hal::SleepPlan> last_light_sleep_plan() const;
    [[nodiscard]] std::uint32_t deep_sleep_count() const;
    [[nodiscard]] std::uint32_t light_sleep_count() const;
    [[nodiscard]] std::uint32_t restart_count() const;
    void set_slow_clock(hal::SlowClockInfo info); ///< e.g. {false, 0} for the RC-fallback path
    void set_heap(hal::HeapInfo info);

private:
    VirtualClock* clock_; ///< never null: set from the constructor's reference
    std::optional<hal::SleepPlan> last_deep_plan_;
    std::optional<hal::SleepPlan> last_light_plan_;
    hal::ResetReason reset_reason_ = hal::ResetReason::kPowerOn;
    hal::WakeSources wake_sources_;
    std::int64_t boot_rtc_us_ = 0;
    hal::SlowClockInfo slow_clock_{.external_crystal = true, .measured_hz = 32768};
    hal::HeapInfo heap_{.free_bytes = 300000, .min_free_bytes = 290000};
    std::uint32_t random_state_ = 2463534242U; ///< xorshift32 state, never 0
    std::uint32_t deep_sleeps_ = 0;
    std::uint32_t light_sleeps_ = 0;
    std::uint32_t restarts_ = 0;
};

/// Scriptable network: per-call results, latency in virtual time, call counters.
class FakeNetStack final : public hal::NetStack {
public:
    explicit FakeNetStack(VirtualClock& clock);
    Status connect(const hal::WifiCredentials& creds, std::uint32_t timeout_ms) override;
    Result<std::int64_t> sntp_sync(std::uint32_t timeout_ms, std::int64_t* rtc_us_at_utc) override;
    Result<hal::HttpResponse>
    https_get(std::string_view url, std::span<char> body, std::uint32_t timeout_ms) override;
    void shutdown() override;
    [[nodiscard]] std::uint32_t radio_init_count() const override;
    void script_connect(Status result, std::uint32_t latency_ms);
    void script_http(std::uint16_t status, std::string body, std::uint32_t latency_ms);
    [[nodiscard]] std::uint32_t connect_calls() const;
    [[nodiscard]] bool radio_on() const;
};

/// Captures console output lines; feeds scripted request lines.
///
/// Semantics (WP-01): start() must precede receive_line() and may not be repeated without a
/// stop() in between (kInvalidState otherwise: those are tether-policy bugs). receive_line()
/// delivers pushed requests in order, without a terminator; an empty queue is a timeout (0), which
/// advances the optional VirtualClock by timeout_ms like a real blocking read would. A request
/// longer than the buffer is discarded and reported as kNoSpace. send_line() records every line,
/// started or not, so tests can prove that nothing was ever printed.
class FakeConsolePort final : public hal::ConsolePort {
public:
    FakeConsolePort() = default;
    /// With a clock, an empty-queue receive_line() consumes its timeout in virtual time.
    explicit FakeConsolePort(VirtualClock& clock) : clock_(&clock) {}
    explicit FakeConsolePort(VirtualClock&&) = delete; // would dangle
    Status start() override;
    void stop() override;
    Result<std::size_t> receive_line(std::span<char> out, std::uint32_t timeout_ms) override;
    void send_line(std::string_view line) override;
    void push_request(std::string line);
    [[nodiscard]] const std::vector<std::string>& sent() const;
    [[nodiscard]] std::uint32_t start_count() const; ///< start() calls, failed ones included

    // Additions (WP-01).
    [[nodiscard]] std::uint32_t stop_count() const;
    [[nodiscard]] bool running() const;                 ///< started and not yet stopped
    [[nodiscard]] std::size_t pending_requests() const; ///< pushed but not yet received
    void fail_next_start(Error error);                  ///< the next start() fails with `error`

private:
    VirtualClock* clock_ = nullptr;
    std::deque<std::string> requests_;
    std::vector<std::string> sent_;
    std::optional<Error> start_failure_;
    bool running_ = false;
    std::uint32_t starts_ = 0;
    std::uint32_t stops_ = 0;
};

} // namespace qz::testkit
