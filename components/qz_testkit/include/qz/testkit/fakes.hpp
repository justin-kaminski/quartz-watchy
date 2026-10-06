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

/// Why FakeEpdPanel rejected a bus operation (Error{kInvalidState, detail = the enumerator}).
/// Every violation also bumps violation_count() and is kept in last_violation(). Rules come from
/// docs/research/ssd1681.md section 10.
enum class EpdViolation : std::uint8_t {
    kNone = 0,
    kWhileBusy,               ///< command/data/read while BUSY high [s10 rule 2]
    kWhileAsleep,             ///< any SPI traffic after 0x10 before a HW reset [rule 3]
    kUnknownCommand,          ///< not in the modelled command set (s4)
    kOtpProgramCommand,       ///< 08/09/0A/2A/30/36/39 OTP programming [rule 9]
    kWrongParamCount,         ///< too few or too many parameter bytes [rule 5]
    kBadParameter,            ///< out-of-range or reserved-bit value [rule 6]
    kDataWithoutCommand,      ///< data byte with no command expecting it
    kRamWriteOverflow,        ///< more RAM bytes than the window holds [rule 7]
    kRamWriteIncomplete,      ///< RAM stream ended before the window was full [rule 7]
    kCounterNotSet,           ///< RAM write without 0x4E/0x4F since the last window/stream [rule 7]
    kCounterNotAtWindowStart, ///< counters not at the window's start corner (model limit)
    kNoUpdateControl,         ///< 0x20 without a fresh 0x22 (POR FF use) [rule 8]
    kUndocumentedUpdateValue, ///< 0x22 value outside the 12 documented ones [s5]
    kNoTemperatureSource,     ///< temperature needed but never loaded/written [rule 8]
    kExternalSensor,          ///< 0x18 = 48 on a board with TSCL/TSDA unconnected [rule 9]
    kNoLut,                   ///< C7/CF without any LUT loaded since reset [s5]
    kRamNotWritten,      ///< display update with a RAM plane never written since reset [rule 8]
    kBusyWaitAfterSleep, ///< wait_idle() after 0x10: BUSY never falls [s2]
    kBadReadLength,      ///< read() length differs from the read command's byte count
    kReadWithoutCommand, ///< read() with no read command pending
};

/// Pseudo command code logged for a hardware reset (all real commands are < 0x100).
inline constexpr std::uint16_t kEpdLogHardwareReset = 0x100;

/// One accepted command: parameters (first 160 bytes) and the total byte count that followed it.
struct EpdLogEntry {
    std::uint16_t cmd = 0;
    std::vector<std::uint8_t> params;
    std::uint32_t data_len = 0;
    bool operator==(const EpdLogEntry&) const = default;
};

/// SSD1681 model: decodes the command protocol (rejecting unknown, out-of-order, mis-sized and
/// out-of-range traffic, see EpdViolation), keeps both RAM planes with window/address counters,
/// models BUSY against the VirtualClock, deep sleep (only hardware_reset() wakes it; RAM contents
/// are scrambled by a reset: retention is NOT modelled, R1 s1) and the panel image: a full update
/// (0x22 F7/C7) shows BW RAM exactly, a partial one (FF/CF) changes only pixels where RED (old)
/// differs from BW (new) [ssd1681.md s9.3, ASSUMED]. RAM bit order D7 = leftmost, polarity
/// 1 = white; displayed() and ram_*_image() are in gfx polarity (1 = black ink).
/// Updates outside 0..50 C (sensed) are refused (R1 s6 precaution) unless a custom LUT was written.
class FakeEpdPanel final : public hal::EpdBus {
public:
    explicit FakeEpdPanel(VirtualClock& clock);
    explicit FakeEpdPanel(VirtualClock&&) = delete; // would dangle
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
    /// Error injection: the next wait for BUSY times out (BUSY is stuck high until the next
    /// hardware_reset()). Applies to the current busy period if one is running, else to the next.
    void fail_next_busy_wait();

