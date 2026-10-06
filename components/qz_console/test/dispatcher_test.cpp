// Dispatcher (registry.hpp): one response line per request, id echo, error mapping, overflow,
// radio gating, sensitive-command hygiene, and a heap-free dispatch path.
#include "json_check.hpp"
#include "null_device_api.hpp"
#include "qz/console/protocol.hpp"
#include "qz/console/registry.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---- allocation counting ---------------------------------------------------------------------
// The dispatch path must never touch the heap (ARCHITECTURE.md section 2). Replacing the global
// allocation functions lets a test count what happens while it is armed; everything else in the
// test binary (GoogleTest included) allocates as usual.

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::uint32_t> g_allocations{0};
std::atomic<std::uint32_t> g_logged{0};
} // namespace
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Every allocation form goes through malloc and every delete form through free, so that no
// block is ever allocated by one allocator (ASan's) and released by another.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads)
namespace {
void* counted_malloc(std::size_t size) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size == 0 ? 1 : size);
}
} // namespace

void* operator new(std::size_t size) {
    void* block = counted_malloc(size);
    if (block == nullptr) {
        std::abort();
    }
    return block;
}
void* operator new[](std::size_t size) {
    return operator new(size);
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return counted_malloc(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return counted_malloc(size);
}
void operator delete(void* block) noexcept {
    std::free(block);
}
void operator delete(void* block, std::size_t /*size*/) noexcept {
    std::free(block);
}
void operator delete(void* block, const std::nothrow_t& /*tag*/) noexcept {
    std::free(block);
}
void operator delete[](void* block) noexcept {
    std::free(block);
}
void operator delete[](void* block, std::size_t /*size*/) noexcept {
    std::free(block);
}
void operator delete[](void* block, const std::nothrow_t& /*tag*/) noexcept {
    std::free(block);
}
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads)

