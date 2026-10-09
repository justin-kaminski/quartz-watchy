// Private wiring of the application (WP-21). The Core owns every service, the UI and the console
// registry, runs the wake flows (ARCHITECTURE.md section 4) and implements console::DeviceApi.
// Not part of the component contract: only app.hpp is public.
#pragma once

#include "qz/app/app.hpp"
#include "qz/app/rtc_state.hpp"
#include "qz/app/wake_planner.hpp"
#include "qz/bma423/accel.hpp"
#include "qz/conn/conn.hpp"
#include "qz/console/registry.hpp"
#include "qz/core/fixed_string.hpp"
#include "qz/faces/registry.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/power/power.hpp"
#include "qz/selftest/selftest.hpp"
#include "qz/settings/settings.hpp"
#include "qz/ssd1681/panel.hpp"
#include "qz/steps/step_tracker.hpp"
#include "qz/time/timekeeper.hpp"
#include "qz/ui/ui.hpp"
#include "qz/weather/provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::app {

/// Wiring constants (one table; [TUNE] = calibrate on hardware).
namespace wiring {
inline constexpr std::int64_t kUs = 1'000'000;
// RtcState::reserved[0] application flags (the layout is pinned by WP-20; the bytes are ours).
inline constexpr std::uint8_t kFlagCredsPresent = 1U << 0U; ///< Wi-Fi credentials stored in NVS
inline constexpr std::uint8_t kFlagTapArmed = 1U << 1U;     ///< BMA423 double-tap interrupt armed
// DisplayState::reserved bit: the "Charge me" screen is on the panel (shown once per Critical).
inline constexpr std::uint16_t kDisplayChargeMeShown = 1U << 0U;
inline constexpr std::size_t kBatterySamples = 16;        ///< ARCHITECTURE.md section 11
inline constexpr std::int64_t kCriticalPeekUs = 10 * kUs; ///< ARCHITECTURE.md section 11
inline constexpr std::int64_t kSessionMaxUs =
    15LL * 60 * kUs;                                       ///< hard cap of an interactive session
inline constexpr std::int64_t kPressedPollUs = 25'000;     ///< sampling period while held [TUNE]
inline constexpr std::int64_t kMinWaitUs = 1'000;          ///< shortest light-sleep timer
inline constexpr std::int64_t kReleaseWaitMaxUs = 5 * kUs; ///< wait for release before sleeping
inline constexpr std::int64_t kTetherPollUs = 1 * kUs;     ///< ARCHITECTURE.md section 17
/// Console receive slice. Buttons are only sampled between slices, so it stays below a short tap.
inline constexpr std::int64_t kTetherReceiveMaxUs = 25'000; ///< [TUNE]
inline constexpr std::int64_t kProvPollUs = 200'000;        ///< portal service period [TUNE]
inline constexpr std::uint32_t kPortalPollMs = 20;          ///< blocking time of one portal poll
/// Phone link wait slice: the loop cannot light-sleep while BLE runs, so buttons are polled.
inline constexpr std::int64_t kPhonePollUs = 25'000;     ///< [TUNE]
inline constexpr std::uint16_t kForgetVibrationMs = 120; ///< "phones forgotten" confirmation
/// How long the PhoneSync screen shows how a session ended before the face returns. [TUNE]
inline constexpr std::int64_t kPhoneResultUs = 5 * kUs;
inline constexpr std::uint16_t kGoalVibrationMs = 200;      ///< goal reached pulse [TUNE]
inline constexpr std::int16_t kTempFullRefreshDc = 100;     ///< ARCHITECTURE.md section 14: 10 C
inline constexpr std::uint16_t kProvisioningTtlS = 300;     ///< conn::Provisioning expiry
inline constexpr std::int64_t kEarlyTickWindowUs = 2 * kUs; ///< > latency_max (1.5 s) [TUNE]
inline constexpr std::size_t kRecentWakes = 8;
inline constexpr std::int32_t kDriftPersistDeltaPpb = 1000; ///< ARCHITECTURE.md section 7
inline constexpr std::uint16_t kSleepMaxS = 3600;
inline constexpr std::string_view kTimeNs = "qz_time";
inline constexpr std::string_view kDriftKey = "drift";
} // namespace wiring

