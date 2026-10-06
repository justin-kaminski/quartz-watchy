// CRC-32 (IEEE 802.3 / zlib / PNG): known vectors, incremental property, bit-flip detection.
#include "qz/core/crc32.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace qz {
namespace {

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

/// Deterministic pseudo-random bytes (xorshift32), identical on every run.
std::vector<std::uint8_t> pattern(std::size_t size, std::uint32_t seed = 2463534242U) {
    std::vector<std::uint8_t> out(size);
    std::uint32_t x = seed;
    for (std::uint8_t& b : out) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        b = static_cast<std::uint8_t>(x & 0xFFU);
    }
    return out;
}

/// Textbook bit-at-a-time CRC-32, independent of the table implementation under test.
std::uint32_t reference_crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0) {
    std::uint32_t state = ~crc;
    for (const std::uint8_t byte : data) {
        state ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            state = ((state & 1U) != 0U) ? ((state >> 1U) ^ 0xEDB88320U) : (state >> 1U);
        }
    }
    return ~state;
}

struct TextVector {
    std::string_view text;
    std::uint32_t crc;
};

// Reference values computed with Python's zlib.crc32.
constexpr std::array<TextVector, 7> kTextVectors = {{
    {.text = "", .crc = 0x00000000U},
    {.text = "a", .crc = 0xE8B7BE43U},
    {.text = "abc", .crc = 0x352441C2U},
    {.text = "message digest", .crc = 0x20159D7FU},
    {.text = "abcdefghijklmnopqrstuvwxyz", .crc = 0x4C2750BDU},
    {.text = "123456789", .crc = 0xCBF43926U},
    {.text = "The quick brown fox jumps over the lazy dog", .crc = 0x414FA339U},
}};

TEST(Crc32, CheckValueOfTheStandardIsCbf43926) {
    EXPECT_EQ(crc32(bytes_of("123456789")), 0xCBF43926U);
}

TEST(Crc32, TextVectors) {
    for (const TextVector& v : kTextVectors) {
        EXPECT_EQ(crc32(bytes_of(v.text)), v.crc) << '"' << v.text << '"';
    }
}

TEST(Crc32, BinaryVectors) {
    const std::vector<std::uint8_t> zeros(32, 0x00);
    const std::vector<std::uint8_t> ones(32, 0xFF);
    std::vector<std::uint8_t> ascending(32);
    std::vector<std::uint8_t> descending(32);
    std::vector<std::uint8_t> all_values(256);
    for (std::size_t i = 0; i < 32; ++i) {
        ascending[i] = static_cast<std::uint8_t>(i);
        descending[i] = static_cast<std::uint8_t>(31U - i);
    }
    for (std::size_t i = 0; i < 256; ++i) {
        all_values[i] = static_cast<std::uint8_t>(i);
    }
    EXPECT_EQ(crc32(zeros), 0x190A55ADU);
    EXPECT_EQ(crc32(ones), 0xFF6CAB0BU);
    EXPECT_EQ(crc32(ascending), 0x91267E8AU);
    EXPECT_EQ(crc32(descending), 0x9AB0EF72U);
    EXPECT_EQ(crc32(all_values), 0x29058C73U);
}

TEST(Crc32, PseudoRandomVectors) {
    const std::vector<std::uint8_t> data = pattern(300);
    EXPECT_EQ(crc32(data), 0x034E54E2U);
    EXPECT_EQ(crc32(std::span(data).first(100)), 0x1E50EEDDU);
    EXPECT_EQ(crc32(std::span(data).first(7)), 0x294EDD59U);
}

TEST(Crc32, PngIendChunkChecksum) {
    // PNG chunk CRCs are CRC-32 over type + data; IEND has no data and a well-known CRC.
    EXPECT_EQ(crc32(bytes_of("IEND")), 0xAE426082U);
}

TEST(Crc32, EmptyInputReturnsTheSeedUnchanged) {
    EXPECT_EQ(crc32({}), 0U);
    for (const std::uint32_t seed : {0U, 1U, 0x12345678U, 0xCBF43926U, 0xFFFFFFFFU}) {
        EXPECT_EQ(crc32({}, seed), seed);
    }
}

TEST(Crc32, IncrementalEqualsOneShotForEverySplitPoint) {
    const std::vector<std::uint8_t> data = pattern(300);
    const std::uint32_t whole = crc32(data);
    for (std::size_t split = 0; split <= data.size(); ++split) {
        const std::uint32_t head = crc32(std::span(data).first(split));
        EXPECT_EQ(crc32(std::span(data).subspan(split), head), whole) << "split at " << split;
    }
}

TEST(Crc32, IncrementalEqualsOneShotForManyChunks) {
    const std::vector<std::uint8_t> data = pattern(1000, 99U);
    const std::uint32_t whole = crc32(data);

    // Byte by byte.
    std::uint32_t running = 0;
    for (const std::uint8_t b : data) {
        running = crc32(std::span(&b, 1), running);
    }
    EXPECT_EQ(running, whole);

    // Chunk sizes 1..37, cycling.
    running = 0;
    std::size_t offset = 0;
    std::size_t chunk = 1;
    while (offset < data.size()) {
        const std::size_t n = std::min(chunk, data.size() - offset);
        running = crc32(std::span(data).subspan(offset, n), running);
        offset += n;
        chunk = (chunk % 37U) + 1U;
    }
    EXPECT_EQ(running, whole);
}

TEST(Crc32, MatchesTheBitwiseReferenceForAllLengthsAndAlignments) {
    const std::vector<std::uint8_t> data = pattern(200, 7U);
    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t length = 0; length <= 130; ++length) {
            const auto window = std::span(data).subspan(offset, length);
            EXPECT_EQ(crc32(window), reference_crc32(window))
                << "offset " << offset << " length " << length;
        }
    }
}

TEST(Crc32, MatchesTheBitwiseReferenceWithAnArbitrarySeed) {
    const std::vector<std::uint8_t> data = pattern(64, 5U);
    for (const std::uint32_t seed : {1U, 0xDEADBEEFU, 0x80000000U}) {
        EXPECT_EQ(crc32(data, seed), reference_crc32(data, seed));
    }
}

TEST(Crc32, LargeBufferMatchesTheReference) {
    const std::vector<std::uint8_t> data = pattern(70000, 12345U);
    EXPECT_EQ(crc32(data), reference_crc32(data));
}

TEST(Crc32, EverySingleBitFlipChangesTheChecksum) {
    // RTC-state validation relies on this: a CRC-32 detects every single-bit error.
    std::vector<std::uint8_t> data = pattern(300, 31U);
    const std::uint32_t original = crc32(data);
    for (std::size_t byte = 0; byte < data.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint8_t>(1U << bit);
            data[byte] ^= mask;
            EXPECT_NE(crc32(data), original) << "byte " << byte << " bit " << bit;
            data[byte] ^= mask;
        }
    }
}

TEST(Crc32, DependsOnByteOrder) {
    EXPECT_NE(crc32(bytes_of("ab")), crc32(bytes_of("ba")));
}

} // namespace
} // namespace qz
