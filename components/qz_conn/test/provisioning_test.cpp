// conn::Provisioning: AP credentials, page + token, urlencoded form parser, expiry, no secret logs.
#include "qz/conn/conn.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace qz::conn {
namespace {

constexpr std::int64_t kStart = 5'000'000;
constexpr std::int64_t kFiveMin = 300LL * 1'000'000;

struct RecordingSink final : FormSink {
    Status apply(const ProvisioningForm& form) override {
        ++calls;
        ssid = std::string(form.ssid.view());
        password = std::string(form.password.reveal());
        tz = std::string(form.tz_name.view());
        lat = std::string(form.lat.view());
        lon = std::string(form.lon.view());
        units = std::string(form.units.view());
        mode = std::string(form.mode.view());
        return result;
    }
    int calls = 0;
    Status result;
    std::string ssid, password, tz, lat, lon, units, mode;
};

/// System whose RNG is scripted by the test.
struct StubSystem final : hal::System {
    [[nodiscard]] hal::ResetReason reset_reason() const override { return {}; }
    [[nodiscard]] hal::WakeSources wake_sources() const override { return {}; }
    [[nodiscard]] std::int64_t boot_rtc_us() const override { return 0; }
    [[nodiscard]] hal::SlowClockInfo slow_clock() const override { return {}; }
    [[nodiscard]] hal::FirmwareInfo firmware() const override { return {}; }
    [[nodiscard]] std::uint64_t chip_id() const override { return 0xDEADBEEF0042ULL; }
    std::uint32_t random_u32() override { return value; }
    [[nodiscard]] hal::HeapInfo heap() const override { return {}; }
    void restart() override {}
    std::uint32_t value = 0xFFFFFFFFU;
};

std::string token_in_page(std::string_view page) {
    const std::string marker = "name=token value=\"";
    const std::size_t at = page.find(marker);
    if (at == std::string_view::npos) {
        return {};
    }
    const std::size_t from = at + marker.size();
    return std::string(page.substr(from, page.find('"', from) - from));
}

std::string good_body(const std::string& token) {
    return "token=" + token +
           "&ssid=My+Home%21&pass=p%40ss%26w%2Bord&tz=America%2FChicago&lat=41.75&lon=-88.14"
           "&units=f&mode=time%2Bweather";
}

struct ProvTest : ::testing::Test {
    testkit::VirtualClock clock;
    testkit::FakeSleepSystem system{clock};
    RecordingSink sink;
    Provisioning prov{system, sink};
};

// ------------------------------------------------------------ credentials
TEST_F(ProvTest, SsidFromChipId) {
    prov.begin(kStart);
    EXPECT_EQ(prov.ssid(), "Quartz-B2C3"); // fake chip id 0x240AC4A1B2C3
}

TEST_F(ProvTest, PasswordAlphabetExcludesAmbiguousCharacters) {
    for (const char c : std::string_view("0O1lIo")) {
        EXPECT_EQ(kPasswordAlphabet.find(c), std::string_view::npos) << c;
    }
    const std::set<char> unique(kPasswordAlphabet.begin(), kPasswordAlphabet.end());
    EXPECT_EQ(unique.size(), kPasswordAlphabet.size()); // no duplicates
    EXPECT_EQ(kPasswordAlphabet.size(), 56U);
}

TEST_F(ProvTest, GeneratedPasswordsUseOnlyAlphabetAndCoverIt) {
    std::set<char> seen;
    std::set<std::string> passwords;
    for (int i = 0; i < 400; ++i) {
        prov.begin(kStart);
        const std::string_view pw = prov.password().reveal();
        ASSERT_EQ(pw.size(), kPasswordLength);
        for (const char c : pw) {
            ASSERT_NE(kPasswordAlphabet.find(c), std::string_view::npos) << "char " << c;
            seen.insert(c);
        }
        passwords.insert(std::string(pw));
    }
    EXPECT_EQ(seen.size(), kPasswordAlphabet.size()); // every symbol reachable
    EXPECT_EQ(passwords.size(), 400U);                // fresh each session
}

TEST(ProvRng, StuckGeneratorStillTerminates) {
    StubSystem stuck;
    RecordingSink sink;
    Provisioning p(stuck, sink);
    p.begin(0); // 0xFF bytes are always rejected by the unbiased draw; must fall back, not spin
    EXPECT_EQ(p.password().reveal().size(), kPasswordLength);
    EXPECT_EQ(p.ssid(), "Quartz-0042");
}

// ------------------------------------------------------------ page / token / expiry
TEST_F(ProvTest, PageCarriesTokenAndNotTheApPassword) {
    prov.begin(kStart);
    const std::string page(prov.page());
    ASSERT_FALSE(page.empty());
    EXPECT_EQ(token_in_page(page).size(), 10U);
    EXPECT_EQ(page.find(std::string(prov.password().reveal())), std::string::npos);
    EXPECT_NE(page.find("name=ssid"), std::string::npos);
    EXPECT_NE(page.find("action=/save"), std::string::npos);
}

TEST_F(ProvTest, EachSessionHasItsOwnToken) {
    prov.begin(kStart);
    const std::string first = token_in_page(prov.page());
    prov.begin(kStart);
    EXPECT_NE(first, token_in_page(prov.page()));
}

TEST_F(ProvTest, ExpiresAfterFiveMinutes) {
    EXPECT_TRUE(prov.expired(0)); // not begun
    prov.begin(kStart);
    EXPECT_FALSE(prov.expired(kStart));
    EXPECT_FALSE(prov.expired(kStart + kFiveMin - 1));
    EXPECT_TRUE(prov.expired(kStart + kFiveMin));
}

TEST_F(ProvTest, TickLatchesExpiryAndWipesSecrets) {
    prov.begin(kStart);
    const std::string token = token_in_page(prov.page());
    EXPECT_FALSE(prov.tick(kStart + 1000));
    EXPECT_TRUE(prov.tick(kStart + kFiveMin));
    EXPECT_TRUE(prov.password().empty());
    EXPECT_TRUE(prov.page().empty());
    EXPECT_EQ(test::code_of(prov.submit(good_body(token))), Errc::kInvalidState);
    EXPECT_EQ(sink.calls, 0);
    EXPECT_FALSE(prov.completed());
}

TEST_F(ProvTest, SubmitBeforeBeginRefused) {
    EXPECT_EQ(test::code_of(prov.submit(good_body("x"))), Errc::kInvalidState);
}

TEST_F(ProvTest, EndWipesAndRefuses) {
    prov.begin(kStart);
    const std::string token = token_in_page(prov.page());
    prov.end();
    EXPECT_TRUE(prov.password().empty());
    EXPECT_EQ(test::code_of(prov.submit(good_body(token))), Errc::kInvalidState);
}

// ------------------------------------------------------------ submit
TEST_F(ProvTest, SubmitAppliesFormOnceAndConsumesToken) {
    prov.begin(kStart);
    const std::string token = token_in_page(prov.page());
    const auto r = prov.submit(good_body(token));
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(r->find("Saved"), std::string_view::npos);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_EQ(sink.ssid, "My Home!");
    EXPECT_EQ(sink.password, "p@ss&w+ord");
    EXPECT_EQ(sink.tz, "America/Chicago");
    EXPECT_EQ(sink.lat, "41.75");
    EXPECT_EQ(sink.lon, "-88.14");
    EXPECT_EQ(sink.units, "f");
    EXPECT_EQ(sink.mode, "time+weather");
    EXPECT_TRUE(prov.completed());
    // one-time: replay is refused and never reaches the sink
    EXPECT_EQ(test::code_of(prov.submit(good_body(token))), Errc::kInvalidState);
    EXPECT_EQ(sink.calls, 1);
}

TEST_F(ProvTest, WrongTokenNeverReachesSink) {
    prov.begin(kStart);
    EXPECT_EQ(test::code_of(prov.submit(good_body("WRONGTOKEN"))), Errc::kInvalidState);
    EXPECT_EQ(sink.calls, 0);
    EXPECT_FALSE(prov.completed());
}

TEST_F(ProvTest, SinkRejectionAllowsRetryWithSameToken) {
    prov.begin(kStart);
    const std::string token = token_in_page(prov.page());
    sink.result = Errc::kBadArgs;
    EXPECT_EQ(test::code_of(prov.submit(good_body(token))), Errc::kBadArgs);
    EXPECT_FALSE(prov.completed());
    sink.result = ok();
    EXPECT_TRUE(prov.submit(good_body(token)).has_value());
    EXPECT_TRUE(prov.completed());
    EXPECT_EQ(sink.calls, 2);
}

// ------------------------------------------------------------ parse_form
Result<ProvisioningForm> parse(const std::string& body, std::string_view token = "TOK") {
    return Provisioning::parse_form(body, token);
}

std::string base_body() {
    return "token=TOK&ssid=net&pass=pw&tz=UTC&lat=1&lon=2&units=c&mode=off";
}

TEST(ParseForm, DecodesPlusAndPercent) {
    const auto f =
        parse("token=TOK&ssid=a+b%20c%2b&pass=%41%62%7e&tz=UTC&lat=&lon=&units=c&mode=time");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->ssid.view(), "a b c+");
    EXPECT_EQ(f->password.reveal(), "Ab~");
    EXPECT_EQ(f->lat.view(), "");
    EXPECT_EQ(f->mode.view(), "time");
}