[[nodiscard]] std::string_view level_name(model::PowerLevel level) noexcept;
[[nodiscard]] std::string_view indicator_name(model::SyncIndicator indicator) noexcept;
[[nodiscard]] std::string_view freshness_name(model::WeatherFreshness freshness) noexcept;
[[nodiscard]] std::string_view mode_name(model::ConnectivityMode mode) noexcept;
[[nodiscard]] std::string_view reset_name(hal::ResetReason reason) noexcept;
[[nodiscard]] std::string_view cause_name(model::WakeCause cause) noexcept;

/// Everything known about the wake in progress.
struct WakeCtx {
    model::WakeCause cause = model::WakeCause::kUnknown;
    hal::ResetReason reason = hal::ResetReason::kPowerOn;
    std::int64_t start_rtc_us = 0;
    std::int64_t boot_rtc_us = 0;
    std::uint8_t flags = 0; ///< model::WakeFlag bits
    std::uint8_t error = 0; ///< first error: Errc + 1
    std::uint16_t steps_delta = 0;
    std::uint8_t pressed_mask = 0; ///< buttons that woke the chip (EXT1)
    bool cold = false;
    bool safe = false;
    bool usb_present = false;
    bool accel_event = false;
    bool held_at_end = false;
};

class Core final : public console::DeviceApi, public conn::FormSink {
public:
    Core(Platform& platform, const BuildFeatures& features, TetherPolicy& tether) noexcept;
    ~Core() override = default;
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    Core(Core&&) = delete;
    Core& operator=(Core&&) = delete;

    hal::SleepPlan run_wake() noexcept;

    // ---- console::DeviceApi (device_api.cpp) ----
    [[nodiscard]] console::FirmwareIdentity firmware() const override;
    void write_status(console::JsonWriter& out) override;
    [[nodiscard]] console::TimeInfo time_info() const override;
    [[nodiscard]] const time::TimeZone& timezone() const override;
    Status set_time_utc(time::UnixSeconds utc) override;
    [[nodiscard]] const settings::Settings& current_settings() const override;
    Status apply_setting(settings::Key key, std::string_view value) override;
    Status reset_settings() override;
    Status inject(const model::InputEvent& event) override;
    [[nodiscard]] std::string_view current_screen() const override;
    Status show_screen(std::string_view name) override;
    [[nodiscard]] std::size_t screen_count() const override;
    [[nodiscard]] std::string_view screen_name_at(std::size_t index) const override;
    [[nodiscard]] std::size_t face_count() const override;
    [[nodiscard]] std::uint8_t face_id_at(std::size_t index) const override;
    [[nodiscard]] std::string_view face_name(std::uint8_t id) const override;
    [[nodiscard]] model::StepsSummary steps() const override;
    Status inject_steps(std::int32_t delta) override;
    Status reset_steps_today() override;
    [[nodiscard]] model::BatteryStatus battery() const override;
    Status fake_battery_mv(std::optional<std::uint16_t> mv) override;
    [[nodiscard]] console::WeatherInfo weather() const override;
    Status fake_weather(const model::WeatherReport& report) override;
    Status clear_weather() override;
    Status push_weather(const model::WeatherReport& report) override;
    Status forget_phones() override;
    [[nodiscard]] std::optional<FixedString<32>> wifi_ssid() const override;
    [[nodiscard]] bool wifi_has_password() const override;
    Status set_wifi(std::string_view ssid, std::string_view password) override;
    Status clear_wifi() override;
    Status sync_now(bool time, bool weather) override;
    [[nodiscard]] console::SyncInfo sync_info() const override;
    Status start_provisioning(FixedString<32>& ssid_out, std::uint16_t& expires_s_out) override;
    Status stop_provisioning() override;
    Status refresh_display(bool full) override;
    [[nodiscard]] std::span<const std::uint8_t> framebuffer() const override;
    [[nodiscard]] std::size_t wake_record_count() const override;
    [[nodiscard]] model::WakeRecord wake_record(std::size_t index_oldest_first) const override;
    void clear_wake_log() override;
    Status write_diag(std::string_view page, console::JsonWriter& out) override;
    void list_selftests(console::JsonWriter& out) override;
    Status run_selftests(std::string_view filter, console::JsonWriter& out) override;
    Status set_log_level(LogLevel level) override;
    Status vibrate(std::uint16_t ms) override;
    Status request_sleep(std::uint32_t seconds) override;
    Status request_reboot() override;
    Status factory_reset() override;

