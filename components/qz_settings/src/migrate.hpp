// Schema migration framework (ARCHITECTURE.md section 7): a table of steps, each one a pure
// function over the KvStore that moves a namespace from version N to N+1. Private to
// qz_settings; the host tests include it directly.
#pragma once

#include "qz/core/result.hpp"
#include "qz/hal/kv_store.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace qz::settings {

/// Upgrades one namespace from `from` to `from + 1`. Must be idempotent: a power loss between
/// the step and the version bump replays the step on the next boot. Must not commit.
struct MigrationStep {
    std::uint16_t from;
    Status (*apply)(hal::KvStore& kv) noexcept;
};

/// Runs steps `stored` .. `target - 1` from `steps` in order, bumping the namespace `ver` key after
/// each step, then commits once. kInvalidState if stored > target (data from newer firmware);
/// kCorrupt if a step is missing. On failure the namespace keeps the last completed version.
[[nodiscard]] Status run_migrations(hal::KvStore& kv,
                                    std::string_view ns,
                                    std::uint16_t stored,
                                    std::uint16_t target,
                                    std::span<const MigrationStep> steps) noexcept;

/// The production table for namespace qz_set.
[[nodiscard]] std::span<const MigrationStep> settings_migrations() noexcept;

} // namespace qz::settings