TEST(ParseForm, AllowsEmptyPasswordAndLocationButNotOtherFields) {
    EXPECT_TRUE(parse("token=TOK&ssid=n&pass=&tz=UTC&lat=&lon=&units=c&mode=off").has_value());
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=&pass=&tz=UTC&lat=&lon=&units=c&mode=off")),
              Errc::kBadArgs);
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=n&pass=&tz=&lat=&lon=&units=c&mode=off")),
              Errc::kBadArgs);
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=n&pass=&tz=UTC&lat=&lon=&units=&mode=off")),
              Errc::kBadArgs);
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=n&pass=&tz=UTC&lat=&lon=&units=c&mode=")),
              Errc::kBadArgs);
}

TEST(ParseForm, MissingFieldsRejected) {
    for (const char* drop : {"ssid=net&", "pass=pw&", "tz=UTC&", "lat=1&", "lon=2&", "units=c&"}) {
        std::string body = base_body();
        body.erase(body.find(drop), std::string_view(drop).size());
        EXPECT_EQ(test::code_of(parse(body)), Errc::kBadArgs) << "without " << drop;
    }
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=net&pass=pw&tz=UTC&lat=1&lon=2&units=c")),
              Errc::kBadArgs);                                // mode missing (last field)
    EXPECT_EQ(test::code_of(parse("")), Errc::kInvalidState); // no token at all
}

