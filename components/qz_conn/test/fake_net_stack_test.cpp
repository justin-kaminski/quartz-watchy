// testkit::FakeNetStack: scripting, virtual-time latency, call log, caller-bug detection.
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace qz::testkit {
namespace {

using conn::test::code_of;

hal::WifiCredentials creds() {
    hal::WifiCredentials c;
    (void)c.ssid.assign("net");
    (void)c.password.assign("pw");
    return c;
}

TEST(FakeNetStack, DefaultsSucceedWithoutLatency) {
    VirtualClock clock;
    clock.set_true_utc_us(1'800'000'000LL * 1'000'000);
    FakeNetStack net(clock);
    EXPECT_FALSE(net.radio_on());
    EXPECT_EQ(net.radio_init_count(), 0U);
    EXPECT_TRUE(net.connect(creds(), 1000));
    EXPECT_TRUE(net.radio_on());
    std::int64_t rtc = -1;
    const auto utc = net.sntp_sync(1000, &rtc);
    ASSERT_TRUE(utc.has_value());
    EXPECT_EQ(*utc, 1'800'000'000LL * 1'000'000);
    EXPECT_EQ(rtc, 0);
    std::array<char, 16> body{};
    const auto http = net.https_get("https://x/y", body, 1000);
    ASSERT_TRUE(http.has_value());
    EXPECT_EQ(http->status, 200);
    EXPECT_EQ(http->body_len, 0U);
    EXPECT_FALSE(http->truncated);
    net.shutdown();
    EXPECT_FALSE(net.radio_on());
    EXPECT_EQ(clock.elapsed_us(), 0);
}

TEST(FakeNetStack, CountersCallLogAndRecordedArguments) {
    VirtualClock clock;
    FakeNetStack net(clock);
    ASSERT_TRUE(net.connect(creds(), 111));
    std::int64_t rtc = 0;
    ASSERT_TRUE(net.sntp_sync(222, &rtc).has_value());
    std::array<char, 8> body{};
    ASSERT_TRUE(net.https_get("https://h/p?q=1", body, 333).has_value());
    net.shutdown();
    net.shutdown(); // idempotent
    EXPECT_EQ(net.connect_calls(), 1U);
    EXPECT_EQ(net.sntp_calls(), 1U);
    EXPECT_EQ(net.http_calls(), 1U);
    EXPECT_EQ(net.shutdown_calls(), 2U);
    EXPECT_EQ(net.radio_init_count(), 1U);
    EXPECT_EQ(net.violations(), 0U);
    EXPECT_EQ(net.last_connect_timeout_ms(), 111U);
    EXPECT_EQ(net.last_sntp_timeout_ms(), 222U);
    EXPECT_EQ(net.last_http_timeout_ms(), 333U);
    EXPECT_EQ(net.last_url(), "https://h/p?q=1");
    EXPECT_EQ(net.last_credentials().ssid.view(), "net");
    EXPECT_EQ(net.last_credentials().password.reveal(), "pw");
    EXPECT_EQ(net.calls(),
              (std::vector<NetCall>{NetCall::kConnect,
                                    NetCall::kSntp,
                                    NetCall::kHttp,
                                    NetCall::kShutdown,
                                    NetCall::kShutdown}));
}

TEST(FakeNetStack, ScriptsAreConsumedInOrderThenLastRepeats) {
    VirtualClock clock;
    FakeNetStack net(clock);
    net.script_connect(Errc::kIo, 0);
    net.script_connect(ok(), 0);
    EXPECT_EQ(code_of(net.connect(creds(), 10)), Errc::kIo);
    net.shutdown();
    EXPECT_TRUE(net.connect(creds(), 10));
    net.shutdown();
    EXPECT_TRUE(net.connect(creds(), 10)); // sticky
    EXPECT_EQ(net.radio_init_count(), 3U); // counted even when association failed
}

TEST(FakeNetStack, LatencyAdvancesVirtualTimeAndTimeoutCapsIt) {
    VirtualClock clock;
    FakeNetStack net(clock);
    net.script_connect(ok(), 1500);
    EXPECT_TRUE(net.connect(creds(), 5000));
    EXPECT_EQ(clock.elapsed_us(), 1'500'000);
    net.script_sntp(ok(), 9000);
    std::int64_t rtc = 0;
    EXPECT_EQ(code_of(net.sntp_sync(2000, &rtc)), Errc::kTimeout);
    EXPECT_EQ(clock.elapsed_us(), 3'500'000);
    net.script_http(200, "x", 4000);
    std::array<char, 4> body{};
    EXPECT_EQ(code_of(net.https_get("u", body, 4000)), std::nullopt); // exactly at the timeout: ok
    EXPECT_EQ(clock.elapsed_us(), 7'500'000);
}

TEST(FakeNetStack, SntpScriptVariants) {
    VirtualClock clock;
    clock.set_true_utc_us(10);
    FakeNetStack net(clock);
    ASSERT_TRUE(net.connect(creds(), 10));
    net.script_sntp(Errc::kNotFound, 5);
    net.script_sntp_utc(777, 5);
    net.script_sntp(ok(), 0);
    std::int64_t rtc = 0;
    EXPECT_EQ(code_of(net.sntp_sync(100, &rtc)), Errc::kNotFound);
    const auto fixed = net.sntp_sync(100, &rtc);
    ASSERT_TRUE(fixed.has_value());
    EXPECT_EQ(fixed.value_or(0), 777);
    EXPECT_EQ(rtc, 10'000); // answer arrived after 2 x 5 ms
    const auto truth = net.sntp_sync(100, nullptr);
    ASSERT_TRUE(truth.has_value());
    EXPECT_EQ(truth.value_or(0), 10 + 10'000);
}

TEST(FakeNetStack, HttpBodyTruncationAndErrors) {
    VirtualClock clock;
    FakeNetStack net(clock);
    ASSERT_TRUE(net.connect(creds(), 10));
    net.script_http(200, "abcdefgh", 0);
    std::array<char, 4> small{};
    auto r = net.https_get("u", small, 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->body_len, 4U);
    EXPECT_TRUE(r->truncated);
    EXPECT_EQ(std::string(small.data(), 4), "abcd");
    net.script_http(404, "no", 0);
    std::array<char, 16> big{};
    r = net.https_get("u", big, 10);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, 404);
    EXPECT_EQ(r->body_len, 2U);
    EXPECT_FALSE(r->truncated);
    net.script_http_error(Error{Errc::kIo, 9}, 0);
    r = net.https_get("u", big, 10);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), (Error{Errc::kIo, 9}));
}

TEST(FakeNetStack, CallerBugsAreCountedAndRefused) {
    VirtualClock clock;
    FakeNetStack net(clock);
    std::int64_t rtc = 0;
    std::array<char, 4> body{};
    EXPECT_EQ(code_of(net.sntp_sync(10, &rtc)), Errc::kInvalidState); // radio off
    EXPECT_EQ(code_of(net.https_get("u", body, 10)), Errc::kInvalidState);
    EXPECT_EQ(net.violations(), 2U);
    ASSERT_TRUE(net.connect(creds(), 10));
    EXPECT_EQ(code_of(net.connect(creds(), 10)), Errc::kInvalidState); // no shutdown in between
    EXPECT_EQ(net.violations(), 3U);
}

} // namespace
} // namespace qz::testkit
