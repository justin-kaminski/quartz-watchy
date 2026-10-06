// Shared helpers for the qz_power tests.
#pragma once

#include "qz/model/types.hpp"
#include "qz/power/power.hpp"

#include <cstdint>

namespace qz::power::testsupport {

/// Deterministic noise (xorshift32): identical on every run and platform.
class Noise {
public:
    explicit Noise(std::uint32_t seed) noexcept : state_(seed == 0U ? 1U : seed) {}

    /// Uniform in [-amplitude, +amplitude].
    [[nodiscard]] std::int32_t next(std::int32_t amplitude) noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        const std::uint32_t width = (2U * static_cast<std::uint32_t>(amplitude)) + 1U;
        return static_cast<std::int32_t>(state_ % width) - amplitude;
    }

private:
    std::uint32_t state_;
};

/// A state that already holds one sample (the filter is seeded) and sits at `level`.
[[nodiscard]] inline PowerState state_at(model::PowerLevel level, std::uint16_t mv) noexcept {
    PowerState state;
    state.level = level;
    state.filtered_mv = mv;
    state.valid = 1;
    return state;
}

/// One letter per level (N L S C), so that transition tables read as tables.
[[nodiscard]] inline char level_char(model::PowerLevel level) noexcept {
    switch (level) {
        case model::PowerLevel::kNormal:
            return 'N';
        case model::PowerLevel::kLow:
            return 'L';
        case model::PowerLevel::kSaver:
            return 'S';
        case model::PowerLevel::kCritical:
            return 'C';
    }
    return '?';
}

} // namespace qz::power::testsupport