    // ---- conn::FormSink (actions.cpp) ----
    Status apply(const conn::ProvisioningForm& form) override;

private:
    // ---- flows.cpp ----
    void begin_wake() noexcept;
    void cold_boot() noexcept;
    void detect_cause(const hal::WakeSources& sources) noexcept;
    void attach_accel() noexcept;
    void sync_tap_interrupt() noexcept;
    void read_steps() noexcept;
    void sample_battery() noexcept;
    void adc_sample() noexcept;
    void take_fake_sample() noexcept;
    void show_wake_frame(bool overlay) noexcept;
    time::UnixMicros timer_wake_target() noexcept;
    void show_critical() noexcept;
    void run_connectivity() noexcept;
    Status run_session(const conn::Plan& plan, bool manual) noexcept;
    void refresh_face_if_stale() noexcept;
    void refresh_after_radio() noexcept;
    Status render_and_present(bool force_full,
                              time::UnixMicros target_utc_us,
                              bool force_update = false) noexcept;
    Status present(bool force_full, time::UnixMicros target_utc_us, bool force_update) noexcept;
    Status display_failed(Error error) noexcept;
    void sleep_for(std::int64_t us, bool wake_on_buttons) noexcept;
    void wait_for_release() noexcept;
    void record_wake() noexcept;
    hal::SleepPlan finish_wake(std::int64_t sleep_override_us) noexcept;
    [[nodiscard]] PlanRequest make_plan_request(std::int64_t sleep_override_us) const noexcept;
    void commit_all() noexcept;
    void note_error(Error error) noexcept;
    [[nodiscard]] bool tap_desired() const noexcept;
    [[nodiscard]] bool has_creds_flag() const noexcept;
    void set_creds_flag(bool present) noexcept;
    void flush_steps() noexcept;
    void persist_drift() noexcept;
    void after_step_update(const steps::StepUpdate& update) noexcept;

    // ---- snapshot.cpp ----
    const ui::WatchState& build_state(time::UnixSeconds utc_s) noexcept;
    [[nodiscard]] conn::Inputs make_inputs(bool manual) const noexcept;
    void refresh_tz() noexcept;
    [[nodiscard]] time::UnixSeconds now_utc_s() const noexcept;
    [[nodiscard]] time::UnixSeconds display_utc_s() const noexcept;
    [[nodiscard]] std::optional<time::DayNumber> local_day(time::UnixSeconds utc_s) const noexcept;
    [[nodiscard]] bool radio_compiled() const noexcept;
    [[nodiscard]] bool phone_compiled() const noexcept;
    [[nodiscard]] model::WeatherFreshness weather_freshness(time::UnixSeconds now_s) const noexcept;

