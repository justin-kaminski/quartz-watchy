// Connectivity policy and sync sessions (ARCHITECTURE.md section 12), plus provisioning form
// handling (section 13). Pure: radio access only through hal::NetStack / hal::ProvisioningPortal.
#pragma once

#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"
#include "qz/hal/net.hpp"
#include "qz/hal/system.hpp"
#include "qz/model/types.hpp"
#include "qz/weather/provider.hpp"

#include <array>
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

    /// Backoff before jitter: min(15 min x 2^(streak-1), min(interval, 12 h)) in seconds; 0 for
    /// streak 0. `interval_s` is clamped to at least 15 min.
    [[nodiscard]] static std::int64_t backoff_base_s(std::uint8_t fail_streak,
                                                     std::int64_t interval_s) noexcept;
    /// backoff_base_s x (1 +- 10 %), the jitter being a pure function of (seed, streak).
    [[nodiscard]] static std::int64_t
    backoff_delay_s(std::uint8_t fail_streak, std::int64_t interval_s, std::uint32_t seed) noexcept;
    /// Delay before the next attempt for the current fail streak, relative to "now" (RTC based
    /// pacing for the app while UTC is invalid and next_* cannot be expressed). 0 = no backoff.
    [[nodiscard]] std::int64_t retry_delay_s(const Inputs& in) const noexcept;

private:
    ConnState& state_;
    std::uint32_t seed_;
};

/// Runs one radio session: connect -> SNTP -> weather -> shutdown (always). Holds a static
/// body buffer (kMaxBodyBytes). Not thread-safe.
class SyncSession {
public:
    SyncSession(hal::NetStack& net, const weather::Provider& provider, hal::Clock& clock) noexcept;
    /// `now_utc` (0 = unknown) stamps the weather report when this session does not sync time
    /// itself; if SNTP succeeds in this session its time is used instead. Weather without any
    /// known UTC fails with kNoTime (no network call). Every attempted session ends with
    /// NetStack::shutdown(); an empty plan or empty SSID never touches the radio.
    SessionResult run(const Plan& plan,
                      const hal::WifiCredentials& creds,
                      const model::Location& loc,
                      const Budget& budget,
                      time::UnixSeconds now_utc = 0) noexcept;

private:
    hal::NetStack& net_;
    const weather::Provider& provider_;
    hal::Clock& clock_;
    std::array<char, weather::kMaxBodyBytes> body_{};
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

/// Characters of the generated AP password: digits 2-9 and letters without I, O, l, o (56 symbols;
/// 12 characters ~ 69.7 bits). Excludes every character easily confused with another.
inline constexpr std::string_view kPasswordAlphabet =
    "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz";
inline constexpr std::size_t kPasswordLength = 12;
/// Largest accepted POST body. Form keys: ssid, pass, tz, lat, lon, units, mode, token (the
/// one-time form token). `pass` may be empty (open network); `lat`/`lon` may be empty (location
/// not set); the other fields must be non-empty. Every key must appear exactly once.
inline constexpr std::size_t kMaxFormBytes = 1024;

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
    /// Latches expiry: the portal loop calls this each poll; once true, submit() is refused and
    /// the secrets are wiped. Returns expired(now_rtc_us).
    bool tick(std::int64_t now_rtc_us) noexcept;
    /// Ends the session (AP stopped): wipes password and token, refuses further submits.
    void end() noexcept;
    [[nodiscard]] bool completed() const noexcept;
    std::string_view page() override;
    Result<std::string_view> submit(std::string_view form_body) override;
    /// Decodes application/x-www-form-urlencoded into the form (exposed for tests).
    static Result<ProvisioningForm> parse_form(std::string_view body,
                                               std::string_view expected_token) noexcept;

private:
    hal::System& system_;
    FormSink& sink_;
    FixedString<11> ssid_;
    Secret<64> password_;
    FixedString<16> token_;
    std::int64_t begin_rtc_us_ = 0;
    bool active_ = false;
    bool completed_ = false;
    std::array<char, 2048> page_{};
};

/// "Quartz-XXXX" from the low 16 bits of the chip id: the provisioning SSID and the Bluetooth name
/// match, so the owner recognizes the watch in either list.
[[nodiscard]] FixedString<11> device_name(std::uint64_t chip_id) noexcept;

/// Why a phone-sync session ends (ARCHITECTURE.md section 13a).
enum class PhoneEnd : std::uint8_t {
    kNone = 0,
    kNoPhone,     ///< no secure connection within the connect window
    kIdle,        ///< secure, but no command for the idle window
    kPhoneLeft,   ///< was secure, the phone disconnected: the normal end
    kMaxDuration, ///< hard cap, whatever happens
};

/// Phone-sync session timing. The link itself lives in hal::PhoneLink; this decides when to stop
/// it so the radio never stays up unattended. Pure; not thread-safe.
class PhoneSession {
public:
    void begin(std::int64_t now_rtc_us) noexcept;
    void end() noexcept;
    /// Feeds the link state seen by the app loop; returns why the session must end, if it must.
    PhoneEnd tick(hal::PhoneLinkState link, std::int64_t now_rtc_us) noexcept;
    /// A request line arrived over the secure link.
    void note_command(std::int64_t now_rtc_us) noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool was_secure() const noexcept { return was_secure_; }

private:
    std::int64_t begin_rtc_us_ = 0;
    std::int64_t last_activity_us_ = 0;
    bool active_ = false;
    bool was_secure_ = false;
};

} // namespace qz::conn