    // Additions (WP-07).
    /// Raw RAM planes in controller polarity (1 = white): 5000 bytes, 25 per row.
    [[nodiscard]] std::span<const std::uint8_t> ram_bw() const;
    [[nodiscard]] std::span<const std::uint8_t> ram_red() const;
    /// RAM planes rendered as images (1 = black ink), e.g. for PNG or comparison with a frame.
    [[nodiscard]] gfx::Framebuffer ram_bw_image() const;
    [[nodiscard]] gfx::Framebuffer ram_red_image() const;
    Status encode_displayed_png(gfx::ByteSink& out) const;
    Status encode_ram_bw_png(gfx::ByteSink& out) const;
    /// Accepted commands in order (plus kEpdLogHardwareReset markers); rejected ones are absent.
    [[nodiscard]] const std::vector<EpdLogEntry>& log() const;
    void clear_log();
    [[nodiscard]] std::uint32_t violation_count() const;
    [[nodiscard]] EpdViolation last_violation() const;
    [[nodiscard]] std::uint32_t hardware_resets() const;
    [[nodiscard]] std::uint32_t soft_resets() const;
    [[nodiscard]] std::uint32_t deep_sleep_entries() const;
    [[nodiscard]] std::uint32_t refused_updates() const;      ///< temperature outside 0..50 C
    [[nodiscard]] std::uint32_t aborted_updates() const;      ///< hardware reset while updating
    void set_temperature_dc(std::int16_t temp_dc);            ///< sensed temperature, default 230
    [[nodiscard]] std::uint16_t temperature_register() const; ///< 12-bit 0x1A value, POR 0x7FF
    /// Bytes returned by read command 0x2D (A..K, 11 bytes) [ssd1681.md s4].
    void set_otp_display_option(const std::array<std::uint8_t, 11>& bytes);
    /// BUSY durations in virtual microseconds (defaults: full 2 s, partial 0.26 s [GD-SPEC 7-2],
    /// load-only sequences 100 ms, SW reset 10 ms [ASSUMED]).
    void set_busy_durations_us(std::int64_t full_us, std::int64_t partial_us);

private:
    struct Window {
        std::uint16_t xsa = 0;
        std::uint16_t xea = 0;
        std::uint16_t ysa = 0;
        std::uint16_t yea = 0;
    };

    void settle() const;
    Status violate(EpdViolation v);
    void por_registers();
    Status finish_pending();
    Status apply_params();
    Status check_activation(std::uint8_t v);
    Status activate();
    Status start_ram_stream();
    void begin_busy(std::int64_t duration_us);
    void log_param_bytes(std::span<const std::uint8_t> bytes);

