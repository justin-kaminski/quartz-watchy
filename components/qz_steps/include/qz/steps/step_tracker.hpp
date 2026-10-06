// Step-day accounting over the BMA423 hardware counter (ARCHITECTURE.md section 10).
#pragma once

#include "qz/core/result.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/model/types.hpp"
#include "qz/time/civil.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace qz::steps {

/// Persistent part, embedded in app::RtcState. Trivially copyable.
struct StepState {
    std::uint32_t last_hw_count = 0;
    std::uint32_t today = 0;
    std::uint32_t pending = 0; ///< steps counted while time was invalid
    time::DayNumber today_day = 0;
    time::DayNumber last_flush_day = 0;
    time::UnixSeconds last_read_utc = 0;
    std::array<model::StepDay, model::kStepHistoryDays> history{}; ///< newest first
    std::uint8_t history_count = 0;
    std::uint8_t has_baseline = 0;  ///< last_hw_count valid
    std::uint8_t today_valid = 0;   ///< today_day valid
    std::uint8_t goal_notified = 0; ///< goal vibration already done today
};

struct StepUpdate {
    std::uint32_t delta = 0;
    bool rolled_over = false;      ///< a new local day started (history pushed)
    bool flush_due = false;        ///< caller should persist history now (StepHistoryStore)
    bool goal_reached_now = false; ///< crossed the goal on this sample
    bool time_went_back = false;   ///< local day moved backwards (counted into today)
};

/// Not thread-safe. References the state owned by the app.
class StepTracker {
public:
    explicit StepTracker(StepState& state) noexcept;
    /// Feed one hardware reading. local_day empty = time invalid (pending bucket).
    StepUpdate on_sample(std::uint32_t hw_count,
                         std::optional<time::DayNumber> local_day,
                         time::UnixSeconds now_utc,
                         std::uint32_t goal) noexcept;
    /// Wall time changed (manual set/SNTP): re-evaluate the day without a new reading.
    StepUpdate on_time_jump(std::optional<time::DayNumber> local_day,
                            time::UnixSeconds now_utc) noexcept;
    /// Sensor was re-initialized: next reading is a new baseline.
    void on_sensor_reset() noexcept;
    [[nodiscard]] model::StepsSummary summary(std::uint32_t goal) const noexcept;
    void inject(std::int32_t delta) noexcept; ///< console/simulator; clamps at 0
    void reset_today() noexcept;
    static void reset(StepState& state) noexcept; ///< cold boot without NVS history

private:
    StepState& state_;
};

/// Daily flush of today + history to NVS namespace qz_steps (<= 1 write per day).
class StepHistoryStore {
public:
    explicit StepHistoryStore(hal::KvStore& kv) noexcept;
    Status save(const StepState& state) noexcept;
    /// Restores history (and today's flushed value) after RTC loss.
    Status load(StepState& state) noexcept;
    Status erase() noexcept;

private:
    hal::KvStore& kv_;
};

} // namespace qz::steps
