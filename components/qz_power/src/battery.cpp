// Battery measurement helpers: LiPo curve, robust mean, board divider (ARCHITECTURE.md section 11).
// Integer-only, no heap.
#include "qz/core/assert.hpp"
#include "qz/power/power.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace qz::power {
namespace {

constexpr std::uint32_t kPercentMax = 100;

std::uint8_t clamp_percent(std::uint32_t percent) noexcept {
    return static_cast<std::uint8_t>(std::min(percent, kPercentMax));
}

} // namespace

std::span<const CurvePoint> default_curve() noexcept {
    return std::span<const CurvePoint>{detail::kDefaultCurve};
}

std::uint8_t percent_from_mv(std::uint16_t mv, std::span<const CurvePoint> curve) noexcept {
    QZ_ASSERT(detail::curve_is_valid(curve));
    if (mv >= curve.front().mv) {
        return clamp_percent(curve.front().percent);
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const CurvePoint& upper = curve[i - 1];
        const CurvePoint& lower = curve[i];
        if (mv >= lower.mv) {
            // mv is in [lower.mv, upper.mv). The segment is valid (strictly descending mV,
            // non-increasing percent), so every difference below is positive.
            const auto span_mv = static_cast<std::uint32_t>(upper.mv - lower.mv);
            const auto span_percent = static_cast<std::uint32_t>(upper.percent - lower.percent);
            const auto offset_mv = static_cast<std::uint32_t>(mv - lower.mv);
            // Rounded to nearest, halves up. Exact at the segment's lower end (offset 0), and the
            // upper end is the next segment's lower end, so the curve is exact at every point.
            const std::uint32_t rise = ((offset_mv * span_percent * 2U) + span_mv) / (2U * span_mv);
            return clamp_percent(static_cast<std::uint32_t>(lower.percent) + rise);
        }
    }
    return clamp_percent(curve.back().percent);
}

std::uint16_t robust_mean_mv(std::span<const std::uint16_t> samples) noexcept {
    if (samples.empty()) {
        return 0;
    }
    // 64-bit sum: a span of more than 65'536 full-scale samples would overflow 32 bits.
    std::uint64_t sum = 0;
    std::uint16_t lowest = samples.front();
    std::uint16_t highest = samples.front();
    for (const std::uint16_t sample : samples) {
        sum += sample;
        lowest = std::min(lowest, sample);
        highest = std::max(highest, sample);
    }
    std::uint64_t count = samples.size();
    if (samples.size() >= detail::kRobustMeanMinSamples) {
        sum -= lowest; // one min and one max, even when several samples share the extreme value
        sum -= highest;
        count -= 2U;
    }
    // Rounded to nearest, halves up; a mean never exceeds the largest sample, so it fits.
    return static_cast<std::uint16_t>((sum + (count / 2U)) / count);
}

std::uint16_t
battery_mv_from_pin(std::uint16_t pin_mv, std::uint16_t num, std::uint16_t den) noexcept {
    QZ_ASSERT(den != 0);
    // 65'535 * 65'535 + 32'767 still fits in 32 bits.
    const std::uint32_t scaled =
        ((static_cast<std::uint32_t>(pin_mv) * num) + (static_cast<std::uint32_t>(den) / 2U)) / den;
    return static_cast<std::uint16_t>(
        std::min<std::uint32_t>(scaled, std::numeric_limits<std::uint16_t>::max()));
}

} // namespace qz::power
