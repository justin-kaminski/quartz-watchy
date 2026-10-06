// Wall-clock service on top of the raw RTC time base (ARCHITECTURE.md section 8).
#pragma once

#include "qz/hal/system.hpp"
#include "qz/time/civil.hpp"
#include "qz/time/tz.hpp"

#include <cstdint>
#include <string_view>

namespace qz::time {

enum class TimeSource : std::uint8_t { kNone = 0, kManual, kSntp, kConsole };

/// Persistent part, embedded in app::RtcState. Trivially copyable; layout pinned by tests.
struct TimeKeeperState {
    std::int64_t anchor_rtc_us = 0;    ///< raw RTC time at the last set/sync
    std::int64_t anchor_utc_us = 0;    ///< UTC at anchor_rtc_us
    std::int64_t last_sync_utc_us = 0; ///< last successful SNTP (0 = never)
    std::int64_t last_sync_rtc_us = 0;
    std::int64_t last_set_utc_us = 0; ///< last manual/console set (0 = never)
    std::int32_t drift_ppb = 0;       ///< correction applied to elapsed RTC time (+ = RTC slow)
    TimeSource source = TimeSource::kNone;
    std::uint8_t valid = 0;          ///< 1 = wall time trustworthy
    std::uint8_t clock_degraded = 0; ///< 1 = slow clock not on the 32 kHz crystal
    std::uint8_t reserved = 0;
};

/// Tunables (ARCHITECTURE.md section 8.2).
struct DriftPolicy {
    std::int64_t min_interval_us =
        6LL * 3600 * 1'000'000;              ///< min elapsed between syncs to estimate
    std::int32_t max_residual_ppb = 500'000; ///< larger residual = clock fault
    std::int32_t max_drift_ppb = 200'000;    ///< clamp
    std::int32_t gain_divisor = 2;           ///< drift += residual / gain_divisor
};

struct TimeJump {
    UnixMicros old_utc_us = 0;
    UnixMicros new_utc_us = 0;
    bool was_valid = false;
};

struct SyncOutcome {
    TimeJump jump;
    std::int64_t residual_us = 0; ///< sntp - predicted (0 if there was no valid prediction)
    bool drift_updated = false;
    bool clock_fault = false;
};

/// Not thread-safe (app task only). Holds references; lifetimes owned by the app.
class TimeKeeper {
public:
    TimeKeeper(TimeKeeperState& state, hal::Clock& clock, DriftPolicy policy = {}) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] TimeSource source() const noexcept;
    /// Best estimate of UTC now (meaningful only when valid()).
    [[nodiscard]] UnixMicros now_utc_us() const noexcept;
    [[nodiscard]] UnixMicros utc_at_rtc(std::int64_t rtc_us) const noexcept;
    /// Inverse mapping for wake scheduling: raw RTC instant at which UTC reaches utc_us.
    [[nodiscard]] std::int64_t rtc_at_utc(UnixMicros utc_us) const noexcept;

    /// Manual/console set. Re-anchors, marks valid, restarts the drift window; pushes libc time.
    TimeJump set_utc(UnixMicros utc_us, TimeSource source) noexcept;
    /// Applies an SNTP result measured at raw RTC instant rtc_us (drift estimation per policy).
    SyncOutcome apply_sntp(UnixMicros utc_us, std::int64_t rtc_us) noexcept;
    /// Drift persisted in NVS (cold boot restore).
    void set_drift_ppb(std::int32_t drift_ppb) noexcept;
    [[nodiscard]] std::int32_t drift_ppb() const noexcept;
    void set_clock_degraded(bool degraded) noexcept;
    /// Cold boot / brownout: invalid time, keep drift.
    static void reset(TimeKeeperState& state) noexcept;

private:
    TimeKeeperState& state_;
    hal::Clock& clock_;
    DriftPolicy policy_;
};

/// Parses "YYYY-MM-DDTHH:MM[:SS]" with optional "Z" or "+HH:MM"; without an offset the value is
/// local time in `zone` (GapPolicy::kEarlier). Errc::kBadArgs on error.
Result<UnixSeconds> parse_iso8601(std::string_view text, const TimeZone& zone) noexcept;

} // namespace qz::time
