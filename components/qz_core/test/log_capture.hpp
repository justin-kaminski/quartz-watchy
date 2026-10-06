// Test helper: a log sink that records every message it receives.
#pragma once

#include "qz/core/log.hpp"

#include <string>
#include <vector>

namespace qz::test {

struct LogRecord {
    LogLevel level = LogLevel::kError;
    std::string tag;
    std::string message;
};

/// Records written by capture_sink(), oldest first. Tests clear it in SetUp().
inline std::vector<LogRecord>& log_records() {
    static std::vector<LogRecord> records;
    return records;
}

inline void capture_sink(LogLevel level, const char* tag, const char* message) noexcept {
    log_records().push_back(LogRecord{level, tag, message});
}

} // namespace qz::test