TEST(ParseForm, TokenMismatchMissingOrEmpty) {
    EXPECT_EQ(test::code_of(parse(base_body(), "TOX")), Errc::kInvalidState);
    EXPECT_EQ(test::code_of(parse(base_body(), "TOKEN")), Errc::kInvalidState); // longer expected
    EXPECT_EQ(test::code_of(parse(base_body(), "TO")), Errc::kInvalidState);    // shorter expected
    EXPECT_EQ(test::code_of(parse(base_body(), "")), Errc::kInvalidState); // never accept empty
    EXPECT_EQ(test::code_of(parse("ssid=net&pass=pw&tz=UTC&lat=1&lon=2&units=c&mode=off")),
              Errc::kInvalidState);
    EXPECT_EQ(test::code_of(parse("token=&ssid=n&pass=&tz=UTC&lat=&lon=&units=c&mode=off", "")),
              Errc::kInvalidState);
    // authentication is judged before field completeness
    EXPECT_EQ(test::code_of(parse("token=NOPE&ssid=n", "TOK")), Errc::kInvalidState);
    // a token too long for the field is a mismatch, not a crash
    EXPECT_EQ(test::code_of(parse(base_body() + "&x=1", "TOK")), std::nullopt);
    EXPECT_EQ(test::code_of(parse(
                  "token=ABCDEFGHIJKLMNOPQRSTUVWXYZ&ssid=n&pass=&tz=U&lat=&lon=&units=c&mode=off",
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ")),
              Errc::kInvalidState);
}

TEST(ParseForm, OversizeBodyRejected) {
    std::string body = base_body() + "&pad=";
    body.append(kMaxFormBytes - body.size(), 'a');
    ASSERT_EQ(body.size(), kMaxFormBytes);
    EXPECT_TRUE(parse(body).has_value()); // exactly at the limit
    body.push_back('a');
    EXPECT_EQ(test::code_of(parse(body)), Errc::kNoSpace);
}

TEST(ParseForm, FieldLongerThanCapacityRejected) {
    EXPECT_TRUE(parse("token=TOK&ssid=" + std::string(32, 's') + "&pass=" + std::string(63, 'p') +
                      "&tz=UTC&lat=&lon=&units=c&mode=off")
                    .has_value());
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=" + std::string(33, 's') +
                                  "&pass=&tz=UTC&lat=&lon=&units=c&mode=off")),
              Errc::kBadArgs);
    EXPECT_EQ(test::code_of(parse("token=TOK&ssid=n&pass=" + std::string(65, 'p') +
                                  "&tz=UTC&lat=&lon=&units=c&mode=off")),
              Errc::kBadArgs);
    // decoded size, not encoded size, counts: 200 escapes decode past the scratch buffer
    std::string escapes;
    for (int i = 0; i < 100; ++i) {
        escapes += "%41";
    }
    EXPECT_EQ(test::code_of(
                  parse("token=TOK&ssid=n&pass=" + escapes + "&tz=UTC&lat=&lon=&units=c&mode=off")),
              Errc::kBadArgs);
}