    VirtualClock* clock_; ///< never null: set from the constructor's reference
    std::array<std::array<std::uint8_t, gfx::kFrameBytes>, 2> ram_{}; ///< [0] BW 0x24, [1] RED 0x26
    std::array<bool, 2> ram_written_{};
    mutable gfx::Framebuffer displayed_{};
    mutable bool pending_commit_ = false;
    mutable gfx::Framebuffer pending_image_{};
    mutable bool pending_is_full_ = false;
    mutable std::uint32_t full_updates_ = 0;
    mutable std::uint32_t partial_updates_ = 0;
    std::int64_t busy_until_us_ = 0; ///< elapsed_us() at which BUSY falls; INT64_MAX = stuck
    bool stick_next_busy_ = false;
    bool asleep_ = false;
    // Command decoding.
    int pending_cmd_ = -1;
    std::size_t expected_params_ = 0;
    std::vector<std::uint8_t> params_;
    std::size_t stream_plane_ = 0;
    bool stream_started_ = false;
    std::size_t stream_count_ = 0;
    std::size_t stream_limit_ = 0;
    // Registers (POR in por_registers()).
    Window window_{};
    std::uint8_t entry_mode_ = 0x03;
    std::uint8_t counter_x_ = 0;
    std::uint16_t counter_y_ = 0;
    bool counter_x_set_ = false;
    bool counter_y_set_ = false;
    std::uint8_t sensor_select_ = 0x48;
    std::uint8_t update_ctrl2_ = 0xFF;
    bool ctrl2_written_ = false;
    std::uint16_t temp_register_ = 0x7FF;
    bool temperature_ready_ = false;
    bool lut_ready_ = false;
    bool lut_manual_ = false;
    std::int16_t sensed_dc_ = 230;
    std::array<std::uint8_t, 11> otp_option_{
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x01, 0x00, 0x00, 0x00};
    std::int64_t full_us_ = 2'000'000;
    std::int64_t partial_us_ = 260'000;
    // Diagnostics.
    std::vector<EpdLogEntry> log_;
    std::uint32_t violations_ = 0;
    EpdViolation last_violation_ = EpdViolation::kNone;
    std::uint32_t hw_resets_ = 0;
    std::uint32_t sw_resets_ = 0;
    std::uint32_t sleeps_ = 0;
    std::uint32_t refused_ = 0;
    std::uint32_t aborted_ = 0;
    std::uint32_t scramble_state_ = 0x9E3779B9U;
};

/// BMA423 register model: chip id, config-blob upload sequence, INTERNAL_STATUS, step counter
/// registers, interrupt status; steps added by tests.
class FakeBma423 final : public hal::I2cDevice {
public:
    /// Without a clock the model has no notion of time: init_ok appears immediately and idle-time
    /// rules are not checked. With one, INTERNAL_STATUS reads 0 until 140 ms after INIT_CTRL=1 and
    /// accesses closer than 450 us to a write while adv_power_save = 1 count as violations.
    FakeBma423();
    explicit FakeBma423(const VirtualClock& clock);
    explicit FakeBma423(const VirtualClock&&) = delete; // would dangle
    Status read_registers(std::uint8_t reg, std::span<std::uint8_t> out) override;
    Status write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) override;
    /// Steps the feature engine counts; dropped unless the engine runs with en_counter set.
    void add_steps(std::uint32_t n);
    void sensor_reset(); ///< power loss: counter -> 0, config lost
    /// Latches the double-tap flag; ignored unless the engine runs with the feature enabled.
    void trigger_double_tap();
    void fail_io(bool fail); ///< every access fails with kIo while true (counted, not applied)

    // Additions (WP-08).
    void set_chip_id(std::uint8_t id); ///< CHIP_ID answered from now on (survives resets)
    void fail_config_load(bool fail);  ///< INIT_CTRL=1 then reports init error (0x02)
    void raise_feature_interrupt(std::uint8_t status_0_bits);   ///< latch bits in INT_STATUS_0
    [[nodiscard]] std::uint8_t reg(std::uint8_t address) const; ///< raw register, no side effects
    [[nodiscard]] std::uint8_t feature_byte(std::size_t offset) const; ///< FEATURES_IN byte
    [[nodiscard]] std::uint32_t read_calls() const;  ///< read_registers() calls, failed included
    [[nodiscard]] std::uint32_t write_calls() const; ///< write_registers() calls, failed included
    [[nodiscard]] std::uint32_t config_uploads() const;      ///< INIT_CTRL=1 after a full load
    [[nodiscard]] std::uint32_t config_chunks() const;       ///< accepted bursts to FEATURES_IN
    [[nodiscard]] std::size_t config_bytes_received() const; ///< distinct blob bytes loaded
    [[nodiscard]] std::uint32_t config_crc32() const;        ///< CRC-32 of the 6144-byte image
    [[nodiscard]] bool engine_running() const;               ///< INTERNAL_STATUS init_ok
    [[nodiscard]] std::uint32_t soft_resets() const;         ///< CMD 0xB6 commands
    [[nodiscard]] std::uint32_t step_counter() const;        ///< hardware counter value
    /// Datasheet rules the host broke: odd/overrunning config bursts, feature access with
    /// adv_power_save on, INIT_CTRL=1 on a running engine, (with a clock) idle-time violations.
    [[nodiscard]] std::uint32_t protocol_violations() const;
    /// INT1 pin: output enabled and a mapped feature interrupt latched.
    [[nodiscard]] bool int1_active() const;
    /// Electrical level of INT1 while its output is enabled (active-low pins idle high); false
    /// when the output is disabled.
    [[nodiscard]] bool int1_level_high() const;

