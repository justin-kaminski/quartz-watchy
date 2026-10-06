// conn::SyncSession: step order, hard budgets, teardown on every path, no secrets in logs.
#include "qz/conn/conn.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace qz::conn {
namespace {

using testkit::NetCall;

constexpr std::int64_t kTrueUtcUs = 1'800'000'000LL * 1'000'000LL;
constexpr const char* kPassword = "hunter2-Secret-pw";
constexpr const char* kWeatherJson = R"({
  "current": {"time": "2026-10-06T12:00", "temperature_2m": 18.46, "weather_code": 61},
  "daily": {"time": ["2026-10-06"], "temperature_2m_max": [21.25], "temperature_2m_min": [-3.04]}
})";

struct SessionTest : ::testing::Test {
    SessionTest() { clock.set_true_utc_us(kTrueUtcUs); }

    SessionResult
    run(const Plan& plan, const Budget& budget = Budget{}, time::UnixSeconds now = 0) {
        return session.run(plan, creds, loc, budget, now);
    }
    [[nodiscard]] std::vector<NetCall> order() const { return net.calls(); }

    testkit::VirtualClock clock;
    testkit::FakeNetStack net{clock};
    weather::OpenMeteoProvider provider;
    SyncSession session{net, provider, clock};
    hal::WifiCredentials creds = [] {
        hal::WifiCredentials c;
        (void)c.ssid.assign("HomeNet");
        (void)c.password.assign(kPassword);
        return c;
    }();
    model::Location loc{4'175'000, -8'814'000};
};

std::vector<NetCall> full_order() {
    return {NetCall::kConnect, NetCall::kSntp, NetCall::kHttp, NetCall::kShutdown};
}

