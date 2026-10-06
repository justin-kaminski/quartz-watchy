#include "migrate.hpp"

#include "qz/settings/settings.hpp"
#include "tuning.hpp"

#include <array>

namespace qz::settings {
namespace {

/// v0 = a qz_set namespace written before versioning existed (keys, no `ver`). The v1 layout is
/// identical, so this step only stamps the version (done by run_migrations).
Status migrate_v0_to_v1(hal::KvStore& /*kv*/) noexcept {
    return ok();
}

constexpr std::array<MigrationStep, 1> kSettingsSteps{{{0, &migrate_v0_to_v1}}};

} // namespace

Status run_migrations(hal::KvStore& kv,
                      std::string_view ns,
                      std::uint16_t stored,
                      std::uint16_t target,
                      std::span<const MigrationStep> steps) noexcept {
    if (stored > target) {
        return Errc::kInvalidState;
    }
    if (stored == target) {
        return ok();
    }
    for (std::uint16_t version = stored; version < target; ++version) {
        const MigrationStep* step = nullptr;
        for (const MigrationStep& candidate : steps) {
            if (candidate.from == version) {
                step = &candidate;
                break;
            }
        }
        if (step == nullptr || step->apply == nullptr) {
            return Errc::kCorrupt;
        }
        QZ_RETURN_IF_ERROR(step->apply(kv));
        QZ_RETURN_IF_ERROR(
            kv.set_u32(ns, tuning::kVerKey, static_cast<std::uint32_t>(version) + 1));
    }
    return kv.commit();
}

std::span<const MigrationStep> settings_migrations() noexcept {
    return kSettingsSteps;
}

} // namespace qz::settings