TEST(ParseForm, MalformedEscapesRejected) {
    for (const char* bad : {"%", "%4", "%zz", "%4g", "a%", "%%41", "%00", "%0a", "%7f"}) {
        const std::string body =
            std::string("token=TOK&ssid=n&pass=") + bad + "&tz=UTC&lat=&lon=&units=c&mode=off";
        EXPECT_EQ(test::code_of(parse(body)), Errc::kBadArgs) << bad;
    }
}

TEST(ParseForm, DuplicateKeysRejectedUnknownKeysIgnored) {
    EXPECT_EQ(test::code_of(parse(base_body() + "&ssid=other")), Errc::kBadArgs);
    EXPECT_EQ(test::code_of(parse(base_body() + "&token=TOK")), Errc::kBadArgs);
    const auto f = parse("&&" + base_body() + "&extra=1&&");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->ssid.view(), "net");
}

// ------------------------------------------------------------ secrets in logs
TEST_F(ProvTest, SecretsNeverReachLogs) {
    const test::LogCapture capture;
    prov.begin(kStart);
    const std::string ap_password(prov.password().reveal());
    const std::string token = token_in_page(prov.page());
    (void)prov.submit(good_body("WRONGTOKEN"));
    (void)prov.submit("token=" + token + "&ssid=n&pass=Zq9%3Fsecret");
    sink.result = Errc::kBadArgs;
    (void)prov.submit(good_body(token));
    sink.result = ok();
    (void)prov.submit(good_body(token));
    (void)prov.tick(kStart + (kFiveMin * 2));
    EXPECT_FALSE(test::captured_logs().empty());
    for (const std::string& secret : {ap_password,
                                      token,
                                      std::string("p@ss&w+ord"),
                                      std::string("p%40ss%26w%2Bord"),
                                      std::string("Zq9?secret"),
                                      std::string("Zq9%3Fsecret")}) {
        EXPECT_FALSE(test::LogCapture::contains(secret)) << secret;
    }
}

} // namespace
} // namespace qz::conn