namespace qz::console {
namespace {

using test::ExactBuffer;
using test::NullDeviceApi;
using namespace std::string_literals;

constexpr std::size_t kHugeBuffer = std::size_t{64} * 1024; // larger than any response may be

// ---- test commands ---------------------------------------------------------------------------
// Handlers are plain function pointers, so what they do is steered by their arguments.

std::uint32_t to_uint(std::string_view digits) {
    std::uint32_t value = 0;
    for (const char c : digits) {
        value = (value * 10U) + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

constexpr std::array<char, 70'000> make_filler() noexcept {
    std::array<char, 70'000> text{};
    for (char& c : text) {
        c = 'x';
    }
    return text;
}
constexpr std::array<char, 70'000> kFiller = make_filler(); // built at compile time: no heap

constexpr std::array<std::uint8_t, 5000> make_pattern() noexcept {
    std::array<std::uint8_t, 5000> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(((i * 7U) + 3U) & 0xFFU);
    }
    return bytes;
}
constexpr std::array<std::uint8_t, 5000> kFramebuffer = make_pattern();

Status ok_handler(DeviceApi& /*api*/, std::span<const std::string_view> /*args*/, JsonWriter& out) {
    out.field("v", 1);
    return ok();
}

/// Writes nothing: the shortest possible success answer is "{}".
Status
quiet_handler(DeviceApi& /*api*/, std::span<const std::string_view> /*args*/, JsonWriter& /*out*/) {
    return ok();
}

Status echo_handler(DeviceApi& /*api*/, std::span<const std::string_view> args, JsonWriter& out) {
    out.field("n", static_cast<std::int64_t>(args.size()));
    out.begin_array("args");
    for (const std::string_view arg : args) {
        out.str(arg);
    }
    out.end_array();
    return ok();
}

/// Calls the DeviceApi it was given: the test sees the call on its own instance.
Status vibrate_handler(DeviceApi& api, std::span<const std::string_view> args, JsonWriter& out) {
    out.field("ok", 1);
    return api.vibrate(static_cast<std::uint16_t>(args.size() + 100U));
}

/// `fail <errc> [detail]`: writes some output, then fails with the Errc given by number.
Status fail_handler(DeviceApi& /*api*/, std::span<const std::string_view> args, JsonWriter& out) {
    out.field("leaked", "partial output");
    out.begin_array("also");
    const std::uint32_t detail = args.size() > 1 ? to_uint(args[1]) : 0U;
    return Error{static_cast<Errc>(to_uint(args[0])), static_cast<std::uint16_t>(detail)};
}

Status io_failure_handler(DeviceApi& /*api*/,
                          std::span<const std::string_view> /*args*/,
                          JsonWriter& /*out*/) {
    return Errc::kIo;
}

/// `big <n>`: {"s":"xxx..."} with n characters.
Status big_handler(DeviceApi& /*api*/, std::span<const std::string_view> args, JsonWriter& out) {
    out.field("s", std::string_view(kFiller.data(), kFiller.size()).substr(0, to_uint(args[0])));
    return ok();
}

Status
dump_handler(DeviceApi& /*api*/, std::span<const std::string_view> /*args*/, JsonWriter& out) {
    out.key("b64").base64(kFramebuffer);
    return ok();
}

/// Forgets to close the object it opens.
Status
leaky_handler(DeviceApi& /*api*/, std::span<const std::string_view> /*args*/, JsonWriter& out) {
    out.key("inner").begin_object();
    return ok();
}

struct Reply {
    std::string line;
    std::string id;
    std::string verb; ///< "OK" or "ERR"
    std::string code; ///< error token, empty for OK
    std::string json;
};

/// Splits a response line and checks what every response must satisfy.
// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
Reply split_reply(const std::string& line) {
    Reply reply;
    reply.line = line;
    EXPECT_TRUE(std::string_view(line).starts_with(kPrefix)) << line;
    EXPECT_EQ(line.find_first_of("\r\n"), std::string::npos) << line;
    EXPECT_LE(line.size(), kMaxResponseBytes);
    std::string_view rest = std::string_view(line).substr(std::min(kPrefix.size(), line.size()));
    const auto take = [&rest]() {
        const std::size_t space = rest.find(' ');
        std::string token(rest.substr(0, space));
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        return token;
    };
    reply.id = take();
    reply.verb = take();
    if (reply.verb == "ERR") {
        reply.code = take();
    }
    reply.json = std::string(rest);
    const test::JsonDoc doc = test::parse_json(reply.json);
    EXPECT_TRUE(doc.valid) << line;
    EXPECT_GE(doc.max_depth, 1U) << line; // always an object
    EXPECT_TRUE(reply.json.starts_with('{')) << line;
    return reply;
}

class DispatcherTest : public test::LogCaptureTest {
protected:
    DispatcherTest() {
        add("ok", 0, 0, kFlagNone, ok_handler, "ok");
        add("quiet", 0, 0, kFlagNone, quiet_handler, "quiet");
        add("echo", 0, 3, kFlagNone, echo_handler, "echo [a] [b] [c]");
        add("time get", 0, 0, kFlagNone, ok_handler, "time get");
        add("time set", 1, 1, kFlagNone, echo_handler, "time set <ISO-8601>");
        add("vibrate", 0, 1, kFlagNone, vibrate_handler, "vibrate [ms]");
        add("fail", 1, 2, kFlagNone, fail_handler, "fail <errc> [detail]");
        add("big", 1, 1, kFlagNone, big_handler, "big <n>");
        add("dump", 0, 0, kFlagNone, dump_handler, "dump");
        add("leaky", 0, 0, kFlagNone, leaky_handler, "leaky");
        add("wipe", 0, 0, kFlagDestructive, ok_handler, "wipe");
        add("sync now", 0, 0, kFlagNeedsRadio, vibrate_handler, "sync now");
        add("wifi set", 2, 2, kFlagSensitive, ok_handler, "wifi set <ssid> <password>");
        add("keys set", 1, 1, kFlagSensitive, io_failure_handler, "keys set <key>");
        add("net set", 1, 1, kFlagSensitive | kFlagNeedsRadio, ok_handler, "net set <password>");
    }

    void add(std::string_view name,
             std::uint8_t min_args,
             std::uint8_t max_args,
             std::uint8_t flags,
             Handler handler,
             std::string_view usage) {
        const Command command{name, usage, "help text", min_args, max_args, flags, handler};
        ASSERT_TRUE(registry_.add(command).has_value()) << name;
    }

