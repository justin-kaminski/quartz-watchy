// Shared helpers for the qz_conn host tests.
#pragma once

#include "qz/core/log.hpp"
#include "qz/core/result.hpp"

#include <optional>
#include <string>
#include <vector>

namespace qz::conn::test {

/// Error code of a failed Result/Status, nullopt on success.
template<class R>
std::optional<Errc> code_of(const R& r) {
    if (r) {
        return std::nullopt;
    }
    return r.error().code;
}

/// Log sink that keeps every formatted message (tests assert secrets never show up).
inline std::vector<std::string>& captured_logs() {
    static std::vector<std::string> logs;
    return logs;
}

inline void capture_sink(LogLevel /*level*/, const char* tag, const char* message) noexcept {
    captured_logs().push_back(std::string(tag) + ": " + message);
}

/// RAII: capture all log levels for the test's lifetime.
class LogCapture {
public:
    LogCapture() {
        captured_logs().clear();
        previous_level_ = log_level();
        set_log_level(LogLevel::kVerbose);
        set_log_sink(&capture_sink);
    }
    ~LogCapture() {
        set_log_sink(nullptr);
        set_log_level(previous_level_);
    }
    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    [[nodiscard]] static bool contains(const std::string& needle) {
        for (const std::string& line : captured_logs()) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    LogLevel previous_level_;
};

} // namespace qz::conn::test
