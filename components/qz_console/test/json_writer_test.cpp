// JsonWriter (protocol.hpp): structure, escaping, UTF-8, numbers, base64, overflow, misuse.
#include "../src/tuning.hpp"
#include "json_check.hpp"
#include "qz/console/protocol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace qz::console {
namespace {

using test::is_valid_utf8;
using test::parse_json;
using namespace std::string_literals;

struct Written {
    std::string text;
    bool overflowed = false;
    std::size_t depth = 0;
};

/// Runs `build` against a writer over a heap block of exactly `capacity` bytes (ASan flags any
/// write past it) and returns what the writer reports.
template<class Build>
Written write_with_capacity(std::size_t capacity, Build build) {
    std::vector<char> storage(capacity);
    // The builder takes a JsonWriter&.
    // NOLINTNEXTLINE(misc-const-correctness)
    JsonWriter writer(std::span<char>(storage.data(), storage.size()));
    build(writer);
    return {std::string(writer.view()), writer.overflowed(), writer.depth()};
}

/// The document `build` writes into a buffer that is large enough.
template<class Build>
std::string write_json(Build build) {
    constexpr std::size_t kRoomy = 70'000;
    const Written written = write_with_capacity(kRoomy, build);
    EXPECT_FALSE(written.overflowed);
    EXPECT_EQ(written.depth, 0U);
    return written.text;
}

// ---- structure -------------------------------------------------------------------------------

TEST(JsonWriter, EmptyDocuments) {
    EXPECT_EQ(write_json([](JsonWriter& w) { w.begin_object().end_object(); }), "{}");
    EXPECT_EQ(write_json([](JsonWriter& w) { w.begin_array().end_array(); }), "[]");
}

TEST(JsonWriter, SeparatorsAndNestingAreExact) {
    const std::string text = write_json([](JsonWriter& w) {
        w.begin_object()
            .field("a", 1)
            .key("b")
            .begin_object()
            .field("c", "x")
            .key("d")
            .null()
            .end_object()
            .begin_array("list")
            .num(1)
            .str("two")
            .boolean(true)
            .null()
            .begin_array()
            .end_array()
            .begin_object()
            .end_object()
            .end_array()
            .field_bool("t", false)
            .end_object();
    });
    EXPECT_EQ(text,
              R"j({"a":1,"b":{"c":"x","d":null},"list":[1,"two",true,null,[],{}],"t":false})j");
    EXPECT_TRUE(parse_json(text).valid);
}

TEST(JsonWriter, ReadyEventFromTheArchitectureDocument) {
    const std::string text = write_json([](JsonWriter& w) {
        w.begin_object()
            .field("evt", "ready")
            .field("proto", kProtocolVersion)
            .field("fw", "1.0.0")
            .field("git", "abc1234")
            .field("reset", "deepsleep")
            .end_object();
    });
    EXPECT_EQ(text,
              R"j({"evt":"ready","proto":1,"fw":"1.0.0","git":"abc1234","reset":"deepsleep"})j");
}

TEST(JsonWriter, ScalarsAndArraysWorkAtTheTopLevel) {
    EXPECT_EQ(write_json([](JsonWriter& w) { w.num(7); }), "7");
    EXPECT_EQ(write_json([](JsonWriter& w) { w.str("s"); }), "\"s\"");
    EXPECT_EQ(write_json([](JsonWriter& w) { w.boolean(true); }), "true");
    EXPECT_EQ(write_json([](JsonWriter& w) { w.null(); }), "null");
    EXPECT_EQ(write_json([](JsonWriter& w) { w.begin_array().num(1).num(2).end_array(); }),
              "[1,2]");
}

TEST(JsonWriter, DepthCountsOpenContainers) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_EQ(w.depth(), 0U);
    w.begin_object();
    EXPECT_EQ(w.depth(), 1U);
    w.begin_array("a");
    EXPECT_EQ(w.depth(), 2U);
    w.begin_object();
    EXPECT_EQ(w.depth(), 3U);
    w.end_object().end_array();
    EXPECT_EQ(w.depth(), 1U);
    w.end_object();
    EXPECT_EQ(w.depth(), 0U);
}