    /// Runs one request through a fresh Dispatcher. The request and the response buffer are heap
    /// blocks of exactly their size, so ASan flags any access past them.
    std::string run(std::string_view line,
                    bool radio = true,
                    std::size_t capacity = kMaxResponseBytes,
                    std::string* line_after = nullptr) {
        ExactBuffer request(line);
        std::vector<char> response(capacity);
        Dispatcher dispatcher(registry_, api_, radio);
        const std::string_view reply = dispatcher.handle_line(
            request.span(), std::span<char>(response.data(), response.size()));
        EXPECT_TRUE(reply.empty() || reply.data() == response.data());
        if (line_after != nullptr) {
            *line_after = request.str();
        }
        return std::string(reply);
    }

    Registry registry_;
    NullDeviceApi api_;
};

// ---- id echo and the success path ------------------------------------------------------------

TEST_F(DispatcherTest, SuccessAnswersOneOkLineAndEchoesTheRequestId) {
    EXPECT_EQ(run("#a1 ok"), R"j(@QZ1 a1 OK {"v":1})j");
    EXPECT_EQ(run("ok"), R"j(@QZ1 - OK {"v":1})j");
    EXPECT_EQ(run("#ABCDEFGH ok"), R"j(@QZ1 ABCDEFGH OK {"v":1})j");
    EXPECT_EQ(run("  #z9   ok  "), R"j(@QZ1 z9 OK {"v":1})j");
}

TEST_F(DispatcherTest, HandlersGetTheArgumentsThatFollowTheCommandWords) {
    EXPECT_EQ(run("echo a \"b c\" \"\""), R"j(@QZ1 - OK {"n":3,"args":["a","b c",""]})j");
    EXPECT_EQ(run("echo"), R"j(@QZ1 - OK {"n":0,"args":[]})j");
    // A two-word name consumes two tokens: the handler sees only what follows them.
    EXPECT_EQ(run("time set 2026-10-05T12:00:00Z"),
              R"j(@QZ1 - OK {"n":1,"args":["2026-10-05T12:00:00Z"]})j");
    EXPECT_EQ(run("time get"), R"j(@QZ1 - OK {"v":1})j");
}

TEST_F(DispatcherTest, HandlersGetTheDispatchersDeviceApi) {
    EXPECT_EQ(run("vibrate 5"), R"j(@QZ1 - OK {"ok":1})j");
    EXPECT_EQ(api_.vibrate_calls, 1U);
    EXPECT_EQ(api_.last_vibrate_ms, 101U); // one argument
}

TEST_F(DispatcherTest, StringsInResponsesStayOnOneLineWhateverTheArguments) {
    // Strings 0 and 1 of each document are the keys "n" and "args".
    const Reply escaped = split_reply(run("echo \"quo\\\"te\" \"tab\there\" \"\x01\""));
    EXPECT_EQ(escaped.verb, "OK");
    const test::JsonDoc escaped_doc = test::parse_json(escaped.json);
    ASSERT_TRUE(escaped_doc.valid);
    ASSERT_EQ(escaped_doc.strings.size(), 5U);
    EXPECT_EQ(escaped_doc.strings[2], "quo\"te");
    EXPECT_EQ(escaped_doc.strings[3], "tab\there");
    EXPECT_EQ(escaped_doc.strings[4], "\x01");

    // UTF-8 passes through; the stray byte \xFF becomes U+FFFD so the line stays valid UTF-8.
    const Reply utf8 = split_reply(run("echo caf\xC3\xA9 \xFF"));
    EXPECT_EQ(utf8.verb, "OK");
    const test::JsonDoc utf8_doc = test::parse_json(utf8.json);
    ASSERT_TRUE(utf8_doc.valid);
    ASSERT_EQ(utf8_doc.strings.size(), 4U);
    EXPECT_EQ(utf8_doc.strings[2], "caf\xC3\xA9");
    EXPECT_EQ(utf8_doc.strings[3], "\xEF\xBF\xBD");
}

TEST_F(DispatcherTest, DestructiveFlagDoesNotChangeDispatch) {
    EXPECT_EQ(run("wipe"), R"j(@QZ1 - OK {"v":1})j");
}

TEST_F(DispatcherTest, EachCallStandsAloneEvenWhenTheResponseBufferIsReused) {
    ExactBuffer first("#l1 big 3000"s);
    ExactBuffer second("#s1 ok"s);
    std::vector<char> response(kMaxResponseBytes);
    const std::span<char> out(response.data(), response.size());
    Dispatcher dispatcher(registry_, api_, true);
    const std::string_view big = dispatcher.handle_line(first.span(), out);
    EXPECT_GT(big.size(), 3000U);
    const std::string_view small = dispatcher.handle_line(second.span(), out);
    EXPECT_EQ(small, R"j(@QZ1 s1 OK {"v":1})j");
}

// ---- unknown commands, argument counts, parse errors -----------------------------------------

TEST_F(DispatcherTest, UnknownCommandIsAnErrorLineThatEchoesNoArguments) {
    EXPECT_EQ(run("#u1 frobnicate secret-argument"),
              R"j(@QZ1 u1 ERR unknown_cmd {"msg":"unknown command, try help"})j");
    EXPECT_EQ(run("frobnicate"), R"j(@QZ1 - ERR unknown_cmd {"msg":"unknown command, try help"})j");
    // Only the two-word forms of "time" exist; a lone "time" or an unknown second word is unknown.
    EXPECT_EQ(split_reply(run("time")).code, "unknown_cmd");
    EXPECT_EQ(split_reply(run("time bogus x")).code, "unknown_cmd");
    EXPECT_EQ(split_reply(run("OK")).code, "unknown_cmd"); // case matters
}

TEST_F(DispatcherTest, ArgumentCountIsCheckedAgainstTheCommandsBounds) {
    EXPECT_EQ(run("#c1 ok extra"), R"j(@QZ1 c1 ERR bad_args {"msg":"usage: ok"})j");
    EXPECT_EQ(run("time set"), R"j(@QZ1 - ERR bad_args {"msg":"usage: time set <ISO-8601>"})j");
    EXPECT_EQ(run("time set a b"), R"j(@QZ1 - ERR bad_args {"msg":"usage: time set <ISO-8601>"})j");
    EXPECT_EQ(split_reply(run("echo 1 2 3 4")).code, "bad_args");
    EXPECT_EQ(split_reply(run("echo 1 2 3")).verb, "OK");
}

TEST_F(DispatcherTest, MalformedRequestsAnswerBadArgsAndEchoAValidId) {
    EXPECT_EQ(run("#p1 echo \"oops"), R"j(@QZ1 p1 ERR bad_args {"msg":"unterminated quote"})j");
    EXPECT_EQ(run("#p2 echo \"a\\qb\""),
              R"j(@QZ1 p2 ERR bad_args {"msg":"bad escape, only \\\" and \\\\ exist"})j");
    EXPECT_EQ(
        run("#p3 echo a\"b"),
        R"j(@QZ1 p3 ERR bad_args {"msg":"a quote must open an argument and be followed by a space"})j");
    EXPECT_EQ(run("#p4"), R"j(@QZ1 p4 ERR bad_args {"msg":"missing command"})j");
    // An unusable id cannot be echoed.
    EXPECT_EQ(run("#bad-id echo"),
              R"j(@QZ1 - ERR bad_args {"msg":"bad request id, use 1-8 letters or digits"})j");
    EXPECT_EQ(run("#waytoolongid ok"),
              R"j(@QZ1 - ERR bad_args {"msg":"bad request id, use 1-8 letters or digits"})j");
    EXPECT_EQ(run(""), R"j(@QZ1 - ERR bad_args {"msg":"empty request"})j");
    EXPECT_EQ(run("   "), R"j(@QZ1 - ERR bad_args {"msg":"empty request"})j");
}

TEST_F(DispatcherTest, RequestsOver256BytesAreRefusedWholeAndOneOfExactly256Runs) {
    const std::string at_limit = "echo " + std::string(kMaxRequestBytes - 5, 'x');
    const Reply fits = split_reply(run(at_limit));
    EXPECT_EQ(fits.verb, "OK");
    const Reply too_long = split_reply(run("#t1 " + at_limit));
    EXPECT_EQ(too_long.line, R"j(@QZ1 - ERR bad_args {"msg":"request too long"})j");
}

TEST_F(DispatcherTest, TooManyTokensAreRefused) {
    std::string line = "echo";
    for (std::size_t i = 0; i < kMaxArgs + 2; ++i) {
        line += " a";
    }
    EXPECT_EQ(run(line), R"j(@QZ1 - ERR bad_args {"msg":"too many arguments"})j");
}

// ---- handler failures ------------------------------------------------------------------------

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(DispatcherTest, HandlerErrorsBecomeTheirTokensAndDiscardPartialOutput) {
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
    for (std::size_t code = 0; code < kTokens.size(); ++code) {
        SCOPED_TRACE(kTokens[code]);
        const Reply reply = split_reply(run("#f1 fail " + std::to_string(code)));
        EXPECT_EQ(reply.verb, "ERR");
        EXPECT_EQ(reply.id, "f1");
        EXPECT_EQ(reply.code, kTokens[code]);
        EXPECT_EQ(reply.line.find("leaked"), std::string::npos);
        EXPECT_EQ(reply.line.find("partial"), std::string::npos);
    }
}

TEST_F(DispatcherTest, HandlerErrorsExplainThemselves) {
    EXPECT_EQ(run("fail 3"),
              R"j(@QZ1 - ERR invalid_state {"msg":"not allowed in the current state"})j");
    EXPECT_EQ(run("fail 5 259"), R"j(@QZ1 - ERR io {"msg":"i/o failure (detail 259)"})j");
    EXPECT_EQ(run("fail 2"), R"j(@QZ1 - ERR unsupported {"msg":"not supported on this build"})j");
    // A handler that rejects its arguments gets the command's usage line.
    EXPECT_EQ(run("fail 0"), R"j(@QZ1 - ERR bad_args {"msg":"usage: fail <errc> [detail]"})j");
}

TEST_F(DispatcherTest, ABuggyHandlerThatLeavesItsJsonOpenIsAnInternalError) {
    EXPECT_EQ(run("#g1 leaky"),
              R"j(@QZ1 g1 ERR internal {"msg":"handler left its JSON unbalanced"})j");
}

// ---- response buffer limits ------------------------------------------------------------------

TEST_F(DispatcherTest, ResponseThatDoesNotFitIsNoSpaceAtTheExactBoundary) {
    const std::string line = "#o1 big 100";
    const std::string expected = R"j(@QZ1 o1 OK {"s":")j" + std::string(100, 'x') + "\"}";
    EXPECT_EQ(run(line, true, expected.size()), expected);
    const std::string overflow = run(line, true, expected.size() - 1);
    EXPECT_EQ(overflow, R"j(@QZ1 o1 ERR no_space {"msg":"response too large"})j");
}

