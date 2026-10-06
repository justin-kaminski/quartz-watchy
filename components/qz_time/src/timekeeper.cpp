// TimeKeeper (ARCHITECTURE.md section 8.2): wall time = anchor + elapsed raw RTC time scaled by
// the drift estimate. All arithmetic is int64 and total: products are split into quotient and
// remainder so nothing overflows for any state, and extreme inputs saturate instead of wrapping.
//
// Mapping, with p = drift_ppb and rate = 1e9 + p:
//   utc(rtc) = anchor_utc + d + floor(d * p / 1e9) = anchor_utc + floor(d * rate / 1e9),
//   d = rtc - anchor_rtc.
// The inverse is exact: the first raw RTC instant at which utc() reaches u is
//   d = ceil((u - anchor_utc) * 1e9 / rate).
#include "qz/time/timekeeper.hpp"

#include "qz/core/assert.hpp"
#include "time_math.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace qz::time {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] constexpr std::int64_t sat_add(std::int64_t a, std::int64_t b) noexcept {
    if (b > 0 && a > kInt64Max - b) {
        return kInt64Max;
    }
    if (b < 0 && a < kInt64Min - b) {
        return kInt64Min;
    }
    return a + b;
}

[[nodiscard]] constexpr std::int64_t sat_sub(std::int64_t a, std::int64_t b) noexcept {
    if (b < 0 && a > kInt64Max + b) {
        return kInt64Max;
    }
    if (b > 0 && a < kInt64Min + b) {
        return kInt64Min;
    }
    return a - b;
}

[[nodiscard]] constexpr std::int64_t abs_i64(std::int64_t v) noexcept {
    return (v < 0) ? -v : v; // callers keep |v| far from int64 min
}

/// Drift as used by the mapping: whatever the state holds, bounded so the rate stays positive.
[[nodiscard]] constexpr std::int64_t mapping_drift(const TimeKeeperState& s) noexcept {
    return std::clamp<std::int64_t>(
        s.drift_ppb, -tuning::kHardDriftLimitPpb, tuning::kHardDriftLimitPpb);
}

/// floor(d * p / 1e9) for any int64 d and |p| <= 5e8, without overflow: d = q * 1e9 + r gives
/// q * p + floor(r * p / 1e9), |q * p| < 4.7e18 and |r * p| < 5e17.
[[nodiscard]] constexpr std::int64_t drift_correction(std::int64_t d, std::int64_t p) noexcept {
    const std::int64_t q = detail::floor_div(d, tuning::kPpbScale);
    const std::int64_t r = detail::floor_mod(d, tuning::kPpbScale);
    return (q * p) + detail::floor_div(r * p, tuning::kPpbScale);
}

[[nodiscard]] constexpr std::int64_t map_to_utc(const TimeKeeperState& s,
                                                std::int64_t rtc_us) noexcept {
    const std::int64_t d = sat_sub(rtc_us, s.anchor_rtc_us);
    const std::int64_t scaled = sat_add(d, drift_correction(d, mapping_drift(s)));
    return sat_add(s.anchor_utc_us, scaled);
}

[[nodiscard]] constexpr std::int64_t map_to_rtc(const TimeKeeperState& s,
                                                std::int64_t utc_us) noexcept {
    const std::int64_t rate = tuning::kPpbScale + mapping_drift(s); // [5e8, 1.5e9]
    const std::int64_t du = sat_sub(utc_us, s.anchor_utc_us);
    const std::int64_t q = detail::floor_div(du, rate);
    const std::int64_t r = detail::floor_mod(du, rate); // [0, rate)
    // du * 1e9 / rate = q * 1e9 + r * 1e9 / rate; r * 1e9 < 1.5e18, so the tail cannot overflow.
    const std::int64_t tail = ((r * tuning::kPpbScale) + rate - 1) / rate; // ceil
    if (q > kInt64Max / tuning::kPpbScale) {
        return kInt64Max;
    }
    if (q < kInt64Min / tuning::kPpbScale) {
        return kInt64Min;
    }
    const std::int64_t d = sat_add(q * tuning::kPpbScale, tail);
    return sat_add(s.anchor_rtc_us, d);
}

} // namespace

TimeKeeper::TimeKeeper(TimeKeeperState& state, hal::Clock& clock, DriftPolicy policy) noexcept
    : state_(state), clock_(clock), policy_(policy) {
    QZ_ASSERT(policy_.max_drift_ppb >= 0 && policy_.max_drift_ppb <= tuning::kHardDriftLimitPpb);
    QZ_ASSERT(policy_.max_residual_ppb >= 0);
    QZ_ASSERT(policy_.gain_divisor >= 1);
    QZ_ASSERT(policy_.min_interval_us >= 0);
}

