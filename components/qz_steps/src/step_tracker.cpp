// Step-day accounting (ARCHITECTURE.md section 10). Pure, integer-only, no heap.
#include "qz/steps/step_tracker.hpp"

#include "qz/core/log.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <limits>

namespace qz::steps {
namespace {

constexpr const char* kTag = "steps";
constexpr std::int64_t kHistoryDays = static_cast<std::int64_t>(model::kStepHistoryDays);

std::uint32_t sat_add(std::uint32_t a, std::uint32_t b) noexcept {
    const std::uint64_t sum = static_cast<std::uint64_t>(a) + b;
    return sum > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(sum);
}

/// Inserts `entry` as the newest history day, dropping the oldest when full.
void push_history(StepState& st, const model::StepDay& entry) noexcept {
    for (std::size_t i = st.history.size() - 1; i > 0; --i) {
        st.history[i] = st.history[i - 1];
    }
    st.history[0] = entry;
    if (st.history_count < st.history.size()) {
        ++st.history_count;
    }
}

/// Closes `st.today_day` and every skipped day up to (excluding) `day`, then opens `day`.
/// Only the last kStepHistoryDays days can survive in the history, so a huge gap costs 7 pushes.
void roll_over(StepState& st, time::DayNumber day) noexcept {
    const std::int64_t first =
        std::max<std::int64_t>(st.today_day, static_cast<std::int64_t>(day) - kHistoryDays);
    for (std::int64_t d = first; d < day; ++d) {
        const bool is_closing_day = d == st.today_day;
        push_history(
            st, model::StepDay{static_cast<time::DayNumber>(d), is_closing_day ? st.today : 0U});
    }
    st.today = 0;
    st.today_day = day;
    st.goal_notified = 0;
}

/// Shared by on_sample / on_time_jump: places `delta` into pending / yesterday / today.
StepUpdate advance(StepState& st,
                   std::uint32_t delta,
                   std::optional<time::DayNumber> local_day,
                   time::UnixSeconds now_utc,
                   std::uint32_t goal) noexcept {
    StepUpdate up;
    up.delta = delta;
    if (!local_day) {
        st.pending = sat_add(st.pending, delta); // time invalid: bucket until time is known
        return up;
    }
    const time::DayNumber day = *local_day;

    if (st.today_valid == 0) {
        st.today_day = day; // first valid time ever (or after cold boot): nothing to close
        st.today_valid = 1;
        st.today = sat_add(st.today, delta);
    } else if (day > st.today_day) {
        const std::int64_t skipped = static_cast<std::int64_t>(day) - st.today_day;
        roll_over(st, day);
        up.rolled_over = true;
        const std::int64_t gap_s = now_utc - st.last_read_utc;
        const bool last_read_before_boundary =
            st.last_read_utc > 0 && gap_s >= 0 && gap_s <= tuning::kBoundaryGraceS && skipped == 1;
        if (last_read_before_boundary) {
            st.history[0].steps = sat_add(st.history[0].steps, delta); // steps walked pre-midnight
        } else {
            st.today = delta;
        }
        if (st.last_flush_day != day) {
            st.last_flush_day = day;
            up.flush_due = true;
        }
    } else {
        if (day < st.today_day) {
            up.time_went_back = true; // never roll back: keep counting into today_day
            QZ_LOGI(kTag,
                    "time_back day=%d today_day=%d",
                    static_cast<int>(day),
                    static_cast<int>(st.today_day));
        }
        st.today = sat_add(st.today, delta);
    }

    st.today = sat_add(st.today, st.pending); // pending belongs to today once time is valid
    st.pending = 0;
    st.last_read_utc = now_utc;

    if (goal > 0 && st.today >= goal && st.goal_notified == 0) {
        st.goal_notified = 1;
        up.goal_reached_now = true;
    }
    return up;
}

} // namespace

StepTracker::StepTracker(StepState& state) noexcept : state_(state) {}

StepUpdate StepTracker::on_sample(std::uint32_t hw_count,
                                  std::optional<time::DayNumber> local_day,
                                  time::UnixSeconds now_utc,
                                  std::uint32_t goal) noexcept {
    std::uint32_t delta = 0;
    if (state_.has_baseline == 0) {
        state_.has_baseline = 1; // first reading only establishes the baseline
    } else if (hw_count >= state_.last_hw_count) {
        delta = hw_count - state_.last_hw_count;
    } else {
        delta = hw_count; // counter below baseline: the sensor was reset and restarted at 0
    }
    state_.last_hw_count = hw_count;
    return advance(state_, delta, local_day, now_utc, goal);
}

StepUpdate StepTracker::on_time_jump(std::optional<time::DayNumber> local_day,
                                     time::UnixSeconds now_utc) noexcept {
    if (!local_day) {
        return {};
    }
    return advance(state_, 0, local_day, now_utc, 0);
}

void StepTracker::on_sensor_reset() noexcept {
    state_.has_baseline = 0;
}

model::StepsSummary StepTracker::summary(std::uint32_t goal) const noexcept {
    model::StepsSummary s;
    s.today = state_.today;
    s.goal = goal;
    s.history = state_.history;
    s.history_count = state_.history_count;
    return s;
}

void StepTracker::inject(std::int32_t delta) noexcept {
    const std::int64_t value = static_cast<std::int64_t>(state_.today) + delta;
    state_.today = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(value, 0, std::numeric_limits<std::uint32_t>::max()));
}

void StepTracker::reset_today() noexcept {
    state_.today = 0;
    state_.pending = 0;
    state_.goal_notified = 0;
}

void StepTracker::reset(StepState& state) noexcept {
    state = StepState{};
}

} // namespace qz::steps