TEST_F(DispatcherTest, ResponseLineIsCappedAt16KiBEvenInALargerBuffer) {
    // "@QZ1 - OK " (10) + {"s":" (6) + n + "} (2): n = 16366 makes exactly 16384.
    const std::size_t at_limit = kMaxResponseBytes - 18;
    const std::string fits = run("big " + std::to_string(at_limit), true, kHugeBuffer);
    EXPECT_EQ(fits.size(), kMaxResponseBytes);
    EXPECT_EQ(split_reply(fits).verb, "OK");
    const std::string over = run("big " + std::to_string(at_limit + 1), true, kHugeBuffer);
    EXPECT_EQ(over, R"j(@QZ1 - ERR no_space {"msg":"response too large"})j");
}

TEST_F(DispatcherTest, FramebufferDumpFitsWithItsBase64) {
    const Reply reply = split_reply(run("#d1 dump"));
    EXPECT_EQ(reply.verb, "OK");
    EXPECT_EQ(reply.id, "d1");
    EXPECT_EQ(reply.line.size(), std::string_view("@QZ1 d1 OK {\"b64\":\"\"}").size() + 6668);
}

TEST_F(DispatcherTest, ABufferTooSmallForTheShortestAnswerGetsNoHandlerCallAndAnEmptyReply) {
    // The shortest success line is "@QZ1 - OK {}": 12 bytes.
    for (std::size_t capacity = 0; capacity < 12; ++capacity) {
        SCOPED_TRACE(capacity);
        EXPECT_EQ(run("vibrate", true, capacity), "");
        EXPECT_EQ(api_.vibrate_calls, 0U); // the command was refused before it could act
    }
    EXPECT_EQ(run("quiet", true, 12), "@QZ1 - OK {}");
    // A two-character id makes the shortest success line one byte longer: 13.
    EXPECT_EQ(run("#ab quiet", true, 13), "@QZ1 ab OK {}");
    EXPECT_EQ(run("#ab quiet", true, 12), "");
}

