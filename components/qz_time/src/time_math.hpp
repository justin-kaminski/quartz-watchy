// Private helpers shared by the qz_time sources. Not part of the public contract.
#pragma once

#include <cstdint>
#include <limits>

namespace qz::time::detail {

inline constexpr std::int64_t kSecondsPerHour = 3'600;

/// Floor division for b > 0: rounds toward negative infinity (operator/ rounds toward zero).
[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t quotient = a / b;
    return ((a % b) < 0) ? (quotient - 1) : quotient;
}

/// Floor modulo for b > 0: the result is always in [0, b).
[[nodiscard]] constexpr std::int64_t floor_mod(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t remainder = a % b;
    return (remainder < 0) ? (remainder + b) : remainder;
}

/// Narrows to int32 by clamping, so out-of-range input saturates instead of overflowing.
[[nodiscard]] constexpr std::int32_t saturate_to_i32(std::int64_t value) noexcept {
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    if (value < kMin) {
        return kMin;
    }
    if (value > kMax) {
        return kMax;
    }
    return static_cast<std::int32_t>(value);
}

} // namespace qz::time::detail
