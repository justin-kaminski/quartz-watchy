// UI model (ARCHITECTURE.md section 15): pure screens rendering a WatchState snapshot and
// emitting Actions. The UI never calls services.
#pragma once

#include "qz/core/containers.hpp"
#include "qz/core/fixed_string.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/model/types.hpp"
#include "qz/settings/settings.hpp"
#include "qz/time/civil.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::ui {

/// Stable ids (console `screen show <name>`, scenes, goldens). Append only.
enum class ScreenId : std::uint8_t {
    kFace = 0,
    kStepsHistory,
    kWeatherDetail,
    kMenu,
    kTimeDateEditor,
    kTimezonePicker,
    kChoice,
    kWeatherSettings,
    kLocationEditor,
    kStepGoalEditor,
    kSyncNow,
    kProvisioning,
    kDiagnostics,
    kAbout,
    kFactoryReset,
    kChargeMe,
    kStatusOverlay,
    kCount
};
[[nodiscard]] std::string_view screen_name(ScreenId id) noexcept; ///< "face", "menu", ...
[[nodiscard]] Result<ScreenId> screen_from_name(std::string_view name) noexcept;

/// Progress of a radio/provisioning operation shown by SyncNow / Provisioning screens.
enum class OpPhase : std::uint8_t { kIdle = 0, kRunning, kSucceeded, kFailed };

/// Everything any screen or face may show. Built by the app once per render; views point into
/// app-owned storage valid until the next build. Scenes build it from fixed fixtures.
struct WatchState {
    // time
    bool time_valid = false;
    time::LocalDateTime local{};
    model::HourFormat hour_format = model::HourFormat::k24h;
    std::string_view tz_label;
    // data
    model::StepsSummary steps{};
    model::BatteryStatus battery{};
    model::WeatherReport weather{};
    model::WeatherFreshness weather_freshness = model::WeatherFreshness::kHidden;
    model::TempUnit temp_unit = model::TempUnit::kCelsius;
    bool weather_high_low = true;
    std::uint32_t weather_age_s = 0;
    // connectivity
    model::SyncIndicator sync = model::SyncIndicator::kNone;
    model::ConnectivityMode conn_mode = model::ConnectivityMode::kOff;
    bool radio_available = true; ///< BuildFeatures.radio
    bool has_credentials = false;
    time::UnixSeconds last_sync_utc = 0;
    OpPhase op_phase = OpPhase::kIdle;
    Errc op_error = Errc::kInternal; ///< valid when op_phase == kFailed
    // provisioning (only while active)
    std::string_view prov_ssid;
    std::string_view prov_password; ///< displayed on the watch only, never logged
    std::uint16_t prov_seconds_left = 0;
    // power / system
    model::PowerLevel power = model::PowerLevel::kNormal;
    bool tethered = false;
    std::string_view fw_version; ///< scenes use "1.2.3", never the real version
    std::string_view git_hash;   ///< scenes use "abc1234"
    std::string_view idf_version;
    std::int32_t drift_ppb = 0;
    bool clock_degraded = false;
    std::uint32_t awake_ms_today = 0;
    std::uint16_t wakes_today = 0;
    std::span<const model::WakeRecord> recent_wakes;
    std::string_view selftest_summary; ///< "12/12 pass" after a run, empty otherwise
    // settings (editors show current values)
    const settings::Settings* settings = nullptr;
};

enum class ActionKind : std::uint8_t {
    kSetSetting, ///< key + value (canonical string, validated by settings::set_from_string)
    kSetTime,    ///< date + time (local, GapPolicy::kEarlier)
    kSyncNow,
    kStartProvisioning,
    kStopProvisioning,
    kFactoryReset,
    kRunSelfTest,
    kFullRefresh,
    kVibrate, ///< arg = ms
};

struct Action {
    ActionKind kind = ActionKind::kFullRefresh;
    settings::Key key = settings::Key::kCount;
    FixedString<48> value;
    time::CivilDate date{};
    time::CivilTime time{};
    std::uint16_t arg = 0;
};
using ActionList = StaticVector<Action, 4>;

/// Implemented by qz_faces (registry). Lets the UI render the selected face without depending
/// on qz_faces.
class FaceSource {
public:
    virtual ~FaceSource() = default;
    [[nodiscard]] virtual std::size_t count() const = 0;
    [[nodiscard]] virtual std::uint8_t id_at(std::size_t index) const = 0;
    [[nodiscard]] virtual std::string_view name_of(std::uint8_t id) const = 0; ///< "" if unknown
    /// Renders face `id` (unknown id -> face 0).
    virtual void render(std::uint8_t id, const WatchState& state, gfx::Canvas& canvas) const = 0;
};

/// Refresh request the UI attaches to each frame.
enum class RefreshHint : std::uint8_t { kPartial = 0, kFull };

/// Converts debounced pin samples into InputEvents (ARCHITECTURE.md section 15).
struct GestureTiming {
    std::uint32_t debounce_ms = 25;
    std::uint32_t hold_ms = 700;
    std::uint32_t repeat_ms = 150;
};

class GestureRecognizer {
public:
    explicit GestureRecognizer(GestureTiming timing = {}) noexcept;
    /// Wake seeded by EXT1: buttons in `mask` are treated as pressed since `press_rtc_us`.
    void seed(std::uint8_t pressed_mask, std::int64_t press_rtc_us) noexcept;
    /// Feed a sample of hal::BoardIo::pressed_buttons(); appends 0..n events.
    void sample(std::uint8_t pressed_mask,
                std::int64_t now_rtc_us,
                StaticVector<model::InputEvent, 8>& out) noexcept;
    /// When the caller should sample next (for light-sleep timing); -1 = only on a pin change.
    [[nodiscard]] std::int64_t next_deadline_us() const noexcept;
    [[nodiscard]] bool any_pressed() const noexcept;

private:
    GestureTiming timing_;
    std::array<std::int64_t, model::kButtonCount> pressed_since_{};
    std::array<std::int64_t, model::kButtonCount> last_emit_{};
    std::uint8_t state_ = 0;
    std::uint8_t hold_sent_ = 0;
};

/// Screen stack + navigation + all system screens. Fixed storage, no heap. Not thread-safe.
class Ui {
public:
    explicit Ui(const FaceSource& faces) noexcept;
    void reset_to_face() noexcept; ///< after deep sleep
    [[nodiscard]] ScreenId current() const noexcept;
    /// Console/scenes: jump to a screen (editors start from current settings).
    Status show(ScreenId id, const WatchState& state) noexcept;
    /// One input event -> actions for the app. Navigation happens inside.
    ActionList handle(const model::InputEvent& event, const WatchState& state) noexcept;
    /// Draws the current screen into the canvas (clears first).
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept;
    /// Full refresh wanted for the next frame (e.g. leaving menus); cleared by the app.
    [[nodiscard]] RefreshHint refresh_hint() const noexcept;
    void clear_refresh_hint() noexcept;
    /// Menus time out after 30 s without input; the face after 2 s (interactive session end).
    [[nodiscard]] bool idle_expired(std::int64_t now_rtc_us,
                                    std::int64_t last_input_rtc_us) const noexcept;

private:
    const FaceSource& faces_;
    // Screen objects and navigation stack are added by the UI work package (fixed storage).
};

} // namespace qz::ui