TEST_F(DispatcherTest, ErrorLinesThatDoNotFitEitherAreEmptyNotTruncated) {
    const std::string full = R"j(@QZ1 - ERR unknown_cmd {"msg":"unknown command, try help"})j";
    EXPECT_EQ(run("nonsense", true, full.size()), full);
    for (std::size_t capacity = 0; capacity < full.size(); ++capacity) {
        EXPECT_EQ(run("nonsense", true, capacity), "") << capacity;
    }
}

// ---- radio gating ----------------------------------------------------------------------------

TEST_F(DispatcherTest, RadioCommandsAreUnsupportedWhenTheRadioIsCompiledOut) {
    EXPECT_EQ(run("#r1 sync now", false),
              R"j(@QZ1 r1 ERR unsupported {"msg":"radio not compiled in"})j");
    EXPECT_EQ(api_.vibrate_calls, 0U); // the handler never ran
    // The build feature wins over argument checking.
    EXPECT_EQ(split_reply(run("sync now extra", false)).code, "unsupported");
}

TEST_F(DispatcherTest, RadioCommandsRunNormallyWhenTheRadioIsCompiledIn) {
    EXPECT_EQ(run("#r2 sync now", true), R"j(@QZ1 r2 OK {"ok":1})j");
    EXPECT_EQ(api_.vibrate_calls, 1U);
    EXPECT_EQ(split_reply(run("sync now extra", true)).code, "bad_args");
}

