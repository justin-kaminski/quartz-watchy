// Tunables of qz_core (one constexpr table per component, ARCHITECTURE.md section 2).
// Private to the component: not installed, not part of the contract.
#pragma once

#include "qz/core/log.hpp"

#include <cstddef>

namespace qz::detail {

/// Stack buffer for one formatted log line, NUL included (log.hpp documents 192 bytes).
inline constexpr std::size_t kLogLineBytes = 192;

/// Runtime log threshold until set_log_level() is called. Warn matches the battery builds
/// (CONFIG_LOG_DEFAULT_LEVEL_WARN, ARCHITECTURE.md section 19): a main() that forgets to configure
/// logging must not make every wake format and print info messages. The console `log level` command
/// and host tests raise it explicitly.
inline constexpr LogLevel kDefaultLogLevel = LogLevel::kWarn;

/// Stack buffer for the QZ_ASSERT report ("file:line: QZ_ASSERT(expr) failed"), NUL included.
/// file:line comes first so that a long expression is what gets truncated.
inline constexpr std::size_t kAssertLineBytes = 192;

/// Tag under which the assertion report reaches the log sink.
inline constexpr const char* kAssertTag = "assert";

} // namespace qz::detail
