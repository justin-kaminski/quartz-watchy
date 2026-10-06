// FakeConsolePort: scripted request lines in, captured lines out, lifecycle bookkeeping.
#include "qz/hal/system.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace qz::testkit {
namespace {

/// Receives one line into `buffer` and returns it as text ("" for a timeout).
std::string receive(FakeConsolePort& port, std::span<char> buffer, std::uint32_t timeout_ms = 10) {
    const Result<std::size_t> result = port.receive_line(buffer, timeout_ms);
    EXPECT_TRUE(result);
    if (!result) {
        return "<error>";
    }
    return {buffer.data(), *result};
}

// --- lifecycle ----------------------------------------------------------------------------------

TEST(FakeConsolePort, StartsDown) {
    const FakeConsolePort port;
    EXPECT_FALSE(port.running());
    EXPECT_EQ(port.start_count(), 0U);
    EXPECT_EQ(port.stop_count(), 0U);
    EXPECT_TRUE(port.sent().empty());
    EXPECT_EQ(port.pending_requests(), 0U);
}

TEST(FakeConsolePort, StartAndStopAreTrackedAndRepeatable) {
    FakeConsolePort port;
    hal::ConsolePort& console = port;
    EXPECT_TRUE(console.start());
    EXPECT_TRUE(port.running());
    EXPECT_EQ(port.start_count(), 1U);

    console.stop();
    EXPECT_FALSE(port.running());
    EXPECT_EQ(port.stop_count(), 1U);

    EXPECT_TRUE(console.start()) << "a new tether session may start again";
    EXPECT_TRUE(port.running());
    EXPECT_EQ(port.start_count(), 2U);
}

TEST(FakeConsolePort, StartingTwiceWithoutAStopIsAnInvalidStateButStillCounted) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    const Status again = port.start();
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, Errc::kInvalidState);
    EXPECT_TRUE(port.running());
    EXPECT_EQ(port.start_count(), 2U);
}

TEST(FakeConsolePort, StopWhenDownIsHarmlessButCounted) {
    FakeConsolePort port;
    port.stop();
    EXPECT_FALSE(port.running());
    EXPECT_EQ(port.stop_count(), 1U);
}

TEST(FakeConsolePort, InjectedStartFailureIsOneShot) {
    FakeConsolePort port;
    port.fail_next_start(Error{Errc::kIo, 0x0102});
    const Status failed = port.start();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error(), (Error{Errc::kIo, 0x0102}));
    EXPECT_FALSE(port.running()) << "a failed start leaves the console down";
    EXPECT_EQ(port.start_count(), 1U);

    EXPECT_TRUE(port.start());
    EXPECT_TRUE(port.running());
    EXPECT_EQ(port.start_count(), 2U);
}

// --- receiving requests -------------------------------------------------------------------------

TEST(FakeConsolePort, DeliversRequestsInOrderWithoutATerminator) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    port.push_request("#1 status");
    port.push_request("time get");
    EXPECT_EQ(port.pending_requests(), 2U);

    std::array<char, 64> buffer{};
    buffer.fill('X');
    const Result<std::size_t> first = port.receive_line(buffer, 100);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, 9U);
    EXPECT_EQ(std::string_view(buffer.data(), *first), "#1 status");
    EXPECT_EQ(buffer[*first], 'X') << "no NUL is appended: the device does not promise one either";
    EXPECT_EQ(port.pending_requests(), 1U);

    EXPECT_EQ(receive(port, buffer), "time get");
    EXPECT_EQ(port.pending_requests(), 0U);
}

TEST(FakeConsolePort, EmptyQueueIsATimeoutReportedAsZero) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    std::array<char, 16> buffer{};
    const Result<std::size_t> result = port.receive_line(buffer, 250);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 0U);
}

TEST(FakeConsolePort, TimeoutConsumesVirtualTimeWhenBoundToAClock) {
    VirtualClock clock;
    FakeConsolePort port(clock);
    ASSERT_TRUE(port.start());
    std::array<char, 16> buffer{};
    for (int poll = 0; poll < 5; ++poll) {
        EXPECT_EQ(receive(port, buffer, 1000), "");
    }
    EXPECT_EQ(clock.rtc_us(), 5'000'000) << "five 1 s polls of a silent console";
}

