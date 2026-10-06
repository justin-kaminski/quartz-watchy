// Console protocol v1 (ARCHITECTURE.md section 16): request tokenizer and line framing.
#include "../src/protocol_internal.hpp"
#include "json_check.hpp"
#include "qz/console/protocol.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qz::console {
namespace {

using detail::ParseFailure;
using test::ExactBuffer;
using namespace std::string_literals;

// ---- tokenizer and quoting table -------------------------------------------------------------

struct AcceptedCase {
    std::string line;
    std::string id;
    std::vector<std::string> tokens;
};

// Backslashes and quotes are spelled out in C++: "a\\\"b" is the four characters  a \ " b.
// NOLINTBEGIN(modernize-raw-string-literal): the table shows the lines as C++ escapes on purpose
const std::vector<AcceptedCase>& accepted_cases() {
    static const std::vector<AcceptedCase> kCases = {
        // bare words and whitespace
        {"status"s, ""s, {"status"s}},
        {"  status  "s, ""s, {"status"s}},
        {"time set 2026-10-05T12:00:00Z"s, ""s, {"time"s, "set"s, "2026-10-05T12:00:00Z"s}},
        {"a   b \t c\r"s, ""s, {"a"s, "b"s, "c"s}},
        {"tz set America/New_York"s, ""s, {"tz"s, "set"s, "America/New_York"s}},
        {"echo a#b #c"s, ""s, {"echo"s, "a#b"s, "#c"s}},
        // request ids
        {"#a1 status"s, "a1"s, {"status"s}},
        {"#ABCDEFGH status"s, "ABCDEFGH"s, {"status"s}},
        {"#7   steps get"s, "7"s, {"steps"s, "get"s}},
        {"  #x9 \t status  "s, "x9"s, {"status"s}},
        {"#a1\tstatus"s, "a1"s, {"status"s}},
        {"#Zz09 status"s, "Zz09"s, {"status"s}},
        {"help #x"s, ""s, {"help"s, "#x"s}}, // only the first token can be an id
        // quoting
        {"wifi set \"My Net\" hunter2"s, ""s, {"wifi"s, "set"s, "My Net"s, "hunter2"s}},
        {"wifi set net \"\""s, ""s, {"wifi"s, "set"s, "net"s, ""s}},
        {"\"status\""s, ""s, {"status"s}},
        {"echo \"a\" \"b\""s, ""s, {"echo"s, "a"s, "b"s}},
        {"echo \"  padded  \""s, ""s, {"echo"s, "  padded  "s}},
        {"echo \"a\\\"b\""s, ""s, {"echo"s, "a\"b"s}},
        {"echo \"a\\\\b\""s, ""s, {"echo"s, "a\\b"s}},
        {"echo \"\\\\\""s, ""s, {"echo"s, "\\"s}},
        {"echo \"\\\"\""s, ""s, {"echo"s, "\""s}},
        {"echo \"\\\\\\\"\""s, ""s, {"echo"s, "\\\""s}},
        {"echo \"x\"\ty"s, ""s, {"echo"s, "x"s, "y"s}},        // whitespace ends a quoted token
        {"echo \"tab\there\""s, ""s, {"echo"s, "tab\there"s}}, // raw tab inside quotes is literal
        {"echo C:\\temp"s, ""s, {"echo"s, "C:\\temp"s}}, // a backslash outside quotes is literal
        {"echo \"a\0b\""s, ""s, {"echo"s, "a\0b"s}},
        // UTF-8 passes through untouched
        {"tz set \"Z\xC3\xBCrich\""s, ""s, {"tz"s, "set"s, "Z\xC3\xBCrich"s}},
        {"echo caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"s,
         ""s,
         {"echo"s, "caf\xC3\xA9"s, "\xE2\x82\xAC"s, "\xF0\x9F\x98\x80"s}},
    };
    return kCases;
}
// NOLINTEND(modernize-raw-string-literal)

struct RejectedCase {
    std::string line;
    ParseFailure reason;
};

// NOLINTBEGIN(modernize-raw-string-literal): the table shows the lines as C++ escapes on purpose
const std::vector<RejectedCase>& rejected_cases() {
    static const std::vector<RejectedCase> kCases = {
        {""s, ParseFailure::kEmpty},
        {"   \t \r"s, ParseFailure::kEmpty},
        {"#a1"s, ParseFailure::kNoCommand},
        {"#a1   "s, ParseFailure::kNoCommand},
        {"#"s, ParseFailure::kBadId},
        {"# status"s, ParseFailure::kBadId},
        {"#123456789 status"s, ParseFailure::kBadId},
        {"#a-b status"s, ParseFailure::kBadId},
        {"#a_ status"s, ParseFailure::kBadId},
        {"#\xC3\xA9 status"s, ParseFailure::kBadId},
        {"#a\"b status"s, ParseFailure::kBadId},
        {"echo \"open"s, ParseFailure::kUnterminatedQuote},
        {"echo \""s, ParseFailure::kUnterminatedQuote},
        {"echo \"abc\\"s, ParseFailure::kUnterminatedQuote},   // the backslash swallows the end
        {"echo \"abc\\\""s, ParseFailure::kUnterminatedQuote}, // the closing quote is escaped
        {"echo \"a\\nb\""s, ParseFailure::kBadEscape},
        {"echo \"\\x\""s, ParseFailure::kBadEscape},
        {"echo \"\\ \""s, ParseFailure::kBadEscape},
        {"echo \"a\"b"s, ParseFailure::kStrayQuote},
        {"echo \"a\"\"b\""s, ParseFailure::kStrayQuote},
        {"echo \"\"x"s, ParseFailure::kStrayQuote},
        {"echo a\"b"s, ParseFailure::kStrayQuote},
        {"echo ab\"cd\""s, ParseFailure::kStrayQuote},
        {"echo a\""s, ParseFailure::kStrayQuote},
    };
    return kCases;
}
// NOLINTEND(modernize-raw-string-literal)

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(ParseRequest, AcceptedLinesTokenizeAsTabulated) {
    for (const AcceptedCase& c : accepted_cases()) {
        SCOPED_TRACE(c.line);
        ExactBuffer buffer(c.line);
        const std::span<char> line = buffer.span();
        const Result<Request> parsed = parse_request(line);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->id.view(), c.id);
        ASSERT_EQ(parsed->tokens.size(), c.tokens.size());
        // Tokens are views into the caller's line, in order, and never overlap.
        const char* previous_end = line.data();
        for (std::size_t i = 0; i < c.tokens.size(); ++i) {
            const std::string_view token = parsed->tokens[i];
            EXPECT_EQ(token, c.tokens[i]);
            EXPECT_TRUE(token.data() >= previous_end);
            EXPECT_TRUE(token.data() + token.size() <= line.data() + line.size());
            previous_end = token.data() + token.size();
        }
    }
}