TEST(JsonWriter, NestingUpToTheLimitWorks) {
    constexpr std::size_t kLevels = detail::kMaxJsonDepth;
    const std::string text = write_json([](JsonWriter& w) {
        for (std::size_t i = 0; i < kLevels; ++i) {
            w.begin_array();
        }
        w.num(1);
        for (std::size_t i = 0; i < kLevels; ++i) {
            w.end_array();
        }
    });
    EXPECT_EQ(text, std::string(kLevels, '[') + "1" + std::string(kLevels, ']'));
    EXPECT_EQ(parse_json(text).max_depth, kLevels);
}

// ---- strings ---------------------------------------------------------------------------------

struct StringCase {
    std::string input;
    std::string body; ///< expected text between the quotes
};

std::string fffd(std::size_t count) {
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += "\\ufffd";
    }
    return text;
}

std::string quoted_str(std::string_view input) {
    return write_json([input](JsonWriter& w) { w.str(input); });
}

TEST(JsonWriter, StringEscapingTable) {
    const std::vector<StringCase> cases = {
        {""s, ""s},
        {"plain text 123"s, "plain text 123"s},
        {"say \"hi\""s, R"j(say \"hi\")j"s},
        {"back\\slash"s, R"j(back\\slash)j"s},
        {"\\\""s, R"j(\\\")j"s},
        {"slash/stays"s, "slash/stays"s},
        {"\b\f\n\r\t"s, R"j(\b\f\n\r\t)j"s},
        {"line1\nline2"s, R"j(line1\nline2)j"s},
        {"\x01"s, R"j(\u0001)j"s},
        {"\x0b"s, R"j(\u000b)j"s},
        {"\x1f"s, R"j(\u001f)j"s},
        {"a\0b"s, R"j(a\u0000b)j"s},
        {"\x7f"s, R"j(\u007f)j"s},
        {"\x1b[31mred"s, R"j(\u001b[31mred)j"s},
    };
    for (const StringCase& c : cases) {
        SCOPED_TRACE(c.body);
        const std::string text = quoted_str(c.input);
        EXPECT_EQ(text, "\"" + c.body + "\"");
        const test::JsonDoc doc = parse_json(text);
        ASSERT_TRUE(doc.valid);
        ASSERT_EQ(doc.strings.size(), 1U);
        EXPECT_EQ(doc.strings[0], c.input);
    }
}

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JsonWriter, EveryAsciiByteRoundTripsAndNeverLeavesAControlCharacterRaw) {
    for (int byte = 0; byte < 0x80; ++byte) {
        SCOPED_TRACE(byte);
        const std::string input(1, static_cast<char>(byte));
        const std::string text = quoted_str(input);
        for (const char c : text) {
            EXPECT_GE(static_cast<unsigned char>(c), 0x20U);
            EXPECT_NE(c, '\x7f');
        }
        const test::JsonDoc doc = parse_json(text);
        ASSERT_TRUE(doc.valid);
        ASSERT_EQ(doc.strings.size(), 1U);
        EXPECT_EQ(doc.strings[0], input);
    }
}

TEST(JsonWriter, ValidUtf8PassesThroughUntouched) {
    const std::vector<std::string> samples = {
        "caf\xC3\xA9"s,              // e acute (2 bytes)
        "\xE2\x82\xAC 5"s,           // euro sign (3 bytes)
        "\xF0\x9F\x98\x80"s,         // emoji (4 bytes)
        "\xE6\x97\xA5\xE6\x9C\xAC"s, // CJK
        "mixed \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 end"s,
        "\xC2\x80"s,         // U+0080, the first 2-byte code point
        "\xDF\xBF"s,         // U+07FF
        "\xE0\xA0\x80"s,     // U+0800
        "\xED\x9F\xBF"s,     // U+D7FF, just below the surrogates
        "\xEE\x80\x80"s,     // U+E000, just above them
        "\xEF\xBF\xBF"s,     // U+FFFF
        "\xF0\x90\x80\x80"s, // U+10000
        "\xF4\x8F\xBF\xBF"s, // U+10FFFF, the last code point
    };
    for (const std::string& sample : samples) {
        SCOPED_TRACE(sample);
        ASSERT_TRUE(is_valid_utf8(sample));
        const std::string text = quoted_str(sample);
        EXPECT_EQ(text, "\"" + sample + "\"");
        EXPECT_TRUE(parse_json(text).valid);
    }
}

