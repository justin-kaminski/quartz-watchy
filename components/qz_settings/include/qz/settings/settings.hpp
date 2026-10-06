// Settings schema, validation and persistence (ARCHITECTURE.md section 7).
// One validation path for UI editors, console `settings set`, and the provisioning form.
#pragma once

#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/net.hpp"
#include "qz/model/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::settings {

/// Every user setting. Console names and NVS keys come from schema(); order is not persisted.
enum class Key : std::uint8_t {
    kHourFormat,         ///< "tfmt"   enum 24h|12h
    kTimeZone,           ///< "tz"     IANA name from the built-in list
    kTempUnit,           ///< "units"  enum c|f
    kConnectivity,       ///< "conn"   enum off|time|time+weather
    kWeatherHighLow,     ///< "wx_hilo" bool
    kLatitude,           ///< "lat"    decimal degrees, stored as 1e-5 deg
    kLongitude,          ///< "lon"
    kSyncIntervalH,      ///< "sync_h" one of 6,12,24,48,168
    kWeatherIntervalMin, ///< "wx_min" one of 30,60,120,180,360
    kStepGoal,           ///< "goal"   0 (off) .. 50000, multiple of 500
    kVibration,          ///< "vib"    bool
    kFace,               ///< "face"   registered face id
    kTapWake,            ///< "tapwake" bool
    kCount
};

struct Settings {
    model::HourFormat hour_format = model::HourFormat::k24h;
    FixedString<40> tz_name{"UTC"};
    FixedString<64> tz_posix{
        "UTC0"}; ///< resolved from the built-in list; fallback if a name disappears
    model::TempUnit temp_unit = model::TempUnit::kCelsius;
    model::ConnectivityMode connectivity = model::ConnectivityMode::kOff;
    bool weather_high_low = true;
    bool location_set = false;
    model::Location location{};
    std::uint16_t sync_interval_h = 24;
    std::uint16_t weather_interval_min = 60;
    std::uint32_t step_goal = 0;
    bool vibration = true;
    std::uint8_t face_id = 0;
    bool tap_wake = false;

    bool operator==(const Settings&) const noexcept = default;
};

enum class ValueType : std::uint8_t { kBool, kEnum, kUInt, kDegrees, kZoneName };

struct KeyInfo {
    Key key;
    std::string_view name; ///< console name == NVS key (<= 15 chars)
    ValueType type;
    std::int64_t min; ///< kUInt / kDegrees (1e-5 deg)
    std::int64_t max;
    std::span<const std::string_view>
        choices; ///< kEnum tokens, or allowed kUInt values ("6","12",...)
    std::string_view help;
};

[[nodiscard]] std::span<const KeyInfo> schema() noexcept;
[[nodiscard]] const KeyInfo* find_key(std::string_view name) noexcept;
[[nodiscard]] const KeyInfo& info(Key key) noexcept;
[[nodiscard]] Settings defaults() noexcept;
/// Full-object validation (ranges, zone exists, face id registered via `face_ok` callback).
Status validate(const Settings& s, bool (*face_ok)(std::uint8_t id)) noexcept;
/// Parses and validates one value (console/provisioning/UI). On error `s` is unchanged.
Status set_from_string(Settings& s, Key key, std::string_view value) noexcept;
/// Canonical string form (inverse of set_from_string). Returns chars written.
std::size_t format_value(const Settings& s, Key key, std::span<char> out) noexcept;

inline constexpr std::uint16_t kSchemaVersion = 1;
inline constexpr std::string_view kNamespace = "qz_set";
inline constexpr std::string_view kCredNamespace = "qz_cred";

/// NVS persistence. Writes only keys that differ from `previous`; commit once.
class SettingsStore {
public:
    explicit SettingsStore(hal::KvStore& kv) noexcept;
    /// Loads, migrates older schema versions, validates; invalid keys fall back to defaults.
    Result<Settings> load() noexcept;
    Status save(const Settings& current, const Settings& previous) noexcept;
    /// Factory reset: erases every qz_* namespace.
    Status erase_all() noexcept;

private:
    hal::KvStore& kv_;
};

/// Wi-Fi credentials in NVS namespace qz_cred. Never logs values.
class CredentialStore {
public:
    explicit CredentialStore(hal::KvStore& kv) noexcept;
    Result<hal::WifiCredentials> load() noexcept; ///< kNoCredentials when absent
    Status save(const hal::WifiCredentials& creds) noexcept;
    Status clear() noexcept;

private:
    hal::KvStore& kv_;
};

} // namespace qz::settings
