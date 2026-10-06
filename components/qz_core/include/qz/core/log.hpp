// Logging for pure code (ARCHITECTURE.md section 19). Never log secrets.
#pragma once

#include <cstdint>

namespace qz {

enum class LogLevel : std::uint8_t { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3, kVerbose = 4 };

/// Receives fully formatted messages (no trailing newline). Must be callable from any task.
using LogSink = void (*)(LogLevel level, const char* tag, const char* message) noexcept;

/// Installs the sink (platform: esp_log_write; host: capture/stderr). nullptr = discard.
void set_log_sink(LogSink sink) noexcept;
/// Runtime threshold (messages above it are dropped before formatting).
void set_log_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel log_level() noexcept;

/// printf-style; formats into a 192-byte stack buffer (truncated). Thread-safe if the sink is.
[[gnu::format(printf, 3, 4)]] void
log_write(LogLevel level, const char* tag, const char* fmt, ...) noexcept;

} // namespace qz

#ifndef QZ_LOG_MAX_LEVEL
#define QZ_LOG_MAX_LEVEL 3 // compile-time ceiling: 0 error .. 4 verbose
#endif

#define QZ_LOGE(tag, ...) ::qz::log_write(::qz::LogLevel::kError, tag, __VA_ARGS__)
#define QZ_LOGW(tag, ...) ::qz::log_write(::qz::LogLevel::kWarn, tag, __VA_ARGS__)
#if QZ_LOG_MAX_LEVEL >= 2
#define QZ_LOGI(tag, ...) ::qz::log_write(::qz::LogLevel::kInfo, tag, __VA_ARGS__)
#else
#define QZ_LOGI(tag, ...) static_cast<void>(0)
#endif
#if QZ_LOG_MAX_LEVEL >= 3
#define QZ_LOGD(tag, ...) ::qz::log_write(::qz::LogLevel::kDebug, tag, __VA_ARGS__)
#else
#define QZ_LOGD(tag, ...) static_cast<void>(0)
#endif