TEST(ParseRequest, RejectedLinesAreBadArgsAndNameTheirReason) {
    for (const RejectedCase& c : rejected_cases()) {
        SCOPED_TRACE(c.line);
        ExactBuffer buffer(c.line);
        const Result<Request> parsed = parse_request(buffer.span());
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().code, Errc::kBadArgs);
        EXPECT_EQ(parsed.error().detail, static_cast<std::uint16_t>(c.reason));
    }
}

TEST(ParseRequest, OneRequestMayCarryTwoCommandWordsAndTwelveArguments) {
    constexpr std::size_t kMostTokens = kMaxArgs + 2;
    std::string line = "cmd";
    for (std::size_t i = 1; i < kMostTokens; ++i) {
        line += " a" + std::to_string(i);
    }
    ExactBuffer fits(line);
    const Result<Request> ok = parse_request(fits.span());
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok->tokens.size(), kMostTokens);

    ExactBuffer too_many(line + " one-more");
    const Result<Request> bad = parse_request(too_many.span());
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, Errc::kBadArgs);
    EXPECT_EQ(bad.error().detail, static_cast<std::uint16_t>(ParseFailure::kTooManyTokens));
}

TEST(ParseRequest, LineLimitIs256Bytes) {
    const std::string at_limit = "echo " + std::string(kMaxRequestBytes - 5, 'x');
    ASSERT_EQ(at_limit.size(), kMaxRequestBytes);
    ExactBuffer fits(at_limit);
    EXPECT_TRUE(parse_request(fits.span()).has_value());

    for (const std::size_t length : {kMaxRequestBytes + 1, std::size_t{300}, std::size_t{5000}}) {
        SCOPED_TRACE(length);
        ExactBuffer too_long("echo " + std::string(length - 5, 'x'));
        const Result<Request> parsed = parse_request(too_long.span());
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().detail, static_cast<std::uint16_t>(ParseFailure::kTooLong));
    }
}

