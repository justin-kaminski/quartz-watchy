// Boot breadcrumbs (see idf_platform.hpp): RTC_NOINIT ring of {reset reason, last phase, detail}.
#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "esp_system.h"
#include "qz/platform/idf_platform.hpp"

#include <array>
#include <cstdint>

namespace qz::platform {

struct BootTrailEntry {
    std::uint8_t reset_reason; ///< esp_reset_reason_t of this boot
    std::uint8_t last_phase;   ///< Phase reached last before the next boot
    std::uint16_t reserved;
    std::int32_t detail;  ///< phase-specific (esp_err_t for kSleepRejected)
    std::uint32_t rtc_ms; ///< RTC time at the last update
};

struct BootTrail {
    std::uint32_t magic;
    std::uint32_t boots;
    std::uint32_t head;
    std::array<BootTrailEntry, 16> entries;
    std::int32_t last_reject; ///< kSleepRejected detail of the most recent refusal (kept apart)
    std::uint32_t rejects;    ///< refusals since the trail was created
};

constexpr std::uint32_t kTrailMagic = 0x51425443; // "QBTC"

// Global (not in an anonymous namespace) so GDB can find it by name.
RTC_NOINIT_ATTR BootTrail g_boot_trail;

void breadcrumb(Phase phase, std::int32_t detail) noexcept {
    BootTrail& t = g_boot_trail;
    const auto now_ms = static_cast<std::uint32_t>(esp_rtc_get_time_us() / 1000);
    if (phase == Phase::kBoot) {
        if (t.magic != kTrailMagic || t.head >= t.entries.size()) {
            t = BootTrail{};
            t.magic = kTrailMagic;
        } else {
            t.head = (t.head + 1) % t.entries.size();
        }
        ++t.boots;
        t.entries[t.head] = BootTrailEntry{static_cast<std::uint8_t>(esp_reset_reason()),
                                           static_cast<std::uint8_t>(phase),
                                           0,
                                           detail,
                                           now_ms};
        return;
    }
    if (t.magic != kTrailMagic || t.head >= t.entries.size()) {
        return;
    }
    if (phase == Phase::kSleepRejected) {
        t.last_reject = detail;
        ++t.rejects;
    }
    BootTrailEntry& e = t.entries[t.head];
    e.last_phase = static_cast<std::uint8_t>(phase);
    e.detail = detail;
    e.rtc_ms = now_ms;
}

} // namespace qz::platform