TEST_F(DispatcherTest, CommandsWithoutTheRadioFlagIgnoreTheBuildFeature) {
    EXPECT_EQ(run("ok", false), R"j(@QZ1 - OK {"v":1})j");
    EXPECT_EQ(run("echo a", false), R"j(@QZ1 - OK {"n":1,"args":["a"]})j");
}

// ---- logging and sensitive commands ----------------------------------------------------------

TEST_F(DispatcherTest, AcceptedRequestsLogTheirArgumentsAtDebugLevel) {
    EXPECT_EQ(split_reply(run("#l1 echo alpha \"beta gamma\"")).verb, "OK");
    ASSERT_FALSE(test::captured_logs().empty());
    const test::LogLine& line = test::captured_logs().front();
    EXPECT_EQ(line.level, LogLevel::kDebug);
    EXPECT_EQ(line.tag, "console");
    EXPECT_EQ(line.message, "#l1 echo alpha beta gamma");
}

TEST_F(DispatcherTest, ARequestWithoutIdIsLoggedWithADash) {
    EXPECT_EQ(split_reply(run("ok")).verb, "OK");
    EXPECT_TRUE(logged("#- ok"));
}

TEST_F(DispatcherTest, SensitiveCommandsNeverLogTheirArguments) {
    const std::string reply = run("#s1 wifi set HomeNet \"hunter 2\"");
    EXPECT_EQ(reply, R"j(@QZ1 s1 OK {"v":1})j");
    // Logging is alive (the command name is there), so the absence below means something.
    ASSERT_FALSE(test::captured_logs().empty());
    EXPECT_TRUE(logged("wifi set"));
    EXPECT_TRUE(logged("#s1 wifi set <arguments hidden>"));
    for (const test::LogLine& line : test::captured_logs()) {
        SCOPED_TRACE(line.message);
        EXPECT_EQ(line.message.find("HomeNet"), std::string::npos);
        EXPECT_EQ(line.message.find("hunter"), std::string::npos);
    }
}

