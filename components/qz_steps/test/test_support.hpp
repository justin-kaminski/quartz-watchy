// Shared helpers for the qz_steps host tests: a virtual-time rig around StepTracker.
#pragma once

#include "qz/steps/step_tracker.hpp"
#include "qz/time/civil.hpp"
#include "qz/time/tz.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace qz::steps::test {

inline time::UnixSeconds
utc(std::int32_t y, std::uint8_t mo, std::uint8_t d, int h = 0, int mi = 0, int s = 0) {
    return static_cast<std::int64_t>(time::days_from_civil({y, mo, d})) * time::kSecondsPerDay +
           h * 3600LL + mi * 60LL + s;
}

inline time::DayNumber day_num(std::int32_t y, std::uint8_t mo, std::uint8_t d) {
    return time::days_from_civil({y, mo, d});
}

inline time::TimeZone tz_of(std::string_view posix) {
    const auto r = time::TimeZone::parse(posix);
    if (!r) {
        ADD_FAILURE() << "bad POSIX TZ " << posix;
        return {};
    }
    return *r;
}

inline time::DayNumber local_day(const time::TimeZone& tz, time::UnixSeconds t) {
    return time::days_from_civil(tz.to_local(t).date);
}

inline constexpr std::string_view kBerlin = "CET-1CEST,M3.5.0,M10.5.0/3";
inline constexpr std::string_view kSydney = "AEST-10AEDT,M10.1.0,M4.1.0/3";
inline constexpr std::string_view kSantiago = "<-04>4<-03>,M9.1.6/24,M4.1.6/24";
inline constexpr std::string_view kHavana = "CST5CDT,M3.2.0/0,M11.1.0/1";
/// DST ends at 00:30 DST: the wall clock jumps from day X 00:30 back to day X-1 23:30.
inline constexpr std::string_view kMidnightBack = "XST5XDT,M3.2.0,M11.1.0/0:30";

/// Virtual-time driver: the app side of the tracker contract (local day from TimeZone).
struct Rig {
    StepState state{};
    StepTracker tracker{state};
    time::TimeZone tz;
    std::uint32_t hw = 0;
    std::uint32_t goal = 0;

    explicit Rig(std::string_view posix = "UTC0") : tz(tz_of(posix)) {}

    /// The hardware counter advances by `steps`, then the app samples it at time `t`.
    StepUpdate walk(time::UnixSeconds t, std::uint32_t steps) {
        hw += steps;
        return tracker.on_sample(hw, local_day(tz, t), t, goal);
    }
    /// Absolute counter value.
    StepUpdate read(time::UnixSeconds t, std::uint32_t counter) {
        hw = counter;
        return tracker.on_sample(hw, local_day(tz, t), t, goal);
    }
    StepUpdate walk_no_time(std::uint32_t steps) {
        hw += steps;
        return tracker.on_sample(hw, std::nullopt, 0, goal);
    }
    StepUpdate jump(time::UnixSeconds t) { return tracker.on_time_jump(local_day(tz, t), t); }
};

/// Steps recorded for `day` in the history, nullopt if the day is not in it.
inline std::optional<std::uint32_t> history_steps(const StepState& st, time::DayNumber day) {
    for (std::size_t i = 0; i < st.history_count; ++i) {
        if (st.history[i].day == day) {
            return st.history[i].steps;
        }
    }
    return std::nullopt;
}

/// Steps recorded for `day`; reports a test failure (and returns UINT32_MAX) if it is missing.
inline std::uint32_t steps_on(const StepState& st, time::DayNumber day) {
    const auto got = history_steps(st, day);
    if (!got.has_value()) {
        ADD_FAILURE() << "day " << day << " not in history";
        return UINT32_MAX;
    }
    return *got;
}

inline constexpr std::int64_t kHourS = 3600;
inline constexpr std::int64_t kMinuteS = 60;
inline constexpr std::int64_t kDayS = 86'400;

} // namespace qz::steps::test
