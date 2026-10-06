// Connectivity policy and sync sessions (ARCHITECTURE.md section 12), plus provisioning form
// handling (section 13). Pure: radio access only through hal::NetStack / hal::ProvisioningPortal.
#pragma once

#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"
#include "qz/hal/net.hpp"
#include "qz/hal/system.hpp"
#include "qz/model/types.hpp"
#include "qz/weather/provider.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::conn {

/// Persistent part, embedded in app::RtcState. Trivially copyable.
struct ConnState {
    time::UnixSeconds next_time_sync = 0; ///< 0 = due now
    time::UnixSeconds next_weather = 0;
    time::UnixSeconds last_ok_utc = 0; ///< last fully successful session
    time::UnixSeconds last_attempt_utc = 0;
    std::uint32_t sessions = 0;
    std::uint8_t fail_streak = 0;
    std::uint8_t last_error = 0; ///< static_cast<uint8_t>(Errc) + 1, 0 = none
    std::uint8_t ever_synced = 0;
    std::uint8_t reserved = 0;
};

struct Inputs {
    model::ConnectivityMode mode = model::ConnectivityMode::kOff;
    bool radio_compiled = true;
    bool has_credentials = false;
    bool location_set = false;
    model::PowerLevel power = model::PowerLevel::kNormal;
    bool time_valid = false;
    time::UnixSeconds now_utc = 0; ///< meaningful only if time_valid
    std::uint16_t sync_interval_h = 24;
    std::uint16_t weather_interval_min = 60;
    bool manual_request = false; ///< "Sync now" (ignores backoff, not preconditions)
};

struct Plan {
    bool time = false;
    bool weather = false;
    [[nodiscard]] bool any() const noexcept { return time || weather; }
};

struct Budget {
    std::uint32_t connect_ms = 10'000;
    std::uint32_t sntp_ms = 8'000;
    std::uint32_t weather_ms = 12'000;
    std::uint32_t total_ms = 30'000;
};

struct SessionResult {
    Status connect;
    Status time;
    Status weather;
    std::optional<std::int64_t> sntp_utc_us;
    std::int64_t sntp_rtc_us = 0;
    std::optional<model::WeatherReport> report;
    std::uint32_t duration_ms = 0;
};

/// Decides when to use the radio. Never plans anything in Off mode or without preconditions.
class Scheduler {
public:
    Scheduler(ConnState& state, std::uint32_t jitter_seed) noexcept;
    [[nodiscard]] Plan plan(const Inputs& in) const noexcept;
    /// Updates next-due times and backoff (base 15 min, x2, cap min(interval, 12 h), +-10 %).
    void on_result(const Plan& plan, const SessionResult& result, const Inputs& in) noexcept;
    [[nodiscard]] model::SyncIndicator
    indicator(const Inputs& in, time::UnixSeconds last_time_sync_utc) const noexcept;
    /// Mode or credentials changed: make jobs due now (manual reconfiguration).
    void on_config_changed() noexcept;

private:
    ConnState& state_;
    std::uint32_t seed_;
};

/// Runs one radio session: connect -> SNTP -> weather -> shutdown (always). Holds a static
/// body buffer (kMaxBodyBytes). Not thread-safe.
class SyncSession {
public:
    SyncSession(hal::NetStack& net, const weather::Provider& provider, hal::Clock& clock) noexcept;
    SessionResult run(const Plan& plan,
                      const hal::WifiCredentials& creds,
                      const model::Location& loc,
                      const Budget& budget) noexcept;

private:
    hal::NetStack& net_;
    const weather::Provider& provider_;
    hal::Clock& clock_;
};

/// Fields submitted by the provisioning page (raw strings, validated by the app through
/// settings::set_from_string and CredentialStore).
struct ProvisioningForm {
    FixedString<32> ssid;
    Secret<64> password;
    FixedString<40> tz_name;
    FixedString<16> lat;
    FixedString<16> lon;
    FixedString<16> units;
    FixedString<16> mode;
};

/// Implemented by the app: validates + applies a form. Error = message shown on the page.
class FormSink {
public:
    virtual ~FormSink() = default;
    virtual Status apply(const ProvisioningForm& form) = 0;
};

/// Provisioning session: random AP credentials, page HTML, urlencoded form parsing,
/// one-time token, 5 min expiry. Implements the pure side of hal::ProvisioningPortal.
class Provisioning final : public hal::PortalHandler {
public:
    Provisioning(hal::System& system, FormSink& sink) noexcept;
    /// Generates SSID "Quartz-XXXX" (chip id) and a 12-char password from the hardware RNG.
    void begin(std::int64_t now_rtc_us) noexcept;
    [[nodiscard]] std::string_view ssid() const noexcept;
    [[nodiscard]] const Secret<64>& password() const noexcept; ///< shown on the watch only
    [[nodiscard]] bool expired(std::int64_t now_rtc_us) const noexcept;
    [[nodiscard]] bool completed() const noexcept;
    std::string_view page() override;
    Result<std::string_view> submit(std::string_view form_body) override;
    /// Decodes application/x-www-form-urlencoded into the form (exposed for tests).
    static Result<ProvisioningForm> parse_form(std::string_view body,
                                               std::string_view expected_token) noexcept;

private:
    hal::System& system_;
    FormSink& sink_;
};

} // namespace qz::conn