    // ---- actions.cpp ----
    void execute(const ui::Action& action) noexcept;
    Status apply_setting_impl(settings::Key key, std::string_view value) noexcept;
    Status set_time_local(const time::CivilDate& date, const time::CivilTime& time) noexcept;
    void set_time_impl(time::UnixSeconds utc, time::TimeSource source) noexcept;
    Status factory_reset_impl() noexcept;
    Status run_sync(bool want_time, bool want_weather) noexcept;
    Status sync_with_ui() noexcept;
    Status start_provisioning_impl(FixedString<32>& ssid_out, std::uint16_t& expires_s) noexcept;
    void stop_provisioning_impl() noexcept;
    void poll_provisioning() noexcept;
    Status start_phone_impl() noexcept;
    void stop_phone_impl(conn::PhoneEnd why) noexcept;
    /// Serves the phone link for up to `wait_us` (capped to the button poll slice); ends the
    /// session when conn::PhoneSession says so.
    void poll_phone(std::int64_t wait_us) noexcept;
    Status vibrate_impl(std::uint16_t ms) noexcept;
    Status run_selftests_impl(std::string_view filter, console::JsonWriter* out) noexcept;
    Status apply_settings_object(const settings::Settings& next) noexcept;

    // ---- interactive.cpp ----
    void interactive_session(std::uint8_t seed_mask) noexcept;
    bool handle_event(const model::InputEvent& event) noexcept;
    void sample_buttons() noexcept;
    std::int64_t tethered_loop() noexcept;
    void tether_tick(const WakePlan& plan) noexcept;
    void handle_console_line(std::size_t length) noexcept;
    void send_event(std::string_view json) noexcept;
    void send_ready_event() noexcept;
    void return_to_face() noexcept;
    [[nodiscard]] bool screen_is_passive() const noexcept;

    Platform& p_;
    BuildFeatures f_;
    TetherPolicy& tether_;

    RtcState rtc_{};
    RtcStore store_;
    gfx::Framebuffer shown_{}; ///< what the panel shows (mirrors the RTC FrameShadow)
    gfx::Framebuffer next_{};  ///< render target
    time::TimeKeeper keeper_;
    steps::StepTracker steps_;
    steps::StepHistoryStore step_store_;
    power::PowerPolicy power_;
    conn::Scheduler sched_;
    weather::OpenMeteoProvider weather_provider_;
    std::optional<conn::SyncSession> session_;
    settings::SettingsStore settings_store_;
    mutable settings::CredentialStore cred_store_; ///< load() reads NVS: logically const
    WakePlanner planner_;
    ssd1681::Panel panel_;
    bma423::Accelerometer accel_;
    faces::Registry faces_;
    ui::Ui ui_;
    ui::GestureRecognizer recognizer_;
    conn::Provisioning prov_;
    conn::PhoneSession phone_;
    FixedString<11> phone_name_;
    console::Registry registry_;
    console::Dispatcher dispatcher_;
    time::TimeZone tz_;

    WakeCtx w_{};
    ui::WatchState state_{};
    std::array<model::WakeRecord, wiring::kRecentWakes> recent_{};
    FixedString<24> selftest_summary_;
    ui::OpPhase op_phase_ = ui::OpPhase::kIdle;
    Errc op_error_ = Errc::kInternal;
    time::UnixSeconds shown_minute_ = -1; ///< UTC minute of the last rendered frame, -1 = none
    std::int64_t last_input_us_ = 0;
    std::int64_t prov_started_rtc_us_ = 0;
    std::uint32_t pending_sleep_s_ = 0;
    bool pending_reboot_ = false;
    bool loaded_ = false; ///< rtc_ holds the committed state (a wake ran)
    bool frame_dirty_ = false;
    bool force_full_next_ = false;
    bool accel_ok_ = false;
    bool prov_active_ = false;
    bool phone_active_ = false;
    hal::PhoneLinkState phone_seen_ = hal::PhoneLinkState::kOff; ///< last state rendered
    std::uint32_t phone_passkey_seen_ = 0;
    std::int64_t phone_ended_us_ = 0; ///< when the last session ended (or failed to start)

    std::array<char, console::kMaxRequestBytes + 1> request_{};
    std::array<char, console::kMaxResponseBytes> response_{};
};

} // namespace qz::app