TEST_F(SessionTest, FullSessionOrderAndResults) {
    net.script_connect(ok(), 1200);
    net.script_sntp(ok(), 300);
    net.script_http(200, kWeatherJson, 900);
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(order(), full_order());
    EXPECT_TRUE(r.connect);
    EXPECT_TRUE(r.time);
    EXPECT_TRUE(r.weather);
    ASSERT_TRUE(r.sntp_utc_us.has_value());
    EXPECT_EQ(r.sntp_utc_us.value_or(0), kTrueUtcUs + (1200 * 1000LL) + (300 * 1000LL));
    EXPECT_EQ(r.sntp_rtc_us, 1500 * 1000LL);
    ASSERT_TRUE(r.report.has_value());
    const model::WeatherReport rep = r.report.value_or(model::WeatherReport{});
    EXPECT_EQ(rep.temp_dc, 185);
    EXPECT_EQ(rep.fetched_utc, 1'800'000'002); // SNTP time + 0.9 s fetch, floor seconds
    EXPECT_EQ(r.duration_ms, 2400U);
    EXPECT_FALSE(net.radio_on());
    EXPECT_EQ(net.violations(), 0U);
    EXPECT_EQ(net.last_credentials().ssid.view(), "HomeNet");
    EXPECT_EQ(net.last_credentials().password.reveal(), kPassword);
    EXPECT_EQ(net.last_url().rfind("https://", 0), 0U);
    EXPECT_NE(net.last_url().find("41.75000"), std::string::npos);
    // The libc clock was corrected so TLS sees the right date.
    EXPECT_EQ(clock.system_utc_us(), kTrueUtcUs + (2400 * 1000LL));
}

TEST_F(SessionTest, BudgetsPassedAsTimeouts) {
    net.script_http(200, kWeatherJson, 0);
    (void)run(Plan{true, true});
    EXPECT_EQ(net.last_connect_timeout_ms(), 10'000U);
    EXPECT_EQ(net.last_sntp_timeout_ms(), 8'000U);
    EXPECT_EQ(net.last_http_timeout_ms(), 12'000U);
}

TEST_F(SessionTest, TimeOnlySkipsWeather) {
    const SessionResult r = run(Plan{true, false});
    EXPECT_EQ(order(),
              (std::vector<NetCall>{NetCall::kConnect, NetCall::kSntp, NetCall::kShutdown}));
    EXPECT_FALSE(r.report.has_value());
    EXPECT_TRUE(r.sntp_utc_us.has_value());
}

TEST_F(SessionTest, WeatherOnlyUsesCallerUtcAdvancedByElapsed) {
    net.script_connect(ok(), 2000);
    net.script_http(200, kWeatherJson, 1000);
    const SessionResult r = run(Plan{false, true}, Budget{}, 1'800'000'000);
    EXPECT_EQ(order(),
              (std::vector<NetCall>{NetCall::kConnect, NetCall::kHttp, NetCall::kShutdown}));
    EXPECT_FALSE(r.sntp_utc_us.has_value());
    ASSERT_TRUE(r.report.has_value());
    EXPECT_EQ(r.report.value_or(model::WeatherReport{}).fetched_utc, 1'800'000'003);
}

TEST_F(SessionTest, WeatherWithoutKnownUtcFailsWithoutNetworkRequest) {
    const SessionResult r = run(Plan{false, true});
    EXPECT_EQ(test::code_of(r.weather), Errc::kNoTime);
    EXPECT_EQ(net.http_calls(), 0U);
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, EmptyPlanNeverTouchesRadio) {
    const SessionResult r = run(Plan{});
    EXPECT_TRUE(r.connect);
    EXPECT_TRUE(net.calls().empty());
    EXPECT_EQ(net.radio_init_count(), 0U);
}

TEST_F(SessionTest, EmptySsidFailsWithoutRadio) {
    creds.ssid.clear();
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(test::code_of(r.connect), Errc::kNoCredentials);
    EXPECT_EQ(test::code_of(r.time), Errc::kNoCredentials);
    EXPECT_EQ(test::code_of(r.weather), Errc::kNoCredentials);
    EXPECT_TRUE(net.calls().empty());
}

TEST_F(SessionTest, ConnectFailureStillTearsDown) {
    net.script_connect(Errc::kIo, 3000);
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(order(), (std::vector<NetCall>{NetCall::kConnect, NetCall::kShutdown}));
    EXPECT_EQ(test::code_of(r.connect), Errc::kIo);
    EXPECT_EQ(test::code_of(r.time), Errc::kIo);
    EXPECT_EQ(test::code_of(r.weather), Errc::kIo);
    EXPECT_FALSE(r.report.has_value());
    EXPECT_FALSE(net.radio_on());
}

TEST_F(SessionTest, ConnectOnlyFailsPlannedJobs) {
    net.script_connect(Errc::kIo, 0);
    const SessionResult r = run(Plan{true, false});
    EXPECT_FALSE(r.time);
    EXPECT_TRUE(r.weather); // never planned, never failed
}

TEST_F(SessionTest, ConnectTimeoutAtBudget) {
    net.script_connect(ok(), 25'000); // slower than the 10 s associate budget
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(test::code_of(r.connect), Errc::kTimeout);
    EXPECT_EQ(r.duration_ms, 10'000U);
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, SntpFailureStillRunsWeatherThenTearsDown) {
    net.script_sntp(Errc::kTimeout, 8000);
    net.script_http(200, kWeatherJson, 100);
    const SessionResult r = run(Plan{true, true}, Budget{}, 1'800'000'000);
    EXPECT_EQ(order(), full_order());
    EXPECT_EQ(test::code_of(r.time), Errc::kTimeout);
    EXPECT_TRUE(r.weather);
    EXPECT_FALSE(r.sntp_utc_us.has_value());
    EXPECT_TRUE(r.report.has_value());
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, SntpFailureWithUnknownUtcSkipsWeatherRequest) {
    net.script_sntp(Errc::kTimeout, 10);
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(test::code_of(r.time), Errc::kTimeout);
    EXPECT_EQ(test::code_of(r.weather), Errc::kNoTime);
    EXPECT_EQ(net.http_calls(), 0U);
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, ImplausibleSntpAnswerRejected) {
    net.script_sntp_utc(86'400LL * 1'000'000, 10);
    const SessionResult r = run(Plan{true, false});
    EXPECT_EQ(test::code_of(r.time), Errc::kCorrupt);
    EXPECT_FALSE(r.sntp_utc_us.has_value());
    EXPECT_EQ(clock.system_utc_us(), clock.rtc_us()); // libc clock never set
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, WeatherTransportFailure) {
    net.script_http_error(Error{Errc::kIo, 5}, 200);
    const SessionResult r = run(Plan{true, true});
    EXPECT_TRUE(r.time);
    EXPECT_EQ(test::code_of(r.weather), Errc::kIo);
    EXPECT_FALSE(r.report.has_value());
    EXPECT_EQ(net.shutdown_calls(), 1U);
    EXPECT_FALSE(net.radio_on());
}

TEST_F(SessionTest, WeatherHttpErrorStatus) {
    net.script_http(503, "busy", 10);
    const SessionResult r = run(Plan{true, true});
    ASSERT_FALSE(r.weather);
    EXPECT_EQ(r.weather.error().code, Errc::kIo);
    EXPECT_EQ(r.weather.error().detail, 503);
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, WeatherOversizeBodyIsCorrupt) {
    net.script_http(200, std::string(weather::kMaxBodyBytes + 1, 'x'), 10);
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(test::code_of(r.weather), Errc::kCorrupt);
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, WeatherGarbageBodyIsCorrupt) {
    net.script_http(200, "not json", 10);
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(test::code_of(r.weather), Errc::kCorrupt);
    EXPECT_FALSE(r.report.has_value());
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, WeatherTimeoutAtItsBudget) {
    net.script_http(200, kWeatherJson, 20'000);
    const SessionResult r = run(Plan{true, true});
    EXPECT_EQ(test::code_of(r.weather), Errc::kTimeout);
    EXPECT_EQ(net.shutdown_calls(), 1U);
}

TEST_F(SessionTest, TotalBudgetCapsStepTimeoutsAndExpires) {
    Budget b;
    b.total_ms = 15'000;
    net.script_connect(ok(), 9'000);
    net.script_sntp(ok(), 20'000); // would blow the session budget
    net.script_http(200, kWeatherJson, 0);
    const SessionResult r = run(Plan{true, true}, b, 1'800'000'000);
    EXPECT_EQ(net.last_sntp_timeout_ms(), 6'000U); // 15 s total minus 9 s spent
    EXPECT_EQ(test::code_of(r.time), Errc::kTimeout);
    EXPECT_EQ(test::code_of(r.weather), Errc::kTimeout); // budget exhausted: no request made
    EXPECT_EQ(net.http_calls(), 0U);
    EXPECT_EQ(r.duration_ms, 15'000U);
    EXPECT_EQ(order(),
              (std::vector<NetCall>{NetCall::kConnect, NetCall::kSntp, NetCall::kShutdown}));
}

TEST_F(SessionTest, ZeroTotalBudgetNeverStartsRadio) {
    Budget b;
    b.total_ms = 0;
    const SessionResult r = run(Plan{true, true}, b);
    EXPECT_EQ(test::code_of(r.connect), Errc::kTimeout);
    EXPECT_EQ(test::code_of(r.time), Errc::kTimeout);
    EXPECT_TRUE(net.calls().empty());
}

/// Runs a full-plan session whose connect/SNTP/HTTP steps fail according to `mask` and checks
/// the radio ends down with exactly one shutdown as the last call.
void expect_clean_teardown(const weather::Provider& provider,
                           const hal::WifiCredentials& creds,
                           const model::Location& loc,
                           int mask) {
    testkit::VirtualClock c;
    c.set_true_utc_us(kTrueUtcUs);
    testkit::FakeNetStack n(c);
    SyncSession s(n, provider, c);
    if ((mask & 1) != 0) {
        n.script_connect(Errc::kIo, 5);
    }
    if ((mask & 2) != 0) {
        n.script_sntp(Errc::kIo, 5);
    }
    if ((mask & 4) != 0) {
        n.script_http_error(Errc::kIo, 5);
    } else {
        n.script_http(200, kWeatherJson, 5);
    }
    (void)s.run(Plan{true, true}, creds, loc, Budget{}, 1'800'000'000);
    EXPECT_EQ(n.shutdown_calls(), 1U) << "mask " << mask;
    EXPECT_FALSE(n.radio_on()) << "mask " << mask;
    EXPECT_EQ(n.calls().back(), NetCall::kShutdown) << "mask " << mask;
    EXPECT_EQ(n.violations(), 0U) << "mask " << mask;
}

TEST_F(SessionTest, TeardownOnEveryScriptedFailurePoint) {
    // connect / sntp / http each failing alone, in combination, and not at all.
    for (int mask = 0; mask < 8; ++mask) {
        expect_clean_teardown(provider, creds, loc, mask);
    }
}

TEST_F(SessionTest, PasswordNeverLogged) {
    const test::LogCapture capture;
    net.script_connect(Errc::kIo, 1);
    (void)run(Plan{true, true});
    net.script_connect(ok(), 1);
    net.script_sntp(Errc::kIo, 1);
    net.script_http(503, "x", 1);
    (void)run(Plan{true, true}, Budget{}, 1'800'000'000);
    net.script_http(200, "garbage", 1);
    (void)run(Plan{true, true}, Budget{}, 1'800'000'000);
    EXPECT_FALSE(test::captured_logs().empty());         // failures are logged ...
    EXPECT_FALSE(test::LogCapture::contains(kPassword)); // ... without the secret
    EXPECT_FALSE(test::LogCapture::contains("hunter2"));
}

} // namespace
} // namespace qz::conn
