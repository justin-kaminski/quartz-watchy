// Battery-life estimate (ARCHITECTURE.md section 11, "Power estimate"). Integer-only.
#include "qz/power/power.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace qz::power {

std::uint32_t estimate_hours(const EstimateInputs& in) noexcept {
    // hours = capacity_uAh / average_uA, with
    //   average_uA = sleep_floor_uA + awake_ms x active_mA / 86'400
    // (awake mA*ms per day -> uA averaged over the day). Both sides are scaled by 86'400 so the
    // whole estimate is one exact integer division, floored: never promise more than the inputs
    // give. Bounds: awake time is clamped to one day, so every product stays below 2^63.
    const std::uint64_t awake_ms = std::min<std::uint64_t>(in.awake_ms_per_day, detail::kMsPerDay);
    const std::uint64_t charge =
        static_cast<std::uint64_t>(in.capacity_mah) * detail::kUaPerMa * detail::kMaMsPerDayPerUa;
    const std::uint64_t drain =
        (static_cast<std::uint64_t>(in.sleep_floor_ua) * detail::kMaMsPerDayPerUa) +
        (awake_ms * in.active_ma);
    if (charge == 0) {
        return 0; // no capacity, no hours
    }
    if (drain == 0) {
        return std::numeric_limits<std::uint32_t>::max(); // no drain: unbounded, saturated
    }
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(charge / drain, std::numeric_limits<std::uint32_t>::max()));
}

} // namespace qz::power