bool TimeKeeper::valid() const noexcept {
    // An RTC that reads earlier than the anchor restarted (power loss): the anchor is meaningless.
    return state_.valid != 0 && clock_.rtc_us() >= state_.anchor_rtc_us;
}

TimeSource TimeKeeper::source() const noexcept {
    return state_.source;
}

UnixMicros TimeKeeper::now_utc_us() const noexcept {
    return map_to_utc(state_, clock_.rtc_us());
}

UnixMicros TimeKeeper::utc_at_rtc(std::int64_t rtc_us) const noexcept {
    return map_to_utc(state_, rtc_us);
}

std::int64_t TimeKeeper::rtc_at_utc(UnixMicros utc_us) const noexcept {
    return map_to_rtc(state_, utc_us);
}

TimeJump TimeKeeper::set_utc(UnixMicros utc_us, TimeSource source) noexcept {
    QZ_ASSERT(source == TimeSource::kManual || source == TimeSource::kConsole);
    const std::int64_t rtc_us = clock_.rtc_us();
    const bool was_valid = valid();
    const TimeJump jump{was_valid ? map_to_utc(state_, rtc_us) : 0, utc_us, was_valid};

    state_.anchor_rtc_us = rtc_us;
    state_.anchor_utc_us = utc_us;
    state_.last_set_utc_us = utc_us;
    state_.source = source; // not kSntp: the next sync re-anchors without estimating drift
    state_.valid = 1;
    clock_.set_system_utc_us(utc_us);
    return jump;
}

SyncOutcome TimeKeeper::apply_sntp(UnixMicros utc_us, std::int64_t rtc_us) noexcept {
    SyncOutcome out;
    const bool was_valid = valid();
    const std::int64_t predicted = was_valid ? map_to_utc(state_, rtc_us) : 0;
    out.jump = TimeJump{predicted, utc_us, was_valid};
    if (was_valid) {
        out.residual_us = sat_sub(utc_us, predicted);
    }

    // Drift is estimated only between two consecutive SNTP anchors (a manual set restarts the
    // window) that are at least min_interval apart; anything else just re-anchors.
    const std::int64_t elapsed_us = sat_sub(rtc_us, state_.anchor_rtc_us);
    const bool eligible = was_valid && state_.source == TimeSource::kSntp && elapsed_us > 0 &&
                          elapsed_us >= policy_.min_interval_us;
    if (eligible) {
        const std::int64_t residual_abs_us = abs_i64(out.residual_us);
        const std::int64_t residual_ppb =
            (residual_abs_us > tuning::kMaxResidualForPpbUs)
                ? kInt64Max
                : (out.residual_us * tuning::kPpbScale) / elapsed_us; // truncates toward zero
        if (abs_i64(residual_ppb) > policy_.max_residual_ppb) {
            out.clock_fault = true; // anchor only; the estimate stays as it was
        } else {
            const std::int64_t divisor =
                (state_.drift_ppb == 0) ? tuning::kBootstrapGainDivisor : policy_.gain_divisor;
            const std::int64_t updated = state_.drift_ppb + (residual_ppb / divisor);
            state_.drift_ppb = static_cast<std::int32_t>(
                std::clamp<std::int64_t>(updated, -policy_.max_drift_ppb, policy_.max_drift_ppb));
            out.drift_updated = true;
        }
    }

    state_.anchor_rtc_us = rtc_us;
    state_.anchor_utc_us = utc_us;
    state_.last_sync_utc_us = utc_us;
    state_.last_sync_rtc_us = rtc_us;
    state_.source = TimeSource::kSntp;
    state_.valid = 1;
    clock_.set_system_utc_us(map_to_utc(state_, clock_.rtc_us()));
    return out;
}

void TimeKeeper::set_drift_ppb(std::int32_t drift_ppb) noexcept {
    // Meant for the cold-boot restore while the time is still invalid; on a valid keeper the
    // current estimate steps because the anchor is not moved.
    state_.drift_ppb = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(drift_ppb, -policy_.max_drift_ppb, policy_.max_drift_ppb));
}

std::int32_t TimeKeeper::drift_ppb() const noexcept {
    return state_.drift_ppb;
}

void TimeKeeper::set_clock_degraded(bool degraded) noexcept {
    state_.clock_degraded = degraded ? 1 : 0;
}

void TimeKeeper::reset(TimeKeeperState& state) noexcept {
    const std::int32_t drift_ppb = state.drift_ppb; // survives: it is a property of the crystal
    state = TimeKeeperState{};
    state.drift_ppb = drift_ppb;
}

} // namespace qz::time