TEST(JsonWriter, BytesThatAreNotUtf8BecomeAReplacementEscape) {
    const std::vector<StringCase> cases = {
        {"\x80"s, fffd(1)}, // lone continuation byte
        {"\xBF"s, fffd(1)},
        {"\xC0\x80"s, fffd(2)},         // overlong NUL
        {"\xC1\xBF"s, fffd(2)},         // overlong
        {"\xC3"s, fffd(1)},             // truncated at the end
        {"\xC3("s, fffd(1) + "("},      // lead byte followed by ASCII
        {"\xE0\x80\x80"s, fffd(3)},     // overlong
        {"\xE0\x9F\xBF"s, fffd(3)},     // overlong, below U+0800
        {"\xED\xA0\x80"s, fffd(3)},     // UTF-16 surrogate U+D800
        {"\xED\xBF\xBF"s, fffd(3)},     // UTF-16 surrogate U+DFFF
        {"\xF0\x80\x80\x80"s, fffd(4)}, // overlong
        {"\xF4\x90\x80\x80"s, fffd(4)}, // above U+10FFFF
        {"\xF5\x80\x80\x80"s, fffd(4)}, // F5..FF never occur
        {"\xFF"s, fffd(1)},
        {"\xFE"s, fffd(1)},
        {"ok\xE2\x82"s, "ok" + fffd(2)}, // euro sign cut short
        {"a\xFF"
         "z"s,
         "a" + fffd(1) + "z"},
        {"\xFF\xC3\xA9"s, fffd(1) + "\xC3\xA9"}, // a valid character right after garbage
        {"\xE2\x82\xAC\x80"s, "\xE2\x82\xAC" + fffd(1)},
    };
    for (const StringCase& c : cases) {
        SCOPED_TRACE(c.body);
        const std::string text = quoted_str(c.input);
        EXPECT_EQ(text, "\"" + c.body + "\"");
        EXPECT_TRUE(parse_json(text).valid);
    }
}

std::string random_text(std::mt19937& rng) {
    static const std::array<std::string_view, 6> kSnippets = {"\xC3\xA9",
                                                              "\xE2\x82\xAC",
                                                              "\xF0\x9F\x98\x80",
                                                              "\xED\x9F\xBF",
                                                              "\xC2\x80",
                                                              "\xF4\x8F\xBF\xBF"};
    std::uniform_int_distribution<int> length(0, 24);
    std::uniform_int_distribution<int> kind(0, 9);
    std::uniform_int_distribution<int> printable(0x20, 0x7e);
    std::uniform_int_distribution<int> control(0, 0x1f);
    std::uniform_int_distribution<int> high(0x80, 0xff);
    std::uniform_int_distribution<std::size_t> snippet(0, kSnippets.size() - 1);
    std::string text;
    for (int i = length(rng); i > 0; --i) {
        const int choice = kind(rng);
        if (choice < 4) {
            text.push_back(static_cast<char>(printable(rng)));
        } else if (choice < 5) {
            text.push_back(static_cast<char>(control(rng)));
        } else if (choice < 7) {
            text.push_back(static_cast<char>(high(rng)));
        } else {
            text += kSnippets[snippet(rng)];
        }
    }
    return text;
}

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JsonWriter, RandomTextAlwaysYieldsValidJsonAndRoundTripsWhenItIsValidUtf8) {
    // A fixed seed keeps the corpus reproducible.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(20261005);
    std::size_t valid_inputs = 0;
    std::size_t invalid_inputs = 0;
    for (int round = 0; round < 5000; ++round) {
        const std::string input = random_text(rng);
        const std::string text = quoted_str(input);
        const test::JsonDoc doc = parse_json(text);
        ASSERT_TRUE(doc.valid) << text;
        ASSERT_EQ(doc.strings.size(), 1U);
        if (is_valid_utf8(input)) {
            ++valid_inputs;
            EXPECT_EQ(doc.strings[0], input);
        } else {
            ++invalid_inputs;
            EXPECT_NE(doc.strings[0].find("\xEF\xBF\xBD"), std::string::npos) << text;
        }
    }
    // The corpus really does exercise both branches.
    EXPECT_GT(valid_inputs, 500U);
    EXPECT_GT(invalid_inputs, 500U);
}

