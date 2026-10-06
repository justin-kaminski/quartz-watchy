// Test helpers shared by the qz_console tests.
#pragma once

#include "qz/core/log.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qz::console::test {

/// The line in a heap block of exactly its size: ASan reports any read or write past either end.
class ExactBuffer {
public:
    explicit ExactBuffer(std::string_view text) : bytes_(text.begin(), text.end()) {}
    [[nodiscard]] std::span<char> span() { return {bytes_.data(), bytes_.size()}; }
    [[nodiscard]] std::string str() const { return {bytes_.data(), bytes_.size()}; }

private:
    std::vector<char> bytes_;
};

struct LogLine {
    LogLevel level;
    std::string tag;
    std::string message;
};

inline std::vector<LogLine>& captured_logs() {
    static std::vector<LogLine> lines;
    return lines;
}

inline void capture_log(LogLevel level, const char* tag, const char* message) noexcept {
    captured_logs().push_back(LogLine{level, tag, message});
}

/// Fixture base: routes log output into captured_logs() at verbose level; restores logging after.
class LogCaptureTest : public ::testing::Test {
protected:
    void SetUp() override {
        saved_level_ = log_level();
        captured_logs().clear();
        set_log_level(LogLevel::kVerbose);
        set_log_sink(&capture_log);
    }
    void TearDown() override {
        set_log_sink(nullptr);
        set_log_level(saved_level_);
        captured_logs().clear();
    }
    /// True when any captured message contains `needle`.
    [[nodiscard]] static bool logged(std::string_view needle) {
        return std::ranges::any_of(captured_logs(), [needle](const LogLine& line) {
            return line.message.find(needle) != std::string::npos;
        });
    }

private:
    LogLevel saved_level_ = LogLevel::kWarn;
};

} // namespace qz::console::test