private:
    void power_on_reset();
    void check_idle(bool is_write);
    void read_feature(std::span<std::uint8_t> out);
    std::uint8_t read_one(std::uint8_t address);
    void write_feature(std::span<const std::uint8_t> data);
    void write_register(std::uint8_t address, std::uint8_t value);

    const VirtualClock* clock_ = nullptr;
    std::array<std::uint8_t, 0x80> regs_{};
    std::array<std::uint8_t, 6144> config_{};
    std::array<bool, 3072> word_loaded_{}; ///< per 16-bit word of the config image
    std::array<std::uint8_t, 70> features_{};
    std::uint8_t chip_id_ = 0x13;
    std::uint8_t internal_status_ = 0;
    std::int64_t init_ready_us_ = 0; ///< INTERNAL_STATUS shows 0 before this (clock mode)
    std::int64_t last_write_us_ = -1;
    std::uint32_t counter_ = 0;
    bool loading_ = false; ///< INIT_CTRL = 0: FEATURES_IN accepts the config image
    bool engine_ = false;
    bool fail_io_ = false;
    bool fail_load_ = false;
    std::uint32_t reads_ = 0;
    std::uint32_t writes_ = 0;
    std::uint32_t uploads_ = 0;
    std::uint32_t chunks_ = 0;
    std::uint32_t soft_resets_ = 0;
    std::uint32_t violations_ = 0;
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
    /// Mutations since construction: every set_* call (even with an unchanged value, so redundant
    /// caller writes are caught) and every erase that removed an entry. commit() is not counted.
    [[nodiscard]] std::uint32_t write_count() const;
    [[nodiscard]] std::uint32_t commit_count() const; ///< commit() calls since construction
    /// Number of entries currently stored in namespace `ns`.
    [[nodiscard]] std::size_t entry_count(std::string_view ns) const;
    /// True if `needle` occurs in any stored value or key name ("ns/key"). Credential-leak checks.
    [[nodiscard]] bool contains_text(std::string_view needle) const;
    /// Fault injection: after `writes` further successful mutations every set_*/erase_* fails with
    /// Errc::kIo. Negative = never fail (default).
    void fail_writes_after(std::int32_t writes);

private:
    std::map<std::string, std::vector<std::uint8_t>> entries_; // "ns/key" -> tag byte + payload
    std::uint32_t writes_ = 0;
    std::uint32_t commits_ = 0;
    std::int32_t fail_after_ = -1;
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

/// One recorded FakeNetStack call.
enum class NetCall : std::uint8_t { kConnect, kSntp, kHttp, kShutdown };