TEST(JsonWriter, KeysAreEscapedLikeStrings) {
    const std::string text = write_json(
        [](JsonWriter& w) { w.begin_object().field("we\"ird\nkey", 1).field("", 2).end_object(); });
    EXPECT_EQ(text, R"j({"we\"ird\nkey":1,"":2})j");
    const test::JsonDoc doc = parse_json(text);
    ASSERT_TRUE(doc.valid);
    EXPECT_EQ(doc.strings[0], "we\"ird\nkey");
}

// ---- numbers ---------------------------------------------------------------------------------

TEST(JsonWriter, NumbersAreDecimalIntegers) {
    struct NumberCase {
        std::int64_t value;
        std::string_view text;
    };
    const std::vector<NumberCase> cases = {
        {0, "0"},
        {1, "1"},
        {-1, "-1"},
        {42, "42"},
        {-4242, "-4242"},
        {1'000'000, "1000000"},
        {std::numeric_limits<std::int32_t>::max(), "2147483647"},
        {std::numeric_limits<std::int32_t>::min(), "-2147483648"},
        {std::numeric_limits<std::int64_t>::max(), "9223372036854775807"},
        {std::numeric_limits<std::int64_t>::min(), "-9223372036854775808"},
    };
    for (const NumberCase& c : cases) {
        SCOPED_TRACE(c.text);
        const std::string text = write_json([&c](JsonWriter& w) { w.num(c.value); });
        EXPECT_EQ(text, c.text);
        const test::JsonDoc doc = parse_json(text);
        ASSERT_TRUE(doc.valid);
        EXPECT_EQ(doc.numbers.at(0), c.text);
    }
}

// ---- base64 ----------------------------------------------------------------------------------

std::optional<std::vector<std::uint8_t>> decode_base64(std::string_view text) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t group = 0;
        std::size_t pad = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                if (i + 4 != text.size() || j < 2) {
                    return std::nullopt; // padding only at the very end, at most two characters
                }
                ++pad;
                group <<= 6U;
                continue;
            }
            const std::size_t digit = kAlphabet.find(c);
            if (digit == std::string_view::npos || pad != 0) {
                return std::nullopt;
            }
            group = (group << 6U) | static_cast<std::uint32_t>(digit);
        }
        bytes.push_back(static_cast<std::uint8_t>(group >> 16U));
        if (pad < 2) {
            bytes.push_back(static_cast<std::uint8_t>(group >> 8U));
        }
        if (pad < 1) {
            bytes.push_back(static_cast<std::uint8_t>(group));
        }
    }
    return bytes;
}

std::string base64_document(const std::vector<std::uint8_t>& bytes) {
    return write_json([&bytes](JsonWriter& w) { w.base64(bytes); });
}

TEST(JsonWriter, Base64MatchesTheRfc4648TestVectors) {
    struct Vector {
        std::string_view plain;
        std::string_view encoded;
    };
    const std::vector<Vector> vectors = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const Vector& v : vectors) {
        SCOPED_TRACE(v.plain);
        const std::vector<std::uint8_t> bytes(v.plain.begin(), v.plain.end());
        EXPECT_EQ(base64_document(bytes), "\"" + std::string(v.encoded) + "\"");
    }
}

