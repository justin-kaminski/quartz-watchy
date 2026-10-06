// Logging for pure code (ARCHITECTURE.md section 19): installable sink, runtime threshold,
// printf formatting into a fixed stack buffer. No heap, no locks.
#include "qz/core/log.hpp"

#include "log_internal.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace qz {
namespace {

// Sink and threshold are written during init (and by the console `log level` command) while
// other tasks may be logging, so both are atomics. Relaxed ordering is enough for the level;
// the sink is published with release/acquire so a task that sees it also sees what it points to.
struct LogState {
    std::atomic<LogSink> sink{nullptr};
    std::atomic<std::uint8_t> level{static_cast<std::uint8_t>(detail::kDefaultLogLevel)};
};

// Function-local static with constant initialisation: no guard variable, no allocation, and
// usable before main() runs.
LogState& state() noexcept {
    static LogState instance;
    return instance;
}

constexpr char kFormatError[] = "<log format error>";
static_assert(sizeof(kFormatError) <= detail::kLogLineBytes);

} // namespace

namespace detail {

LogSink installed_log_sink() noexcept {
    return state().sink.load(std::memory_order_acquire);
}

} // namespace detail

void set_log_sink(LogSink sink) noexcept {
    state().sink.store(sink, std::memory_order_release);
}

void set_log_level(LogLevel level) noexcept {
    state().level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
}

LogLevel log_level() noexcept {
    return static_cast<LogLevel>(state().level.load(std::memory_order_relaxed));
}

// The printf-style variadic signature is the contract in log.hpp (and gets -Wformat checking
// there).

// NOLINTNEXTLINE(cert-dcl50-cpp, modernize-avoid-variadic-functions)
void log_write(LogLevel level, const char* tag, const char* fmt, ...) noexcept {
    // Cheapest checks first: a filtered message (or one nobody listens to) costs two loads and
    // never touches its arguments.
    if (static_cast<std::uint8_t>(level) > state().level.load(std::memory_order_relaxed)) {
        return;
    }
    const LogSink sink = state().sink.load(std::memory_order_acquire);
    if (sink == nullptr) {
        return;
    }

    char message[detail::kLogLineBytes];
    std::va_list args; // NOLINT(cppcoreguidelines-pro-type-vararg): forwards to vsnprintf
    va_start(args, fmt);
    const int written = std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written < 0) {
        // Encoding error: the buffer content is unspecified, so replace it wholesale.
        std::ranges::copy(kFormatError, std::begin(message)); // fits: static_assert above
    }
    // vsnprintf truncates and always NUL-terminates a non-empty buffer.
    sink(level, tag != nullptr ? tag : "", message);
}

} // namespace qz
