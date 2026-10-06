// QZ_ASSERT failure path (ARCHITECTURE.md section 2): report file:line, then abort().
#include "qz/core/assert.hpp"

#include "log_internal.hpp"
#include "qz/core/log.hpp"
#include "tuning.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace qz {
namespace {

/// "dir/dir/file.cpp" -> "file.cpp" (the message buffer is small; __FILE__ is often absolute).
const char* base_name(const char* path) noexcept {
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

/// Reports through the installed log sink, or on stderr when none is installed (early boot,
/// host tests without a sink), so a failed assertion is never silent.
void report(const char* expr, const char* file, int line) noexcept {
    char message[detail::kAssertLineBytes];
    // (void): truncation is acceptable and the buffer is always terminated.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): bounded formatting into a stack buffer
    (void)std::snprintf(message,
                        sizeof(message),
                        "%s:%d: QZ_ASSERT(%s) failed",
                        base_name(file != nullptr ? file : "?"),
                        line,
                        expr != nullptr ? expr : "?");
    if (const LogSink sink = detail::installed_log_sink(); sink != nullptr) {
        sink(LogLevel::kError, detail::kAssertTag, message);
    } else {
        (void)std::fputs(message, stderr);
        (void)std::fputc('\n', stderr);
        (void)std::fflush(stderr);
    }
}

} // namespace

void assert_fail(const char* expr, const char* file, int line) noexcept {
    // Only the first failing caller reports. A second task asserting at the same time, or a sink
    // that itself asserts, goes straight to abort() instead of recursing or interleaving.
    static std::atomic<std::uint32_t> reporting{0};
    if (reporting.exchange(1U, std::memory_order_acq_rel) == 0U) {
        report(expr, file, line);
    }
    std::abort();
}

} // namespace qz