TEST_F(DispatcherTest, SensitiveCommandsKeepTheirArgumentsOutOfEveryFailureToo) {
    struct Case {
        std::string line;
        bool radio;
        std::string_view code;
    };
    const std::vector<Case> cases = {
        {"#k1 keys set topsecret-key", true, "io"},            // handler fails
        {"#k2 wifi set only-one-argument", true, "bad_args"},  // too few arguments
        {"#k3 wifi set a b c-extra-secret", true, "bad_args"}, // too many arguments
        {"#k4 net set radio-secret", false, "unsupported"},    // radio compiled out
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.line);
        test::captured_logs().clear();
        const Reply reply = split_reply(run(c.line, c.radio));
        EXPECT_EQ(reply.code, c.code);
        for (const std::string_view secret :
             {"topsecret", "only-one", "extra-secret", "radio-secret", "wifi set a b"}) {
            EXPECT_EQ(reply.line.find(secret), std::string::npos) << secret;
            EXPECT_FALSE(logged(secret)) << secret;
        }
    }
}

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(DispatcherTest, RequestsThatDidNotResolveToACommandLogNoRequestText) {
    // A mistyped sensitive command, an unterminated quote, and a bad id must not leak either.
    for (const std::string_view line : {"wfi set HomeNet hunter2",
                                        "wifi set HomeNet \"hunter2",
                                        "#bad! wifi set HomeNet hunter2",
                                        "wifi set HomeNet \"hunter2\"x"}) {
        SCOPED_TRACE(line);
        test::captured_logs().clear();
        const Reply reply = split_reply(run(line));
        EXPECT_EQ(reply.verb, "ERR");
        EXPECT_EQ(reply.line.find("hunter2"), std::string::npos);
        EXPECT_EQ(reply.line.find("HomeNet"), std::string::npos);
        EXPECT_FALSE(test::captured_logs().empty()); // the rejection itself is logged
        EXPECT_FALSE(logged("hunter2"));
        EXPECT_FALSE(logged("HomeNet"));
    }
}

TEST_F(DispatcherTest, TheRequestLineIsZeroedAfterSensitiveUnknownAndMalformedRequests) {
    for (const std::string_view line : {"wifi set HomeNet hunter2",
                                        R"(#s1 wifi set "Home Net" "hun ter2")",
                                        "keys set secret",
                                        "wfi set HomeNet hunter2",
                                        "wifi set HomeNet \"hunter2",
                                        "#!! wifi set HomeNet hunter2"}) {
        SCOPED_TRACE(line);
        std::string after;
        (void)run(line, true, kMaxResponseBytes, &after); // the reply is checked elsewhere
        ASSERT_EQ(after.size(), line.size());
        EXPECT_EQ(after, std::string(line.size(), '\0'));
    }
}

TEST_F(DispatcherTest, LoggedArgumentsCannotForgeLinesOrDriveTheTerminal) {
    const std::string line = "echo \"a\rb\x1b[31m\x7f\" \"@QZ1 ! EVT {}\" \"x\ty\"";
    EXPECT_EQ(split_reply(run(line)).verb, "OK");
    ASSERT_FALSE(test::captured_logs().empty());
    for (const test::LogLine& log : test::captured_logs()) {
        SCOPED_TRACE(log.message);
        for (const char c : log.message) {
            EXPECT_GE(static_cast<unsigned char>(c), 0x20U);
            EXPECT_NE(c, '\x7f');
        }
        EXPECT_FALSE(std::string_view(log.message).starts_with(kPrefix));
    }
    EXPECT_TRUE(logged("echo a?b?[31m? @QZ1 ! EVT {} x?y"));
}

TEST_F(DispatcherTest, ALongArgumentListIsCutInTheLogNotInTheResponse) {
    const std::string line = "echo " + std::string(100, 'a') + " " + std::string(100, 'b');
    const Reply reply = split_reply(run(line));
    EXPECT_EQ(reply.verb, "OK");
    EXPECT_NE(reply.line.find(std::string(100, 'b')), std::string::npos); // the response is whole
    ASSERT_FALSE(test::captured_logs().empty());
    EXPECT_LT(test::captured_logs().front().message.size(), 192U);
    EXPECT_FALSE(logged(std::string(100, 'b'))); // the log line is cut
}

TEST_F(DispatcherTest, FailedCommandsAreLoggedByNameAndTokenOnly) {
    EXPECT_EQ(split_reply(run("fail 5 259")).code, "io");
    EXPECT_TRUE(logged("fail failed: io"));
}