/// Scriptable network. Each script_* call appends one scripted outcome for the next call of that
/// kind; when the queue runs dry the last outcome repeats (sticky), and before any script the
/// defaults apply (connect ok, SNTP ok with VirtualClock ground truth, HTTP 200 with empty body;
/// zero latency). A call advances the VirtualClock by its latency, capped at the caller's timeout:
/// latency > timeout_ms returns Errc::kTimeout after advancing by timeout_ms (like a real
/// blocking call). connect() increments radio_init_count() every time (esp_wifi_init runs even if
/// association then fails) and turns the radio on until shutdown(). Calling sntp_sync/https_get
/// with the radio off, or connect() twice without shutdown(), is a caller bug: it fails with
/// Errc::kInvalidState and bumps violations().
class FakeNetStack final : public hal::NetStack {
public:
    explicit FakeNetStack(VirtualClock& clock);
    explicit FakeNetStack(VirtualClock&&) = delete; // would dangle
    Status connect(const hal::WifiCredentials& creds, std::uint32_t timeout_ms) override;
    Result<std::int64_t> sntp_sync(std::uint32_t timeout_ms, std::int64_t* rtc_us_at_utc) override;
    Result<hal::HttpResponse>
    https_get(std::string_view url, std::span<char> body, std::uint32_t timeout_ms) override;
    void shutdown() override;
    [[nodiscard]] std::uint32_t radio_init_count() const override;
    void script_connect(Status result, std::uint32_t latency_ms);
    /// SNTP outcome: success answers the VirtualClock ground truth (true_utc_us) at the RTC instant
    /// the answer arrives; an error is returned as is.
    void script_sntp(Status result, std::uint32_t latency_ms);
    /// SNTP success with a fixed (possibly bogus) UTC answer.
    void script_sntp_utc(std::int64_t utc_us, std::uint32_t latency_ms);
    /// HTTP response with `status` and `body`; a body larger than the caller's buffer is cut and
    /// flagged truncated.
    void script_http(std::uint16_t status, std::string body, std::uint32_t latency_ms);
    /// Transport-level HTTP failure (no response).
    void script_http_error(Error error, std::uint32_t latency_ms);
    [[nodiscard]] std::uint32_t connect_calls() const;
    [[nodiscard]] std::uint32_t sntp_calls() const;
    [[nodiscard]] std::uint32_t http_calls() const;
    [[nodiscard]] std::uint32_t shutdown_calls() const;
    [[nodiscard]] std::uint32_t violations() const;
    [[nodiscard]] bool radio_on() const;
    /// Every call in order (connect, SNTP, HTTP, shutdown).
    [[nodiscard]] const std::vector<NetCall>& calls() const;
    [[nodiscard]] const hal::WifiCredentials& last_credentials() const;
    [[nodiscard]] std::uint32_t last_connect_timeout_ms() const;
    [[nodiscard]] std::uint32_t last_sntp_timeout_ms() const;
    [[nodiscard]] std::uint32_t last_http_timeout_ms() const;
    [[nodiscard]] const std::string& last_url() const;

private:
    struct Step {
        Status status;
        std::uint32_t latency_ms = 0;
        std::optional<std::int64_t> utc_us; ///< SNTP: fixed answer instead of ground truth
        std::uint16_t http_status = 200;
        std::string body;
    };
    /// Pops the next scripted step (sticky last) or returns the default.
    static Step next_step(std::deque<Step>& queue, std::optional<Step>& last);
    /// Advances the clock by min(latency, timeout); true if the latency fits the timeout.
    bool wait(std::uint32_t latency_ms, std::uint32_t timeout_ms);

    VirtualClock* clock_; ///< never null: set from the constructor's reference
    std::deque<Step> connect_q_;
    std::deque<Step> sntp_q_;
    std::deque<Step> http_q_;
    std::optional<Step> connect_last_;
    std::optional<Step> sntp_last_;
    std::optional<Step> http_last_;
    std::vector<NetCall> calls_;
    hal::WifiCredentials last_credentials_;
    std::string last_url_;
    std::uint32_t connects_ = 0;
    std::uint32_t sntps_ = 0;
    std::uint32_t https_ = 0;
    std::uint32_t shutdowns_ = 0;
    std::uint32_t violations_ = 0;
    std::uint32_t init_count_ = 0;
    std::uint32_t connect_timeout_ms_ = 0;
    std::uint32_t sntp_timeout_ms_ = 0;
    std::uint32_t http_timeout_ms_ = 0;
    bool radio_on_ = false;
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