TEST(FakeConsolePort, AnAvailableRequestIsDeliveredWithoutTimePassing) {
    VirtualClock clock;
    FakeConsolePort port(clock);
    ASSERT_TRUE(port.start());
    port.push_request("help");
    std::array<char, 16> buffer{};
    EXPECT_EQ(receive(port, buffer, 1000), "help");
    EXPECT_EQ(clock.rtc_us(), 0);
}

TEST(FakeConsolePort, TooLongLineIsDiscardedAndReportedAsNoSpace) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    port.push_request(std::string(40, 'a'));
    port.push_request("next");

    std::array<char, 16> buffer{};
    const Result<std::size_t> too_long = port.receive_line(buffer, 10);
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().code, Errc::kNoSpace);
    EXPECT_EQ(port.pending_requests(), 1U) << "the long line is gone";
    EXPECT_EQ(receive(port, buffer), "next");
}

TEST(FakeConsolePort, LineThatExactlyFillsTheBufferIsAccepted) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    port.push_request(std::string(16, 'b'));
    std::array<char, 16> buffer{};
    EXPECT_EQ(receive(port, buffer), std::string(16, 'b'));

    port.push_request(std::string(17, 'c'));
    EXPECT_FALSE(port.receive_line(buffer, 10));
}

TEST(FakeConsolePort, EmptyRequestLineIsConsumedAndLooksLikeATimeout) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    port.push_request("");
    port.push_request("after");
    std::array<char, 16> buffer{};
    const Result<std::size_t> empty = port.receive_line(buffer, 10);
    ASSERT_TRUE(empty);
    EXPECT_EQ(*empty, 0U);
    EXPECT_EQ(receive(port, buffer), "after");
}

TEST(FakeConsolePort, ReceivingWhileDownIsAPolicyBug) {
    FakeConsolePort port;
    port.push_request("status");
    std::array<char, 16> buffer{};

    const Result<std::size_t> before_start = port.receive_line(buffer, 10);
    ASSERT_FALSE(before_start);
    EXPECT_EQ(before_start.error().code, Errc::kInvalidState);
    EXPECT_EQ(port.pending_requests(), 1U) << "nothing is consumed while down";

    ASSERT_TRUE(port.start());
    EXPECT_EQ(receive(port, buffer), "status");
    port.stop();
    const Result<std::size_t> after_stop = port.receive_line(buffer, 10);
    ASSERT_FALSE(after_stop);
    EXPECT_EQ(after_stop.error().code, Errc::kInvalidState);
}

TEST(FakeConsolePort, QueuedRequestsSurviveAStopStartCycle) {
    FakeConsolePort port;
    ASSERT_TRUE(port.start());
    port.push_request("queued");
    port.stop();
    ASSERT_TRUE(port.start());
    std::array<char, 16> buffer{};
    EXPECT_EQ(receive(port, buffer), "queued");
}

// --- sending lines ------------------------------------------------------------------------------

TEST(FakeConsolePort, CapturesSentLinesInOrder) {
    FakeConsolePort port;
    hal::ConsolePort& console = port;
    ASSERT_TRUE(console.start());
    console.send_line("@QZ1 - OK {}");
    console.send_line("");
    console.send_line(R"(@QZ1 ! EVT {"evt":"ready"})");
    ASSERT_EQ(port.sent().size(), 3U);
    EXPECT_EQ(port.sent()[0], "@QZ1 - OK {}");
    EXPECT_EQ(port.sent()[1], "");
    EXPECT_EQ(port.sent()[2], R"(@QZ1 ! EVT {"evt":"ready"})");
}

TEST(FakeConsolePort, SentLinesAreCopiedAndRecordedEvenWhileDown) {
    FakeConsolePort port;
    {
        std::string transient = "temporary text";
        port.send_line(transient);
        transient = "overwritten";
    }
    ASSERT_EQ(port.sent().size(), 1U);
    EXPECT_EQ(port.sent()[0], "temporary text");
    EXPECT_FALSE(port.running()) << "sending does not start the port";
}

TEST(FakeConsolePort, ClockCannotBeATemporary) {
    static_assert(std::is_default_constructible_v<FakeConsolePort>);
    static_assert(std::is_constructible_v<FakeConsolePort, VirtualClock&>);
    static_assert(!std::is_constructible_v<FakeConsolePort, VirtualClock>, "would dangle");
    SUCCEED();
}

} // namespace
} // namespace qz::testkit
