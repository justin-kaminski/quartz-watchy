// conn::Scheduler: when to use the radio (ARCHITECTURE.md section 12). Pure, heap-free.
#include "qz/conn/conn.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <cstdint>

namespace qz::conn {
namespace {

using time::UnixSeconds;

constexpr std::int64_t kSecondsPerHour = 3600;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kMicrosPerSecond = 1'000'000;

/// murmur3 finalizer: spreads (seed, streak) over 32 bits.
constexpr std::uint32_t mix(std::uint32_t h) noexcept {
    h ^= h >> 16U;
    h *= 0x85EBCA6BU;
    h ^= h >> 13U;
    h *= 0xC2B2AE35U;
    h ^= h >> 16U;
    return h;
}

bool preconditions_hold(const Inputs& in) noexcept {
    return in.radio_compiled && in.mode != model::ConnectivityMode::kOff && in.has_credentials &&
           in.power == model::PowerLevel::kNormal;
}

constexpr std::int64_t sync_interval_s(const Inputs& in) noexcept {
    return std::max<std::int64_t>(static_cast<std::int64_t>(in.sync_interval_h) * kSecondsPerHour,
                                  tuning::kMinIntervalS);
}

constexpr std::int64_t weather_interval_s(const Inputs& in) noexcept {
    return std::max<std::int64_t>(static_cast<std::int64_t>(in.weather_interval_min) *
                                      kSecondsPerMinute,
                                  tuning::kMinIntervalS);
}

constexpr std::uint8_t saturating_inc(std::uint8_t v) noexcept {
    return v == UINT8_MAX ? v : static_cast<std::uint8_t>(v + 1U);
}

constexpr std::uint8_t error_code_byte(const Status& s) noexcept {
    return s ? std::uint8_t{0}
             : static_cast<std::uint8_t>(static_cast<unsigned>(s.error().code) + 1U);
}

/// First failing step of a session as the stored error byte (Errc + 1).
std::uint8_t first_error(const Plan& plan, const SessionResult& result) noexcept {
    std::uint8_t code = error_code_byte(result.connect);
    if (code == 0 && plan.time) {
        code = error_code_byte(result.time);
    }
    if (code == 0 && plan.weather) {
        code = error_code_byte(result.weather);
    }
    return code;
}

} // namespace

Scheduler::Scheduler(ConnState& state, std::uint32_t jitter_seed) noexcept
    : state_(state), seed_(jitter_seed) {}

std::int64_t Scheduler::backoff_base_s(std::uint8_t fail_streak, std::int64_t interval_s) noexcept {
    if (fail_streak == 0) {
        return 0;
    }
    const std::int64_t cap =
        std::min(std::max(interval_s, tuning::kMinIntervalS), tuning::kBackoffCapMaxS);
    const std::uint8_t shift = std::min<std::uint8_t>(static_cast<std::uint8_t>(fail_streak - 1U),
                                                      tuning::kMaxBackoffShift);
    const std::int64_t grown = tuning::kBackoffBaseS * (std::int64_t{1} << shift);
    return std::min(grown, cap);
}

std::int64_t Scheduler::backoff_delay_s(std::uint8_t fail_streak,
                                        std::int64_t interval_s,
                                        std::uint32_t seed) noexcept {
    const std::int64_t base = backoff_base_s(fail_streak, interval_s);
    if (base == 0) {
        return 0;
    }
    const std::uint32_t h = mix(seed ^ mix(static_cast<std::uint32_t>(fail_streak) * 0x9E3779B9U));
    constexpr std::uint32_t kSpan = (2U * static_cast<std::uint32_t>(tuning::kJitterPermille)) + 1U;
    const std::int64_t offset_permille =
        static_cast<std::int64_t>(h % kSpan) - tuning::kJitterPermille;
    return (base * (1000 + offset_permille)) / 1000;
}

std::int64_t Scheduler::retry_delay_s(const Inputs& in) const noexcept {
    return backoff_delay_s(state_.fail_streak, sync_interval_s(in), seed_);
}

Plan Scheduler::plan(const Inputs& in) const noexcept {
    if (!preconditions_hold(in)) {
        return {};
    }
    const bool weather_eligible =
        in.mode == model::ConnectivityMode::kTimeWeather && in.location_set && in.time_valid;
    if (in.manual_request) {
        return Plan{.time = true, .weather = weather_eligible};
    }
    if (!in.time_valid) {
        // Wall-clock time is unknown: sync it first; weather needs it (section 12 preconditions).
        return Plan{.time = true, .weather = false};
    }
    const UnixSeconds now = in.now_utc;
    bool time_due = now >= state_.next_time_sync;
    bool weather_due = weather_eligible && now >= state_.next_weather;
    if (time_due && weather_eligible && !weather_due &&
        state_.next_weather - now <= tuning::kPiggybackWindowS) {
        weather_due = true;
    }
    if (weather_due && !time_due && state_.next_time_sync - now <= tuning::kPiggybackWindowS) {
        time_due = true;
    }
    return Plan{.time = time_due, .weather = weather_due};
}

void Scheduler::on_result(const Plan& plan,
                          const SessionResult& result,
                          const Inputs& in) noexcept {
    if (!plan.any()) {
        return;
    }
    const bool connected = static_cast<bool>(result.connect);
    const bool time_ok = !plan.time || (connected && static_cast<bool>(result.time));
    const bool weather_ok = !plan.weather || (connected && static_cast<bool>(result.weather));
    const bool all_ok = time_ok && weather_ok;

    // "Now" at result application: the caller's clock if valid, else the SNTP answer.
    bool now_known = in.time_valid;
    UnixSeconds now = in.now_utc;
    if (!now_known && result.sntp_utc_us.has_value()) {
        now_known = true;
        now = *result.sntp_utc_us / kMicrosPerSecond;
    }

    state_.sessions = state_.sessions == UINT32_MAX ? state_.sessions : state_.sessions + 1U;
    if (now_known) {
        state_.last_attempt_utc = now;
    }
    if (all_ok) {
        state_.fail_streak = 0;
        state_.last_error = 0;
        state_.last_ok_utc = now_known ? now : state_.last_ok_utc;
    } else {
        state_.fail_streak = saturating_inc(state_.fail_streak);
        state_.last_error = first_error(plan, result);
    }
    if (plan.time && time_ok) {
        state_.ever_synced = 1;
    }
    if (!now_known) {
        return; // cannot express next-due in UTC; the app paces with retry_delay_s()
    }
    if (plan.time) {
        const std::int64_t interval = sync_interval_s(in);
        state_.next_time_sync =
            now + (time_ok ? interval : backoff_delay_s(state_.fail_streak, interval, seed_));
    }
    if (plan.weather) {
        const std::int64_t interval = weather_interval_s(in);
        state_.next_weather =
            now + (weather_ok ? interval : backoff_delay_s(state_.fail_streak, interval, seed_));
    }
}

model::SyncIndicator Scheduler::indicator(const Inputs& in,
                                          time::UnixSeconds last_time_sync_utc) const noexcept {
    using model::SyncIndicator;
    if (!in.radio_compiled || in.mode == model::ConnectivityMode::kOff || !in.has_credentials) {
        return SyncIndicator::kNone;
    }
    if (state_.fail_streak > 0) {
        return SyncIndicator::kLastFailed;
    }
    if (state_.ever_synced == 0 && last_time_sync_utc == 0) {
        return SyncIndicator::kNeverSynced;
    }
    if (in.time_valid && last_time_sync_utc != 0 &&
        in.now_utc - last_time_sync_utc > tuning::kStaleIntervals * sync_interval_s(in)) {
        return SyncIndicator::kStale;
    }
    return SyncIndicator::kOk;
}

void Scheduler::on_config_changed() noexcept {
    state_.next_time_sync = 0;
    state_.next_weather = 0;
    state_.fail_streak = 0;
    state_.last_error = 0;
}

} // namespace qz::conn
