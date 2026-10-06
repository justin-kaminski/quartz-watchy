// conn::SyncSession: one radio session, connect -> SNTP -> weather -> shutdown (section 12).
// Heap-free (the parser behind weather::Provider may allocate: radio sessions are allowed to).
#include "qz/conn/conn.hpp"
#include "qz/core/log.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace qz::conn {
namespace {

constexpr const char* kTag = "conn";
constexpr std::int64_t kUsPerMs = 1000;
constexpr std::int64_t kUsPerS = 1'000'000;
constexpr std::uint16_t kHttpOk = 200;

/// Logs an error code by its stable token (never any request/credential data).
void log_failure(const char* what, Errc code) noexcept {
    const std::string_view token = to_token(code);
    QZ_LOGW(kTag, "%s failed: %.*s", what, static_cast<int>(token.size()), token.data());
}

/// State and steps of one session; lives on the stack of SyncSession::run.
class Run {
public:
    Run(hal::NetStack& net,
        const weather::Provider& provider,
        hal::Clock& clock,
        std::span<char> body,
        const Plan& plan,
        const Budget& budget,
        time::UnixSeconds now_utc) noexcept
        : net_(net), provider_(provider), clock_(clock), body_(body), plan_(plan), budget_(budget),
          now_utc_(now_utc), start_us_(clock.rtc_us()) {}

    [[nodiscard]] SessionResult execute(const hal::WifiCredentials& creds,
                                        const model::Location& loc) noexcept {
        if (remaining_ms() <= 0) {
            // Zero budget: nothing was started, nothing to tear down.
            return reject(Errc::kTimeout);
        }
        res_.connect = net_.connect(creds, step_timeout_ms(budget_.connect_ms));
        if (!res_.connect) {
            log_failure("connect", res_.connect.error().code);
            fail_planned(res_.connect.error());
        } else {
            if (plan_.time) {
                sync_time();
            }
            if (plan_.weather) {
                fetch_weather(loc);
            }
        }
        net_.shutdown(); // teardown on every path that started the radio
        const std::int64_t d_ms = (clock_.rtc_us() - start_us_) / kUsPerMs;
        res_.duration_ms =
            static_cast<std::uint32_t>(std::clamp<std::int64_t>(d_ms, 0, UINT32_MAX));
        return res_;
    }

    /// Fails the connect and every planned job without touching the radio.
    [[nodiscard]] SessionResult reject(Errc code) noexcept {
        res_.connect = code;
        fail_planned(Error{code});
        return res_;
    }

private:
    void fail_planned(Error e) noexcept {
        if (plan_.time) {
            res_.time = e;
        }
        if (plan_.weather) {
            res_.weather = e;
        }
    }
    /// Time left in the whole-session budget, in ms (<= 0: expired).
    [[nodiscard]] std::int64_t remaining_ms() const noexcept {
        const std::int64_t elapsed_ms = (clock_.rtc_us() - start_us_) / kUsPerMs;
        return static_cast<std::int64_t>(budget_.total_ms) - elapsed_ms;
    }
    /// Per-step timeout: the step budget capped by what is left of the session.
    [[nodiscard]] std::uint32_t step_timeout_ms(std::uint32_t step_ms) const noexcept {
        return static_cast<std::uint32_t>(std::min<std::int64_t>(step_ms, remaining_ms()));
    }
    /// UTC (seconds) now: this session's SNTP time or the caller's, advanced by RTC elapsed;
    /// 0 = unknown.
    [[nodiscard]] time::UnixSeconds utc_now_s() const noexcept {
        if (synced_) {
            return (sntp_utc_us_ + (clock_.rtc_us() - sntp_set_rtc_us_)) / kUsPerS;
        }
        return now_utc_ == 0 ? 0 : now_utc_ + ((clock_.rtc_us() - start_us_) / kUsPerS);
    }

    void sync_time() noexcept {
        if (remaining_ms() <= 0) {
            res_.time = Errc::kTimeout;
            return;
        }
        std::int64_t rtc_at_utc = 0;
        const Result<std::int64_t> r =
            net_.sntp_sync(step_timeout_ms(budget_.sntp_ms), &rtc_at_utc);
        if (!r) {
            res_.time = r.error();
            log_failure("sntp", r.error().code);
        } else if (*r < tuning::kMinPlausibleUtcUs) {
            res_.time = Errc::kCorrupt;
            QZ_LOGW(kTag, "sntp answer implausible");
        } else {
            res_.sntp_utc_us = *r;
            res_.sntp_rtc_us = rtc_at_utc;
            // Corrected UTC *now*, so TLS certificate validation sees the right date.
            sntp_set_rtc_us_ = clock_.rtc_us();
            sntp_utc_us_ = *r + (sntp_set_rtc_us_ - rtc_at_utc);
            clock_.set_system_utc_us(sntp_utc_us_);
            synced_ = true;
        }
    }

    void fetch_weather(const model::Location& loc) noexcept {
        if (utc_now_s() == 0) {
            res_.weather = Errc::kNoTime;
            return;
        }
        if (remaining_ms() <= 0) {
            res_.weather = Errc::kTimeout;
            return;
        }
        std::array<char, weather::kMaxUrlBytes> url{};
        const Result<std::size_t> n = provider_.build_url(loc, url);
        if (!n) {
            res_.weather = n.error();
            return;
        }
        const Result<hal::HttpResponse> http = net_.https_get(
            std::string_view{url.data(), *n}, body_, step_timeout_ms(budget_.weather_ms));
        if (!http) {
            res_.weather = http.error();
            log_failure("weather request", http.error().code);
        } else if (http->status != kHttpOk) {
            res_.weather = Error{Errc::kIo, http->status};
            QZ_LOGW(kTag, "weather http status %u", static_cast<unsigned>(http->status));
        } else if (http->truncated || http->body_len > body_.size()) {
            res_.weather = Errc::kCorrupt;
        } else {
            const Result<model::WeatherReport> rep =
                provider_.parse(std::string_view{body_.data(), http->body_len}, utc_now_s());
            if (rep) {
                res_.report = *rep;
            } else {
                res_.weather = rep.error();
                log_failure("weather parse", rep.error().code);
            }
        }
    }

    hal::NetStack& net_;
    const weather::Provider& provider_;
    hal::Clock& clock_;
    std::span<char> body_;
    const Plan& plan_;
    const Budget& budget_;
    time::UnixSeconds now_utc_;
    std::int64_t start_us_;
    SessionResult res_;
    bool synced_ = false; ///< this session obtained UTC from SNTP
    std::int64_t sntp_utc_us_ = 0;
    std::int64_t sntp_set_rtc_us_ = 0;
};

} // namespace

SyncSession::SyncSession(hal::NetStack& net,
                         const weather::Provider& provider,
                         hal::Clock& clock) noexcept
    : net_(net), provider_(provider), clock_(clock) {}

SessionResult SyncSession::run(const Plan& plan,
                               const hal::WifiCredentials& creds,
                               const model::Location& loc,
                               const Budget& budget,
                               time::UnixSeconds now_utc) noexcept {
    if (!plan.any()) {
        return SessionResult{};
    }
    Run run(net_, provider_, clock_, body_, plan, budget, now_utc);
    return creds.ssid.empty() ? run.reject(Errc::kNoCredentials) : run.execute(creds, loc);
}

} // namespace qz::conn