TEST_F(DispatcherTest, NothingIsLoggedBelowDebugLevel) {
    set_log_level(LogLevel::kInfo);
    EXPECT_EQ(split_reply(run("echo a")).verb, "OK");
    EXPECT_TRUE(test::captured_logs().empty());
}

// ---- no heap ---------------------------------------------------------------------------------

void count_log(LogLevel /*level*/, const char* /*tag*/, const char* /*message*/) noexcept {
    g_logged.fetch_add(1, std::memory_order_relaxed);
}

TEST_F(DispatcherTest, TheDispatchPathNeverAllocates) {
    set_log_sink(&count_log); // allocation-free, and at verbose level every log path runs
    constexpr std::array<std::string_view, 14> kLines = {
        "#a1 ok",
        "echo a \"b c\" d",
        "time set 2026-01-01T00:00:00Z",
        "frobnicate x",
        "echo \"open",
        "#bad! x",
        "ok extra",
        "fail 5 259",
        "big 4000",
        "dump",
        "sync now",
        "wifi set Home pw",
        "keys set k",
        "leaky",
    };
    std::array<char, kMaxRequestBytes> line{};
    std::vector<char> response_storage(kMaxResponseBytes);
    const std::span<char> response(response_storage.data(), response_storage.size());
    Dispatcher dispatcher(registry_, api_, false); // radio compiled out: covers "unsupported" too
    std::array<std::size_t, kLines.size()> reply_sizes{};
    g_logged.store(0);
    g_allocations.store(0);
    g_counting.store(true);
    for (std::size_t i = 0; i < kLines.size(); ++i) {
        std::copy(kLines[i].begin(), kLines[i].end(), line.begin());
        reply_sizes[i] =
            dispatcher.handle_line(std::span<char>(line.data(), kLines[i].size()), response).size();
    }
    g_counting.store(false);
    EXPECT_EQ(g_allocations.load(), 0U);
    EXPECT_GE(g_logged.load(), kLines.size() - 1); // the logging code really ran
    for (const std::size_t size : reply_sizes) {
        EXPECT_GT(size, 0U);
    }
}

// ---- the console loop of ARCHITECTURE.md section 16, over the fake port ----------------------

TEST_F(DispatcherTest, ALoopOverTheFakePortAnswersEachRequestWithExactlyOneLine) {
    testkit::FakeConsolePort port;
    ASSERT_TRUE(port.start().has_value());
    port.push_request("#r1 echo a b");
    port.push_request("bogus");
    port.push_request("#r3 time set \"2026-10-05T12:00:00Z\"");
    port.push_request("#r4 ok extra");

    std::array<char, kMaxRequestBytes + 1> line{};
    std::array<char, kMaxResponseBytes> response{};
    Dispatcher dispatcher(registry_, api_, true);
    while (port.pending_requests() > 0) {
        const Result<std::size_t> length = port.receive_line(line, 0);
        ASSERT_TRUE(length.has_value());
        port.send_line(dispatcher.handle_line(std::span<char>(line.data(), *length), response));
    }
    ASSERT_EQ(port.sent().size(), 4U);
    EXPECT_EQ(port.sent()[0], R"j(@QZ1 r1 OK {"n":2,"args":["a","b"]})j");
    EXPECT_EQ(port.sent()[1], R"j(@QZ1 - ERR unknown_cmd {"msg":"unknown command, try help"})j");
    EXPECT_EQ(port.sent()[2], R"j(@QZ1 r3 OK {"n":1,"args":["2026-10-05T12:00:00Z"]})j");
    EXPECT_EQ(port.sent()[3], R"j(@QZ1 r4 ERR bad_args {"msg":"usage: ok"})j");

    // An event goes through the same port, framed by the protocol.
    std::array<char, 128> event_json{};
    JsonWriter json(event_json);
    json.begin_object().field("evt", "detach").end_object();
    std::array<char, 160> event_line{};
    const std::size_t event_length = format_event(event_line, json.view());
    port.send_line(std::string_view(event_line.data(), event_length));
    EXPECT_EQ(port.sent().back(), R"j(@QZ1 ! EVT {"evt":"detach"})j");
}

} // namespace
} // namespace qz::console
