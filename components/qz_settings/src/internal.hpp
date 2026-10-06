// Private helpers shared by the settings translation units.
#pragma once

#include "qz/settings/settings.hpp"

#include <cstdint>
#include <string_view>

namespace qz::settings::detail {

/// Keys that map to one integer (everything except the zone name).
[[nodiscard]] bool is_numeric_key(Key key) noexcept;

/// Integer persisted for `key`: bool 0/1, enum token index, uint value, degrees in 1e-5.
/// Precondition: is_numeric_key(key).
[[nodiscard]] std::int64_t to_number(const Settings& s, Key key) noexcept;

/// Range/choice check of a persisted integer (no state change).
[[nodiscard]] Status check_number(const KeyInfo& ki, std::int64_t value) noexcept;

/// Validates `value` and stores it. On error `s` is unchanged. Degrees keys mark the location set.
[[nodiscard]] Status apply_number(Settings& s, Key key, std::int64_t value) noexcept;

/// Sets the zone from the built-in list (name + POSIX fallback). kBadArgs if unknown.
[[nodiscard]] Status apply_zone_name(Settings& s, std::string_view name) noexcept;

} // namespace qz::settings::detail
