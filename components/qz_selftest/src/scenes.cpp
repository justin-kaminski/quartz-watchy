// Canonical UI scenes (ARCHITECTURE.md section 18): fixed WatchState fixtures covering every
// screen and every face variant. Scenes pin ALL inputs (date, steps, battery, weather, fixed
// version strings "1.2.3" / "abc1234" - never the real git hash). Integer-only.
#include "qz/core/assert.hpp"
#include "qz/selftest/selftest.hpp"
#include "qz/time/civil.hpp"

#include <array>

namespace qz::selftest {

namespace {

using model::Button;
using model::InputEvent;
using model::InputKind;
using model::PowerLevel;
using model::SyncIndicator;
using model::WeatherCondition;
using model::WeatherFreshness;
using settings::Key;
using settings::Settings;
using ui::ScreenId;
using ui::WatchState;

constexpr time::CivilDate kDate{2026, 10, 6}; ///< Tuesday
constexpr std::int32_t kCdtOffsetS = -5 * 3600;
constexpr std::int64_t kFixtureLocalS = (14 * 3600) + (32 * 60); ///< 14:32:00 local
constexpr std::uint32_t kStepsToday = 6240;
constexpr std::uint32_t kStepGoal = 10000;
constexpr std::int64_t kHourS = 3600;

constexpr std::array<std::uint32_t, model::kStepHistoryDays> kHistorySteps{
    8421, 10250, 5310, 12044, 7788, 9120, 3402};

constexpr std::array<model::WakeRecord, 4> kWakes{{
    {1790000000U,
     312,
     model::WakeCause::kTimer,
     model::kWakeFlagPartialRefresh,
     3950,
     PowerLevel::kNormal,
     0,
     14,
     0},
    {1789999940U,
     1840,
     model::WakeCause::kButton,
     model::kWakeFlagPartialRefresh | model::kWakeFlagInput,
     3948,
     PowerLevel::kNormal,
     0,
     0,
     0},
    {1789996400U,
     9120,
     model::WakeCause::kTimer,
     model::kWakeFlagFullRefresh | model::kWakeFlagRadio,
     3940,
     PowerLevel::kNormal,
     0,
     35,
     0},
    {1789990000U,
     455,
     model::WakeCause::kAccel,
     model::kWakeFlagPartialRefresh | model::kWakeFlagError,
     3921,
     PowerLevel::kLow,
     static_cast<std::uint8_t>(static_cast<unsigned>(Errc::kTimeout) + 1U),
     0,
     0},
}};

void set(Settings& b, Key key, std::string_view value) noexcept {
    // A fixture the validator rejects is a programmer error, not a runtime condition.
    QZ_ASSERT(settings::set_from_string(b, key, value).has_value());
}

time::UnixSeconds fixture_utc() noexcept {
    return (time::days_from_civil(kDate) * time::kSecondsPerDay) + kFixtureLocalS - kCdtOffsetS;
}

/// The reference state every scene starts from: Tuesday 2026-10-06 14:32 CDT, 24 h, 6240 of
/// 10000 steps, 80 % battery, fresh partly-cloudy weather, last sync OK, firmware 1.2.3 (abc1234).
void base(WatchState& s, Settings& b) noexcept {
    b = settings::defaults();
    set(b, Key::kTimeZone, "America/Chicago");
    set(b, Key::kStepGoal, "10000");
    set(b, Key::kConnectivity, "time+weather");
    s = WatchState{};
    s.settings = &b;

    s.time_valid = true;
    s.local.date = kDate;
    s.local.time = {14, 32, 0};
    s.local.weekday = time::Weekday::kTuesday;
    s.local.utc_offset_s = kCdtOffsetS;
    s.local.is_dst = true;
    s.hour_format = model::HourFormat::k24h;
    s.tz_label = "Chicago (UTC-6)";

    s.steps.today = kStepsToday;
    s.steps.goal = kStepGoal;
    const time::DayNumber today = time::days_from_civil(kDate);
    for (std::size_t i = 0; i < kHistorySteps.size(); ++i) {
        s.steps.history[i] = {today - 1 - static_cast<time::DayNumber>(i), kHistorySteps[i]};
    }
    s.steps.history_count = static_cast<std::uint8_t>(kHistorySteps.size());

    s.battery.valid = true;
    s.battery.mv = 3950;
    s.battery.percent = 80;
    s.battery.level = PowerLevel::kNormal;

    s.weather.valid = 1;
    s.weather.has_high_low = 1;
    s.weather.temp_dc = 185;
    s.weather.high_dc = 210;
    s.weather.low_dc = 90;
    s.weather.condition = WeatherCondition::kPartlyCloudy;
    s.weather_age_s = 1800;
    s.weather.fetched_utc = fixture_utc() - static_cast<time::UnixSeconds>(s.weather_age_s);
    s.weather_freshness = WeatherFreshness::kFresh;
    s.temp_unit = model::TempUnit::kCelsius;
    s.weather_high_low = true;

    s.sync = SyncIndicator::kOk;
    s.conn_mode = model::ConnectivityMode::kTimeWeather;
    s.radio_available = true;
    s.has_credentials = true;
    s.last_sync_utc = fixture_utc() - kHourS;
    s.op_phase = ui::OpPhase::kIdle;

    s.power = PowerLevel::kNormal;
    s.tethered = false;
    s.fw_version = "1.2.3";
    s.git_hash = "abc1234";
    s.idf_version = "6.1.0";
    s.drift_ppb = 12'500;
    s.awake_ms_today = 41'250;
    s.wakes_today = 37;
    s.recent_wakes = kWakes;
}

// ---- state variants (each starts from base) ----

void v_24h(WatchState& s, Settings& b) noexcept {
    base(s, b);
}
void v_12h_pm(WatchState& s, Settings& b) noexcept {
    base(s, b);
    set(b, Key::kHourFormat, "12h");
    s.hour_format = model::HourFormat::k12h;
}
void v_12h_am(WatchState& s, Settings& b) noexcept {
    v_12h_pm(s, b);
    s.local.time = {9, 5, 0};
}
void v_24h_location(WatchState& s, Settings& b) noexcept {
    base(s, b); // sunrise/sunset on the progress face
    set(b, Key::kLatitude, "41.77");
    set(b, Key::kLongitude, "-88.15");
}
void v_time_invalid(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.time_valid = false;
    s.local = {};
    s.sync = SyncIndicator::kNeverSynced;
    s.last_sync_utc = 0;
    s.weather = {};
    s.weather_freshness = WeatherFreshness::kHidden;
}
void v_no_goal(WatchState& s, Settings& b) noexcept {
    base(s, b);
    set(b, Key::kStepGoal, "0");
    s.steps.goal = 0;
}
void v_goal_met(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.steps.today = 10450;
}
void v_wx_stale(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.weather_age_s = 5 * 3600;
    s.weather.fetched_utc = fixture_utc() - static_cast<time::UnixSeconds>(s.weather_age_s);
    s.weather_freshness = WeatherFreshness::kStale;
    s.sync = SyncIndicator::kStale;
}
void v_wx_hidden(WatchState& s, Settings& b) noexcept {
    base(s, b);
    set(b, Key::kConnectivity, "time");
    s.conn_mode = model::ConnectivityMode::kTimeOnly;
    s.weather = {};
    s.weather_freshness = WeatherFreshness::kHidden;
}
void v_wx_rain(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.weather.condition = WeatherCondition::kRain;
    s.weather.temp_dc = 124;
}
void v_wx_snow(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.weather.condition = WeatherCondition::kSnow;
    s.weather.temp_dc = -32;
    s.weather.high_dc = 5;
    s.weather.low_dc = -80;
}
void v_wx_thunder(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.weather.condition = WeatherCondition::kThunder;
    s.weather.temp_dc = 261;
}
void v_wx_cold_f(WatchState& s, Settings& b) noexcept {
    base(s, b);
    set(b, Key::kTempUnit, "f");
    s.temp_unit = model::TempUnit::kFahrenheit;
    s.weather.temp_dc = -55;
    s.weather.high_dc = -20;
    s.weather.low_dc = -140;
    s.weather.condition = WeatherCondition::kClear;
}
void v_sync_none(WatchState& s, Settings& b) noexcept {
    base(s, b);
    set(b, Key::kConnectivity, "off");
    s.conn_mode = model::ConnectivityMode::kOff;
    s.sync = SyncIndicator::kNone;
    s.weather = {};
    s.weather_freshness = WeatherFreshness::kHidden;
}
void v_sync_never(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.sync = SyncIndicator::kNeverSynced;
    s.last_sync_utc = 0;
}
void v_sync_failed(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.sync = SyncIndicator::kLastFailed;
}
void v_sync_stale(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.sync = SyncIndicator::kStale;
    s.last_sync_utc = fixture_utc() - (std::int64_t{3} * 24 * kHourS);
}
void v_power_low(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.battery.mv = 3620;
    s.battery.percent = 25;
    s.battery.level = PowerLevel::kLow;
    s.power = PowerLevel::kLow;
}
void v_power_saver(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.battery.mv = 3480;
    s.battery.percent = 10;
    s.battery.level = PowerLevel::kSaver;
    s.power = PowerLevel::kSaver;
}
void v_power_critical(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.battery.mv = 3380;
    s.battery.percent = 5;
    s.battery.level = PowerLevel::kCritical;
    s.power = PowerLevel::kCritical;
}
void v_charging(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.battery.mv = 4150;
    s.battery.percent = 90;
    s.battery.usb_present = true;
    s.battery.charging = true;
    s.tethered = true;
}

/// Runs a variant, then selects the face.
template<void (*Variant)(WatchState&, Settings&) noexcept, std::uint8_t FaceId>
void on_face(WatchState& s, Settings& b) noexcept {
    Variant(s, b);
    b.face_id = FaceId;
}

// ---- screen states ----

void s_base(WatchState& s, Settings& b) noexcept {
    base(s, b);
}
void s_weather_stale(WatchState& s, Settings& b) noexcept {
    v_wx_stale(s, b);
}
void s_location_set(WatchState& s, Settings& b) noexcept {
    base(s, b);
    set(b, Key::kLatitude, "41.77");
    set(b, Key::kLongitude, "-88.15");
}
void s_sync_running(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.op_phase = ui::OpPhase::kRunning;
}
void s_sync_ok(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.op_phase = ui::OpPhase::kSucceeded;
}
void s_sync_failed(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.op_phase = ui::OpPhase::kFailed;
    s.op_error = Errc::kTimeout;
    s.sync = SyncIndicator::kLastFailed;
}
void s_sync_no_creds(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.has_credentials = false;
    s.sync = SyncIndicator::kNeverSynced;
}
void s_sync_no_radio(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.radio_available = false;
    set(b, Key::kConnectivity, "off");
    s.conn_mode = model::ConnectivityMode::kOff;
}
void s_provisioning(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.prov_ssid = "Quartz-1A2B";
    s.prov_password = "quartz-7421";
    s.prov_seconds_left = 240;
    s.op_phase = ui::OpPhase::kRunning;
    s.tethered = true;
}
void s_provisioning_starting(WatchState& s, Settings& b) noexcept {
    s_provisioning(s, b);
    s.prov_ssid = {};
    s.prov_password = {};
}
void s_provisioning_long(WatchState& s, Settings& b) noexcept {
    s_provisioning(s, b);
    s.prov_password = "quartz-7421-0f3a-9c1d-55e2-b8aa-1d4c-72e9";
    s.prov_seconds_left = 17;
}
void s_phone(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.phone_available = true;
}
void s_phone_off(WatchState& s, Settings& b) noexcept {
    s_phone(s, b);
    set(b, Key::kPhoneSync, "off");
}
void s_phone_waiting(WatchState& s, Settings& b) noexcept {
    s_phone(s, b);
    s.phone_phase = ui::PhonePhase::kAdvertising;
    s.phone_name = "Quartz-1A2B";
    s.op_phase = ui::OpPhase::kRunning;
}
void s_phone_pairing(WatchState& s, Settings& b) noexcept {
    s_phone_waiting(s, b);
    s.phone_phase = ui::PhonePhase::kPairing;
    s.phone_passkey = 47'201; // leading zero is part of the code: "047201"
}
void s_phone_connected(WatchState& s, Settings& b) noexcept {
    s_phone_waiting(s, b);
    s.phone_phase = ui::PhonePhase::kConnected;
}
void s_phone_done(WatchState& s, Settings& b) noexcept {
    s_phone(s, b);
    s.op_phase = ui::OpPhase::kSucceeded;
}
void s_phone_no_phone(WatchState& s, Settings& b) noexcept {
    s_phone(s, b);
    s.op_phase = ui::OpPhase::kFailed;
    s.op_error = Errc::kTimeout;
}
void s_diagnostics(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.tethered = true;
}
void s_diagnostics_degraded(WatchState& s, Settings& b) noexcept {
    base(s, b);
    s.clock_degraded = true;
    s.drift_ppb = 480'000;
    s.selftest_summary = "28/28 pass";
}
void s_charge_me(WatchState& s, Settings& b) noexcept {
    v_power_critical(s, b);
}
void s_status_charging(WatchState& s, Settings& b) noexcept {
    v_charging(s, b);
}

// ---- input scripts ----

constexpr InputEvent kDown{Button::kDown, InputKind::kClick, 50, 0};
constexpr std::array<InputEvent, 1> kDown1{kDown};
constexpr std::array<InputEvent, 2> kDown2{kDown, kDown};
constexpr std::array<InputEvent, 3> kDown3{kDown, kDown, kDown};
constexpr std::array<InputEvent, 5> kDown5{kDown, kDown, kDown, kDown, kDown};

constexpr auto kScenes = std::to_array<Scene>({
    // default face
    {"face_default_24h", ScreenId::kFace, &on_face<v_24h, 0>, {}},
    {"face_default_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 0>, {}},
    {"face_default_12h_am", ScreenId::kFace, &on_face<v_12h_am, 0>, {}},
    {"face_default_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 0>, {}},
    {"face_default_no_goal", ScreenId::kFace, &on_face<v_no_goal, 0>, {}},
    {"face_default_goal_met", ScreenId::kFace, &on_face<v_goal_met, 0>, {}},
    {"face_default_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 0>, {}},
    {"face_default_wx_hidden", ScreenId::kFace, &on_face<v_wx_hidden, 0>, {}},
    {"face_default_wx_rain", ScreenId::kFace, &on_face<v_wx_rain, 0>, {}},
    {"face_default_wx_snow", ScreenId::kFace, &on_face<v_wx_snow, 0>, {}},
    {"face_default_wx_thunder", ScreenId::kFace, &on_face<v_wx_thunder, 0>, {}},
    {"face_default_wx_cold_f", ScreenId::kFace, &on_face<v_wx_cold_f, 0>, {}},
    {"face_default_sync_none", ScreenId::kFace, &on_face<v_sync_none, 0>, {}},
    {"face_default_sync_never", ScreenId::kFace, &on_face<v_sync_never, 0>, {}},
    {"face_default_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 0>, {}},
    {"face_default_sync_stale", ScreenId::kFace, &on_face<v_sync_stale, 0>, {}},
    {"face_default_power_low", ScreenId::kFace, &on_face<v_power_low, 0>, {}},
    {"face_default_power_saver", ScreenId::kFace, &on_face<v_power_saver, 0>, {}},
    {"face_default_power_critical", ScreenId::kFace, &on_face<v_power_critical, 0>, {}},
    {"face_default_charging", ScreenId::kFace, &on_face<v_charging, 0>, {}},
    // minimal face
    {"face_minimal_24h", ScreenId::kFace, &on_face<v_24h, 1>, {}},
    {"face_minimal_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 1>, {}},
    {"face_minimal_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 1>, {}},
    {"face_minimal_goal_met", ScreenId::kFace, &on_face<v_goal_met, 1>, {}},
    {"face_minimal_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 1>, {}},
    {"face_minimal_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 1>, {}},
    {"face_minimal_power_saver", ScreenId::kFace, &on_face<v_power_saver, 1>, {}},
    {"face_minimal_charging", ScreenId::kFace, &on_face<v_charging, 1>, {}},
    // analog face
    {"face_analog_24h", ScreenId::kFace, &on_face<v_24h, 2>, {}},
    {"face_analog_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 2>, {}},
    {"face_analog_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 2>, {}},
    {"face_analog_goal_met", ScreenId::kFace, &on_face<v_goal_met, 2>, {}},
    {"face_analog_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 2>, {}},
    {"face_analog_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 2>, {}},
    {"face_analog_power_saver", ScreenId::kFace, &on_face<v_power_saver, 2>, {}},
    {"face_analog_charging", ScreenId::kFace, &on_face<v_charging, 2>, {}},
    // stacked face
    {"face_stacked_24h", ScreenId::kFace, &on_face<v_24h, 3>, {}},
    {"face_stacked_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 3>, {}},
    {"face_stacked_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 3>, {}},
    {"face_stacked_goal_met", ScreenId::kFace, &on_face<v_goal_met, 3>, {}},
    {"face_stacked_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 3>, {}},
    {"face_stacked_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 3>, {}},
    {"face_stacked_power_saver", ScreenId::kFace, &on_face<v_power_saver, 3>, {}},
    {"face_stacked_charging", ScreenId::kFace, &on_face<v_charging, 3>, {}},
    // words face
    {"face_words_24h", ScreenId::kFace, &on_face<v_24h, 4>, {}},
    {"face_words_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 4>, {}},
    {"face_words_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 4>, {}},
    {"face_words_goal_met", ScreenId::kFace, &on_face<v_goal_met, 4>, {}},
    {"face_words_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 4>, {}},
    {"face_words_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 4>, {}},
    {"face_words_power_saver", ScreenId::kFace, &on_face<v_power_saver, 4>, {}},
    {"face_words_charging", ScreenId::kFace, &on_face<v_charging, 4>, {}},
    // dashboard face
    {"face_dashboard_24h", ScreenId::kFace, &on_face<v_24h, 5>, {}},
    {"face_dashboard_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 5>, {}},
    {"face_dashboard_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 5>, {}},
    {"face_dashboard_goal_met", ScreenId::kFace, &on_face<v_goal_met, 5>, {}},
    {"face_dashboard_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 5>, {}},
    {"face_dashboard_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 5>, {}},
    {"face_dashboard_power_saver", ScreenId::kFace, &on_face<v_power_saver, 5>, {}},
    {"face_dashboard_charging", ScreenId::kFace, &on_face<v_charging, 5>, {}},
    // progress face
    {"face_progress_24h", ScreenId::kFace, &on_face<v_24h, 6>, {}},
    {"face_progress_12h_pm", ScreenId::kFace, &on_face<v_12h_pm, 6>, {}},
    {"face_progress_time_invalid", ScreenId::kFace, &on_face<v_time_invalid, 6>, {}},
    {"face_progress_goal_met", ScreenId::kFace, &on_face<v_goal_met, 6>, {}},
    {"face_progress_wx_stale", ScreenId::kFace, &on_face<v_wx_stale, 6>, {}},
    {"face_progress_sync_failed", ScreenId::kFace, &on_face<v_sync_failed, 6>, {}},
    {"face_progress_power_saver", ScreenId::kFace, &on_face<v_power_saver, 6>, {}},
    {"face_progress_charging", ScreenId::kFace, &on_face<v_charging, 6>, {}},
    {"face_progress_sun", ScreenId::kFace, &on_face<v_24h_location, 6>, {}},
    // system screens
    {"screen_steps_history", ScreenId::kStepsHistory, &s_base, {}},
    {"screen_weather_detail", ScreenId::kWeatherDetail, &s_base, {}},
    {"screen_weather_detail_stale", ScreenId::kWeatherDetail, &s_weather_stale, {}},
    {"screen_menu", ScreenId::kMenu, &s_base, {}},
    {"screen_menu_down3", ScreenId::kMenu, &s_base, kDown3},
    {"screen_time_date_editor", ScreenId::kTimeDateEditor, &s_base, {}},
    {"screen_timezone_picker", ScreenId::kTimezonePicker, &s_base, {}},
    {"screen_timezone_picker_down5", ScreenId::kTimezonePicker, &s_base, kDown5},
    {"screen_choice_hour_format", ScreenId::kChoice, &s_base, {}},
    {"screen_weather_settings", ScreenId::kWeatherSettings, &s_base, {}},
    {"screen_location_editor", ScreenId::kLocationEditor, &s_location_set, {}},
    {"screen_location_editor_unset", ScreenId::kLocationEditor, &s_base, {}},
    {"screen_step_goal_editor", ScreenId::kStepGoalEditor, &s_base, {}},
    {"screen_sync_now_idle", ScreenId::kSyncNow, &s_base, {}},
    {"screen_sync_now_running", ScreenId::kSyncNow, &s_sync_running, {}},
    {"screen_sync_now_ok", ScreenId::kSyncNow, &s_sync_ok, {}},
    {"screen_sync_now_failed", ScreenId::kSyncNow, &s_sync_failed, {}},
    {"screen_sync_now_no_creds", ScreenId::kSyncNow, &s_sync_no_creds, {}},
    {"screen_sync_now_no_radio", ScreenId::kSyncNow, &s_sync_no_radio, {}},
    {"screen_provisioning", ScreenId::kProvisioning, &s_provisioning, {}},
    {"screen_provisioning_starting", ScreenId::kProvisioning, &s_provisioning_starting, {}},
    {"screen_provisioning_long_password", ScreenId::kProvisioning, &s_provisioning_long, {}},
    {"screen_phone", ScreenId::kPhone, &s_phone, {}},
    {"screen_phone_off", ScreenId::kPhone, &s_phone_off, {}},
    {"screen_phone_sync_waiting", ScreenId::kPhoneSync, &s_phone_waiting, {}},
    {"screen_phone_sync_pairing", ScreenId::kPhoneSync, &s_phone_pairing, {}},
    {"screen_phone_sync_connected", ScreenId::kPhoneSync, &s_phone_connected, {}},
    {"screen_phone_sync_done", ScreenId::kPhoneSync, &s_phone_done, {}},
    {"screen_phone_sync_no_phone", ScreenId::kPhoneSync, &s_phone_no_phone, {}},
    {"screen_menu_with_phone", ScreenId::kMenu, &s_phone, kDown5},
    {"screen_diagnostics", ScreenId::kDiagnostics, &s_diagnostics, {}},
    {"screen_diagnostics_degraded", ScreenId::kDiagnostics, &s_diagnostics_degraded, {}},
    {"screen_diagnostics_page2", ScreenId::kDiagnostics, &s_diagnostics, kDown1},
    {"screen_diagnostics_page3", ScreenId::kDiagnostics, &s_diagnostics, kDown2},
    {"screen_diagnostics_page4", ScreenId::kDiagnostics, &s_diagnostics, kDown3},
    {"screen_diagnostics_page6", ScreenId::kDiagnostics, &s_diagnostics_degraded, kDown5},
    {"screen_about", ScreenId::kAbout, &s_base, {}},
    {"screen_factory_reset", ScreenId::kFactoryReset, &s_base, {}},
    {"screen_charge_me", ScreenId::kChargeMe, &s_charge_me, {}},
    {"screen_status_overlay", ScreenId::kStatusOverlay, &s_base, {}},
    {"screen_status_overlay_charging", ScreenId::kStatusOverlay, &s_status_charging, {}},
});

} // namespace

std::span<const Scene> scenes() noexcept {
    return kScenes;
}

Status
render_scene(const Scene& scene, const ui::FaceSource& faces, gfx::Framebuffer& out) noexcept {
    if (scene.build_state == nullptr) {
        return Errc::kBadArgs;
    }
    Settings backing = settings::defaults();
    WatchState state;
    state.settings = &backing;
    scene.build_state(state, backing);
    state.settings = &backing;

    ui::Ui ui(faces);
    QZ_RETURN_IF_ERROR(ui.show(scene.screen, state));
    for (const InputEvent& event : scene.inputs) {
        (void)ui.handle(event, state); // actions of a scripted input are not executed
    }
    gfx::Canvas canvas(out);
    ui.render(state, canvas);
    return ok();
}

} // namespace qz::selftest