TEST(ParseRequest, ParseLineKeepsAValidIdWhenALaterTokenIsMalformed) {
    ExactBuffer buffer("#q9 echo \"oops"s);
    Request request;
    EXPECT_EQ(detail::parse_line(buffer.span(), request), ParseFailure::kUnterminatedQuote);
    EXPECT_EQ(request.id.view(), "q9");

    ExactBuffer bad_id("#nope! echo"s);
    EXPECT_EQ(detail::parse_line(bad_id.span(), request), ParseFailure::kBadId);
    EXPECT_TRUE(request.id.empty());
}

TEST(ParseRequest, EveryFailureHasAReadableDescription) {
    for (std::uint8_t value = 0; value <= static_cast<std::uint8_t>(ParseFailure::kTooManyTokens);
         ++value) {
        const std::string_view text = detail::describe(static_cast<ParseFailure>(value));
        EXPECT_FALSE(text.empty()) << static_cast<int>(value);
    }
}

TEST(ValidId, OneToEightLettersOrDigits) {
    for (const std::string_view id : {"a", "Z", "0", "a1B2c3D4", "12345678"}) {
        EXPECT_TRUE(detail::is_valid_id(id)) << id;
    }
    for (const std::string_view id :
         {"", "123456789", "a b", "a-b", "a_b", "a\n", "a\"", "\xC3\xA9", "-", "#a"}) {
        EXPECT_FALSE(detail::is_valid_id(id)) << id;
    }
}

// ---- constants of the specification ----------------------------------------------------------

TEST(Protocol, ConstantsMatchTheSpecification) {
    EXPECT_EQ(kPrefix, "@QZ1 ");
    EXPECT_EQ(kProtocolVersion, 1);
    EXPECT_EQ(kMaxRequestBytes, 256U);
    EXPECT_EQ(kMaxResponseBytes, 16U * 1024U);
    EXPECT_EQ(kMaxArgs, 12U);
}

// ---- framing ---------------------------------------------------------------------------------

/// Runs `format` into a heap block of exactly `capacity` bytes (ASan flags any overrun).
template<class Format>
std::string framed(std::size_t capacity, Format format) {
    std::vector<char> out(capacity);
    const std::size_t length = format(std::span<char>(out.data(), out.size()));
    EXPECT_LE(length, capacity);
    return {out.data(), length};
}

std::string ok_line(std::string_view id, std::string_view json, std::size_t capacity = 512) {
    return framed(capacity, [&](std::span<char> out) { return format_ok(out, id, json); });
}

std::string
err_line(std::string_view id, Errc code, std::string_view msg, std::size_t capacity = 512) {
    return framed(capacity,
                  [&](std::span<char> out) { return format_err(out, id, Error{code}, msg); });
}

std::string event_line(std::string_view json, std::size_t capacity = 512) {
    return framed(capacity, [&](std::span<char> out) { return format_event(out, json); });
}

TEST(Framing, OkLineCarriesTheIdOrADash) {
    EXPECT_EQ(ok_line("a1", R"j({"x":1})j"), R"j(@QZ1 a1 OK {"x":1})j");
    EXPECT_EQ(ok_line("", "{}"), "@QZ1 - OK {}");
    EXPECT_EQ(ok_line("ABCDEFGH", "{}"), "@QZ1 ABCDEFGH OK {}");
}

TEST(Framing, AnIdThatWouldBreakTheFramingIsShownAsADash) {
    for (const std::string_view id : {"bad id", "toolongid1", "a\nb", "\xC3\xA9", "a\"b", "-"}) {
        EXPECT_EQ(ok_line(id, "{}"), "@QZ1 - OK {}") << id;
        EXPECT_EQ(err_line(id, Errc::kBusy, "m"), R"j(@QZ1 - ERR busy {"msg":"m"})j") << id;
    }
}

TEST(Framing, ErrLineCarriesTokenAndMessageObject) {
    EXPECT_EQ(err_line("a1", Errc::kBadArgs, "nope"), R"j(@QZ1 a1 ERR bad_args {"msg":"nope"})j");
    EXPECT_EQ(err_line("", Errc::kNoSpace, ""), R"j(@QZ1 - ERR no_space {"msg":""})j");
}