TEST(JsonWriter, Base64RoundTripsEveryByteValueAndEveryTailLength) {
    std::vector<std::uint8_t> all_values;
    all_values.reserve(256);
    for (int value = 0; value < 256; ++value) {
        all_values.push_back(static_cast<std::uint8_t>(value));
    }
    for (std::size_t length = 0; length <= all_values.size(); ++length) {
        SCOPED_TRACE(length);
        const std::vector<std::uint8_t> bytes(
            all_values.begin(), all_values.begin() + static_cast<std::ptrdiff_t>(length));
        const std::string text = base64_document(bytes);
        ASSERT_TRUE(parse_json(text).valid);
        const auto decoded = decode_base64(std::string_view(text).substr(1, text.size() - 2));
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(decoded.value_or(std::vector<std::uint8_t>{}), bytes);
    }
}

TEST(JsonWriter, FramebufferSizedBase64IsFourThirdsPlusPadding) {
    constexpr std::size_t kFramebufferBytes = 200 * 200 / 8; // 5000, the `display dump` payload
    std::vector<std::uint8_t> bytes(kFramebufferBytes);
    // A fixed seed keeps the data reproducible.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(5000);
    for (std::uint8_t& byte : bytes) {
        byte = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    const std::string text = base64_document(bytes);
    EXPECT_EQ(text.size(), (((kFramebufferBytes + 2) / 3) * 4) + 2); // 6668 digits + two quotes
    const auto decoded = decode_base64(std::string_view(text).substr(1, text.size() - 2));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value_or(std::vector<std::uint8_t>{}), bytes);
}

TEST(JsonWriter, Base64SitsInObjectsAndArraysLikeAnyValue) {
    const std::vector<std::uint8_t> foo = {'f', 'o', 'o'};
    const std::string text = write_json([&foo](JsonWriter& w) {
        w.begin_object()
            .key("b64")
            .base64(foo)
            .begin_array("l")
            .base64(foo)
            .base64(foo)
            .end_array()
            .end_object();
    });
    EXPECT_EQ(text, R"j({"b64":"Zm9v","l":["Zm9v","Zm9v"]})j");
}

// ---- overflow --------------------------------------------------------------------------------

void build_sample(JsonWriter& w) {
    static const std::array<std::uint8_t, 5> kBytes = {1, 2, 3, 4, 5};
    w.begin_object()
        .field("name", "a \"quoted\" \x01 caf\xC3\xA9 \xFF end")
        .field("n", -1234567)
        .key("nested")
        .begin_object()
        .field_bool("flag", true)
        .key("none")
        .null()
        .end_object()
        .begin_array("list")
        .num(1)
        .str("two")
        .begin_object()
        .field("k", "v")
        .end_object()
        .end_array()
        .key("blob")
        .base64(kBytes)
        .end_object();
}

TEST(JsonWriter, EveryTooSmallBufferOverflowsAndNeverWritesPastIt) {
    const std::string expected = write_json(build_sample);
    ASSERT_TRUE(parse_json(expected).valid);
    for (std::size_t capacity = 0; capacity < expected.size(); ++capacity) {
        SCOPED_TRACE(capacity);
        const Written written = write_with_capacity(capacity, build_sample);
        EXPECT_TRUE(written.overflowed);
        EXPECT_LE(written.text.size(), capacity);
        // Whatever was written is a prefix of the full document, and the structure bookkeeping
        // carries on to the end (a balanced document leaves no container open).
        EXPECT_EQ(written.text, expected.substr(0, written.text.size()));
        EXPECT_EQ(written.depth, 0U);
    }
    const Written exact = write_with_capacity(expected.size(), build_sample);
    EXPECT_FALSE(exact.overflowed);
    EXPECT_EQ(exact.text, expected);
}

