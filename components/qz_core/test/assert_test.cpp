// QZ_ASSERT / assert_fail (ARCHITECTURE.md section 2): reports file:line and aborts.
#include "qz/core/assert.hpp"
#include "qz/core/log.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace qz {
namespace {

// A sink that writes to stderr so that a death test can match on what the sink received.
void stderr_sink(LogLevel level, const char* tag, const char* message) noexcept {
    (void)std::fprintf(stderr,
                       "SINK level=%d tag=%s len=%zu msg=%s\n",
                       static_cast<int>(level),
                       tag,
                       std::strlen(message),
                       message);
}

// A sink that violates an assertion itself: must not recurse or hang.
void asserting_sink(LogLevel /*level*/, const char* /*tag*/, const char* /*message*/) noexcept {
    (void)std::fprintf(stderr, "SINK entered\n");
    QZ_ASSERT(false);
}

// The statements below run in the child process of a death test, one failing assertion each.
void assert_one_plus_one_is_three() {
    QZ_ASSERT(1 + 1 == 3);
}

void assert_false_with_sink() {
    set_log_sink(&stderr_sink);
    QZ_ASSERT(false);
}

void assert_false_at_the_strictest_threshold() {
    set_log_sink(&stderr_sink);
    set_log_level(LogLevel::kError);
    QZ_ASSERT(false);
}

void assert_false_with_an_asserting_sink() {
    set_log_sink(&asserting_sink);
    QZ_ASSERT(false);
}

void fail_with_a_very_long_expression() {
    const std::string long_expression(500, 'x');
    set_log_sink(&stderr_sink);
    assert_fail(long_expression.c_str(), "file.cpp", 77);
}

constexpr int checked_square_root_floor(int x) {
    QZ_ASSERT(x >= 0);
    int r = 0;
    while ((r + 1) * (r + 1) <= x) {
        ++r;
    }
    return r;
}

TEST(Assert, PassingConditionIsANoOpThatEvaluatesOnce) {
    int evaluations = 0;
    QZ_ASSERT(++evaluations == 1);
    EXPECT_EQ(evaluations, 1);
}

TEST(Assert, IsUsableInConstantExpressionsWhenTheConditionHolds) {
    static_assert(checked_square_root_floor(17) == 4);
    EXPECT_EQ(checked_square_root_floor(17), 4);
}

TEST(AssertDeathTest, FailureAbortsAndNamesFileLineAndExpressionOnStderr) {
    // No sink installed: the report goes to stderr. SIGABRT proves abort(), not exit().
    EXPECT_EXIT(assert_one_plus_one_is_three(),
                ::testing::KilledBySignal(SIGABRT),
                "assert_test\\.cpp:[0-9]+: QZ_ASSERT\\(1 \\+ 1 == 3\\) failed");
}

TEST(AssertDeathTest, FailureGoesThroughTheLogSinkWhenOneIsInstalled) {
    EXPECT_EXIT(assert_false_with_sink(),
                ::testing::KilledBySignal(SIGABRT),
                "SINK level=0 tag=assert len=[0-9]+ msg=assert_test\\.cpp:[0-9]+: "
                "QZ_ASSERT\\(false\\) failed");
}

TEST(AssertDeathTest, ReportIgnoresTheRuntimeLogThreshold) {
    // Errors are always at or below any threshold, so even the strictest one lets it through.
    EXPECT_EXIT(assert_false_at_the_strictest_threshold(),
                ::testing::KilledBySignal(SIGABRT),
                "SINK level=0 tag=assert");
}

TEST(AssertDeathTest, SinkThatAssertsItselfStillEndsInAbort) {
    EXPECT_EXIT(
        assert_false_with_an_asserting_sink(), ::testing::KilledBySignal(SIGABRT), "SINK entered");
}

TEST(AssertFailDeathTest, ReportsOnlyTheBaseNameOfTheFile) {
    EXPECT_EXIT(assert_fail("x > 0", "/a/b/c/some_file.cpp", 123),
                ::testing::KilledBySignal(SIGABRT),
                "some_file\\.cpp:123: QZ_ASSERT\\(x > 0\\) failed");
}

TEST(AssertFailDeathTest, AcceptsNullExpressionAndFile) {
    EXPECT_EXIT(assert_fail(nullptr, nullptr, 0),
                ::testing::KilledBySignal(SIGABRT),
                "\\?:0: QZ_ASSERT\\(\\?\\) failed");
}

TEST(AssertFailDeathTest, TruncatesALongReportButKeepsFileAndLine) {
    // 192-byte buffer including the NUL: the 191 visible characters start with file:line.
    EXPECT_EXIT(fail_with_a_very_long_expression(),
                ::testing::KilledBySignal(SIGABRT),
                "len=191 msg=file\\.cpp:77: QZ_ASSERT\\(xxx");
}

} // namespace
} // namespace qz
