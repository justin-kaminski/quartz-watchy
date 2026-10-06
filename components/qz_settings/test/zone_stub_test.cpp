// Stand-in for the generated built-in zone table (WP-03). The definitions are weak so the real
// table, once it exists, never collides with them at link time; these tests only rely on the
// names below, which any real table also contains.
#include "qz/time/tz.hpp"

#include <array>
#include <span>
#include <string_view>

namespace qz::time {
namespace {

constexpr std::array<TzEntry, 4> kStubZones{{
    {"UTC", "UTC", "UTC0", 0},
    {"America/Chicago", "Chicago", "CST6CDT,M3.2.0,M11.1.0", -6 * 3600},
    {"Europe/Berlin", "Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", 3600},
    {"Asia/Kolkata", "Kolkata", "IST-5:30", 19800},
}};

} // namespace

__attribute__((weak)) std::span<const TzEntry> builtin_zones() noexcept {
    return kStubZones;
}

__attribute__((weak)) const TzEntry* find_zone(std::string_view iana_name) noexcept {
    for (const TzEntry& z : kStubZones) {
        if (z.name == iana_name) {
            return &z;
        }
    }
    return nullptr;
}

} // namespace qz::time