TEST(JsonWriter, OverflowLatchesEvenForLaterWritesThatWouldFit) {
    std::array<char, 5> storage{};
    JsonWriter w(storage);
    w.begin_object();
    EXPECT_FALSE(w.overflowed());
    w.field("abcdef", "v"); // the key alone needs 9 bytes
    EXPECT_TRUE(w.overflowed());
    // What got written before the latch is the start of the document (the writer promises no
    // more than that: the bytes are an incomplete document once overflowed() is true).
    const std::string written(w.view());
    EXPECT_LT(written.size(), storage.size());
    EXPECT_TRUE(std::string_view(R"j({"abcdef":"v"})j").starts_with(written));
    w.end_object(); // the closing brace would fit in the room that is left: it must not be written
    EXPECT_TRUE(w.overflowed());
    EXPECT_EQ(w.view(), written);
    EXPECT_EQ(w.depth(), 0U); // the bookkeeping went on: the object is closed
}

TEST(JsonWriter, Base64IsWrittenWholeOrNotAtAll) {
    const std::vector<std::uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7};
    const std::string encoded = base64_document(bytes); // 12 digits + two quotes
    for (std::size_t capacity = 0; capacity < encoded.size(); ++capacity) {
        SCOPED_TRACE(capacity);
        const Written written =
            write_with_capacity(capacity, [&bytes](JsonWriter& w) { w.base64(bytes); });
        EXPECT_TRUE(written.overflowed);
        EXPECT_TRUE(written.text.empty());
    }
}

TEST(JsonWriter, ViewOfAFreshWriterIsEmpty) {
    JsonWriter w(std::span<char>{});
    EXPECT_TRUE(w.view().empty());
    EXPECT_FALSE(w.overflowed());
    w.begin_object();
    EXPECT_TRUE(w.overflowed()); // not even "{" fits in zero bytes
}

// ---- misuse is a programmer error ------------------------------------------------------------

TEST(JsonWriterDeathTest, KeyNeedsAnOpenObject) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_DEATH(w.key("a"), "QZ_ASSERT");
    EXPECT_DEATH(w.begin_array().key("a"), "QZ_ASSERT");
}

TEST(JsonWriterDeathTest, TwoKeysInARowAreRefused) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_DEATH(w.begin_object().key("a").key("b"), "QZ_ASSERT");
}

TEST(JsonWriterDeathTest, ObjectMembersNeedAKey) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_DEATH(w.begin_object().str("x"), "QZ_ASSERT");
    EXPECT_DEATH(w.begin_object().num(1), "QZ_ASSERT");
    EXPECT_DEATH(w.begin_object().begin_object(), "QZ_ASSERT");
    EXPECT_DEATH(w.begin_object().begin_array(), "QZ_ASSERT");
}

TEST(JsonWriterDeathTest, AKeyWithoutItsValueCannotBeClosedOver) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_DEATH(w.begin_object().key("a").end_object(), "QZ_ASSERT");
}

TEST(JsonWriterDeathTest, EndCallsMustMatchTheirBegin) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_DEATH(w.begin_object().end_array(), "QZ_ASSERT");
    EXPECT_DEATH(w.begin_array().end_object(), "QZ_ASSERT");
    EXPECT_DEATH(w.end_object(), "QZ_ASSERT");
    EXPECT_DEATH(w.end_array(), "QZ_ASSERT");
}

TEST(JsonWriterDeathTest, OnlyOneTopLevelValue) {
    std::array<char, 64> storage{};
    JsonWriter w(storage);
    EXPECT_DEATH(w.begin_object().end_object().begin_object(), "QZ_ASSERT");
    EXPECT_DEATH(w.num(1).num(2), "QZ_ASSERT");
}

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(JsonWriterDeathTest, NestingPastTheLimitIsRefused) {
    std::array<char, 128> storage{};
    JsonWriter w(storage);
    for (std::size_t i = 0; i < detail::kMaxJsonDepth; ++i) {
        w.begin_array();
    }
    EXPECT_DEATH(w.begin_array(), "QZ_ASSERT");
}

} // namespace
} // namespace qz::console