TEST(Framing, EveryErrorCodeUsesItsDocumentedToken) {
    // The list of ARCHITECTURE.md section 16, in Errc declaration order.
    constexpr std::array<std::string_view, 14> kTokens = {"bad_args",
                                                          "unknown_cmd",
                                                          "unsupported",
                                                          "invalid_state",
                                                          "busy",
                                                          "io",
                                                          "timeout",
                                                          "not_found",
                                                          "no_time",
                                                          "no_creds",
                                                          "battery_low",
                                                          "corrupt",
                                                          "no_space",
                                                          "internal"};
    for (std::size_t i = 0; i < kTokens.size(); ++i) {
        const auto code = static_cast<Errc>(i);
        const std::string expected = "@QZ1 x ERR " + std::string(kTokens[i]) + R"j( {"msg":"m"})j";
        EXPECT_EQ(err_line("x", code, "m"), expected);
    }
}

TEST(Framing, ErrMessageIsJsonEscapedAndStaysOneLine) {
    const std::string line = err_line("x", Errc::kBadArgs, "say \"hi\"\n\\ \x01\r\t\xC3\xA9");
    EXPECT_EQ(line,
              "@QZ1 x ERR bad_args {\"msg\":\"say \\\"hi\\\"\\n\\\\ \\u0001\\r\\t\xC3\xA9\"}");
    EXPECT_EQ(line.find_first_of("\r\n"), std::string::npos);
    const std::size_t json_at = line.find('{');
    EXPECT_TRUE(test::parse_json(line.substr(json_at)).valid);
}

TEST(Framing, EventLineHasNoIdAndTheEvtVerb) {
    EXPECT_EQ(event_line(R"j({"evt":"detach"})j"), R"j(@QZ1 ! EVT {"evt":"detach"})j");

    std::array<char, 160> storage{};
    JsonWriter json(storage);
    json.begin_object()
        .field("evt", "ready")
        .field("proto", kProtocolVersion)
        .field("fw", "1.0.0")
        .field("git", "abc1234")
        .field("reset", "deepsleep")
        .end_object();
    EXPECT_EQ(
        event_line(json.view()),
        R"j(@QZ1 ! EVT {"evt":"ready","proto":1,"fw":"1.0.0","git":"abc1234","reset":"deepsleep"})j");
}

TEST(Framing, EveryFrameIsOneLineThatStartsWithThePrefix) {
    // Clients ignore every line that does not start with the prefix (those are logs), so each
    // protocol line must, and must not contain a line break that would start another line.
    const std::vector<std::string> lines = {
        ok_line("a", R"j({"k":"v"})j"),
        err_line("a", Errc::kIo, "line\nbreak\r"),
        event_line(R"j({"evt":"x"})j"),
    };
    for (const std::string& line : lines) {
        SCOPED_TRACE(line);
        EXPECT_TRUE(std::string_view(line).starts_with(kPrefix));
        EXPECT_EQ(line.find_first_of("\r\n"), std::string::npos);
    }
    // And the usual log line formats are not mistaken for protocol lines.
    EXPECT_FALSE(std::string_view("I (123) console: #- status").starts_with(kPrefix));
}

TEST(Framing, LinesThatDoNotFitAreRefusedWholeAtEveryCapacity) {
    const std::string ok_expected = R"j(@QZ1 a1 OK {"x":12})j";
    const std::string err_expected = R"j(@QZ1 a1 ERR busy {"msg":"try \"later\""})j";
    const std::string event_expected = R"j(@QZ1 ! EVT {"evt":"wake"})j";
    for (std::size_t capacity = 0; capacity <= err_expected.size() + 1; ++capacity) {
        SCOPED_TRACE(capacity);
        const std::string ok = ok_line("a1", R"j({"x":12})j", capacity);
        EXPECT_EQ(ok, capacity >= ok_expected.size() ? ok_expected : "");
        const std::string err = err_line("a1", Errc::kBusy, "try \"later\"", capacity);
        EXPECT_EQ(err, capacity >= err_expected.size() ? err_expected : "");
        const std::string event = event_line(R"j({"evt":"wake"})j", capacity);
        EXPECT_EQ(event, capacity >= event_expected.size() ? event_expected : "");
    }
}

TEST(Framing, FormatOkAcceptsJsonThatLivesInTheOutputBuffer) {
    // A caller may re-frame JSON it wrote at the end of its own buffer.
    std::array<char, 64> buffer{};
    const std::string_view json = R"j({"k":1})j";
    std::ranges::copy(json, buffer.end() - static_cast<std::ptrdiff_t>(json.size()));
    const std::string_view moved(buffer.data() + buffer.size() - json.size(), json.size());
    const std::size_t length = format_ok(buffer, "z", moved);
    EXPECT_EQ(std::string_view(buffer.data(), length), R"j(@QZ1 z OK {"k":1})j");
}

} // namespace
} // namespace qz::console
