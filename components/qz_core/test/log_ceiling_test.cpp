// The compile-time ceiling QZ_LOG_MAX_LEVEL (log.hpp): macros above it vanish and do not evaluate
// their arguments. Own translation unit so the ceiling can differ from log_test.cpp's default.
#define QZ_LOG_MAX_LEVEL 2 // info and above are compiled in; debug is compiled out

#include "log_capture.hpp"
#include "qz/core/log.hpp"

#include <gtest/gtest.h>

namespace qz {
namespace {

class LogCeilingTest : public ::testing::Test {
protected:
    void SetUp() override {
        saved_level_ = log_level();
        test::log_records().clear();
        set_log_level(LogLevel::kVerbose);
        set_log_sink(&test::capture_sink);
    }
    void TearDown() override {
        set_log_sink(nullptr);
        set_log_level(saved_level_);
        test::log_records().clear();
    }

private:
    LogLevel saved_level_ = LogLevel::kWarn;
};

TEST_F(LogCeilingTest, MacrosAtOrBelowTheCeilingStillLog) {
    QZ_LOGE("t", "e");
    QZ_LOGW("t", "w");
    QZ_LOGI("t", "i");
    ASSERT_EQ(test::log_records().size(), 3U);
    EXPECT_EQ(test::log_records()[2].level, LogLevel::kInfo);
}

TEST_F(LogCeilingTest, MacrosAboveTheCeilingAreCompiledOut) {
    // Not const: the macro would increment it if it were compiled in.
    int evaluations = 0; // NOLINT(misc-const-correctness)
    QZ_LOGD("t", "%d", ++evaluations);
    EXPECT_EQ(evaluations, 0) << "arguments of a compiled-out macro must not be evaluated";
    EXPECT_TRUE(test::log_records().empty());
}

} // namespace
} // namespace qz
