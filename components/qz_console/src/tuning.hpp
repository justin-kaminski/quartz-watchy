// Tunables of qz_console (one constexpr table per component, ARCHITECTURE.md section 2).
// Private to the component: not installed, not part of the contract.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qz::console::detail {

/// Containers the JSON writer can have open at once (the top-level object included). The writer
/// keeps one bit per level in a uint32_t, so the depth must stay below 32. Deepest document in the
/// v1 catalog is `selftest run` (object -> results[] -> object), 3 levels.
inline constexpr std::uint8_t kMaxJsonDepth = 16;
static_assert(kMaxJsonDepth < 32U);

/// Longest request id: 1-8 characters of [A-Za-z0-9] (ARCHITECTURE.md section 16). Equals the
/// capacity of Request::id (checked in protocol.cpp).
inline constexpr std::size_t kMaxIdChars = 8;

/// Bytes of a non-sensitive request's arguments copied into its debug log line. The log line
/// itself is cut at 192 bytes by qz/core/log.hpp; the rest of it is the id and the command name.
inline constexpr std::size_t kLoggedArgsBytes = 120;

/// Scratch for composing an error `msg` (a usage string is its longest ingredient).
inline constexpr std::size_t kErrorMsgBytes = 112;

/// Log tag of everything the dispatcher says.
inline constexpr const char* kLogTag = "console";

} // namespace qz::console::detail
