// Logging (ARCHITECTURE.md section 19): sink, runtime threshold, truncation, macros.
#include "log_capture.hpp"
#include "qz/core/log.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <sys/mman.h>

namespace qz {
namespace {

using test::capture_sink;
using test::log_records;

/// A readable-looking C string that faults when read (an inaccessible page): passing it as a %s
/// argument proves that a log call never touched its arguments.
class FaultingString {
public:
    FaultingString()
        : page_(mmap(nullptr, kBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {}
    FaultingString(const FaultingString&) = delete;
    FaultingString& operator=(const FaultingString&) = delete;
    FaultingString(FaultingString&&) = delete;
    FaultingString& operator=(FaultingString&&) = delete;
    ~FaultingString() {
        if (page_ != MAP_FAILED) {
            munmap(page_, kBytes);
        }
    }
    [[nodiscard]] bool valid() const { return page_ != MAP_FAILED; }
    [[nodiscard]] const char* c_str() const { return static_cast<const char*>(page_); }

private:
    static constexpr std::size_t kBytes = 4096;
    void* page_;
};

constexpr std::array<LogLevel, 5> kLevels = {
    LogLevel::kError, LogLevel::kWarn, LogLevel::kInfo, LogLevel::kDebug, LogLevel::kVerbose};

// log.hpp documents a 192-byte stack buffer: 191 characters plus the NUL.
constexpr std::size_t kMaxMessageChars = 191;

class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        saved_level_ = log_level();
        log_records().clear();
        set_log_level(LogLevel::kVerbose);
        set_log_sink(&capture_sink);
    }
    void TearDown() override {
        set_log_sink(nullptr);
        set_log_level(saved_level_);
        log_records().clear();
    }

private:
    LogLevel saved_level_ = LogLevel::kWarn;
};

TEST_F(LogTest, ForwardsLevelTagAndFormattedMessage) {
    log_write(LogLevel::kWarn, "net", "x=%d s=%s h=%04x c=%c", 7, "abc", 0xBEEF, 'z');
    ASSERT_EQ(log_records().size(), 1U);
    EXPECT_EQ(log_records()[0].level, LogLevel::kWarn);
    EXPECT_EQ(log_records()[0].tag, "net");
    EXPECT_EQ(log_records()[0].message, "x=7 s=abc h=beef c=z");
}

TEST_F(LogTest, DeliversMessagesInOrder) {
    log_write(LogLevel::kInfo, "t", "first");
    log_write(LogLevel::kInfo, "t", "second");
    log_write(LogLevel::kError, "t", "third");
    ASSERT_EQ(log_records().size(), 3U);
    EXPECT_EQ(log_records()[0].message, "first");
    EXPECT_EQ(log_records()[1].message, "second");
    EXPECT_EQ(log_records()[2].message, "third");
}

TEST_F(LogTest, LevelRoundTrips) {
    for (const LogLevel level : kLevels) {
        set_log_level(level);
        EXPECT_EQ(log_level(), level);
    }
}

TEST_F(LogTest, ThresholdKeepsExactlyTheMessagesAtOrBelowIt) {
    for (const LogLevel threshold : kLevels) {
        set_log_level(threshold);
        for (const LogLevel message_level : kLevels) {
            log_records().clear();
            log_write(message_level, "t", "m");
            const bool expected = static_cast<int>(message_level) <= static_cast<int>(threshold);
            EXPECT_EQ(log_records().size(), expected ? 1U : 0U)
                << "threshold " << static_cast<int>(threshold) << " message "
                << static_cast<int>(message_level);
        }
    }
}

TEST_F(LogTest, FilteredMessagesAreDroppedBeforeFormatting) {
    const FaultingString poison;
    ASSERT_TRUE(poison.valid());
    set_log_level(LogLevel::kError);
    log_write(LogLevel::kDebug, "t", "%s", poison.c_str());
    log_write(LogLevel::kWarn, "t", "%s", poison.c_str());
    EXPECT_TRUE(log_records().empty());
}

TEST_F(LogTest, NoSinkMeansNothingIsFormatted) {
    const FaultingString poison;
    ASSERT_TRUE(poison.valid());
    set_log_sink(nullptr);
    log_write(LogLevel::kError, "t", "%s", poison.c_str());
    EXPECT_TRUE(log_records().empty());
}

TEST_F(LogTest, ReplacingTheSinkRedirectsMessages) {
    static std::size_t second_calls = 0;
    second_calls = 0;
    set_log_sink([](LogLevel, const char*, const char*) noexcept { ++second_calls; });
    log_write(LogLevel::kError, "t", "m");
    EXPECT_EQ(second_calls, 1U);
    EXPECT_TRUE(log_records().empty());
}

TEST_F(LogTest, NullTagIsPassedOnAsEmpty) {
    log_write(LogLevel::kError, nullptr, "m");
    ASSERT_EQ(log_records().size(), 1U);
    EXPECT_EQ(log_records()[0].tag, "");
}

TEST_F(LogTest, EmptyMessageIsDelivered) {
    log_write(LogLevel::kError, "t", "%s", "");
    ASSERT_EQ(log_records().size(), 1U);
    EXPECT_EQ(log_records()[0].message, "");
}

TEST_F(LogTest, MessageThatFillsTheBufferExactlyIsDeliveredWhole) {
    const std::string text(kMaxMessageChars, 'k');
    log_write(LogLevel::kInfo, "t", "%s", text.c_str());
    ASSERT_EQ(log_records().size(), 1U);
    EXPECT_EQ(log_records()[0].message, text);
}

TEST_F(LogTest, LongerMessageIsTruncatedToTheBuffer) {
    const std::string text(kMaxMessageChars + 1U, 'k');
    log_write(LogLevel::kInfo, "t", "%s", text.c_str());
    ASSERT_EQ(log_records().size(), 1U);
    EXPECT_EQ(log_records()[0].message, std::string(kMaxMessageChars, 'k'));
}

TEST_F(LogTest, TruncationKeepsTheFormattedPrefixAndNeverOverflows) {
    // Far longer than the buffer: ASan reports any write past the 192 bytes.
    const std::string text(1000, 'p');
    log_write(LogLevel::kInfo, "t", "<%s>-%d", text.c_str(), 5);
    ASSERT_EQ(log_records().size(), 1U);
    const std::string& message = log_records()[0].message;
    ASSERT_EQ(message.size(), kMaxMessageChars);
    EXPECT_EQ(message, "<" + std::string(kMaxMessageChars - 1U, 'p'));
}

TEST_F(LogTest, MacrosLogAtTheirLevelWithTagAndArguments) {
    QZ_LOGE("e", "error %d", 1);
    QZ_LOGW("w", "warn %d", 2);
    QZ_LOGI("i", "info %d", 3);
    QZ_LOGD("d", "debug %d", 4);
    ASSERT_EQ(log_records().size(), 4U);
    EXPECT_EQ(log_records()[0].level, LogLevel::kError);
    EXPECT_EQ(log_records()[0].tag, "e");
    EXPECT_EQ(log_records()[0].message, "error 1");
    EXPECT_EQ(log_records()[1].level, LogLevel::kWarn);
    EXPECT_EQ(log_records()[1].message, "warn 2");
    EXPECT_EQ(log_records()[2].level, LogLevel::kInfo);
    EXPECT_EQ(log_records()[2].message, "info 3");
    EXPECT_EQ(log_records()[3].level, LogLevel::kDebug);
    EXPECT_EQ(log_records()[3].message, "debug 4");
}

TEST_F(LogTest, MacrosRespectTheRuntimeThreshold) {
    set_log_level(LogLevel::kWarn);
    QZ_LOGE("t", "kept");
    QZ_LOGW("t", "kept");
    QZ_LOGI("t", "dropped");
    QZ_LOGD("t", "dropped");
    EXPECT_EQ(log_records().size(), 2U);
}

// No fixture on purpose: the child process of a "threadsafe" death test starts from scratch, so
// this observes the state before anything called set_log_level().
[[noreturn]] void exit_with_the_verdict_on_the_default_level() {
    std::_Exit(log_level() == LogLevel::kWarn ? 0 : 3);
}

TEST(LogDefaultsDeathTest, ThresholdStartsAtWarn) {
    const std::string saved_style = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exit_with_the_verdict_on_the_default_level(), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, saved_style);
}

} // namespace
} // namespace qz
