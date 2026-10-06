// FakeRtcMemory: two RTC_NOINIT-like byte regions that persist, plus power-loss noise.
#include "qz/core/crc32.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/system.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace qz::testkit {
namespace {

constexpr std::size_t kStateBytes = 2048;
constexpr std::size_t kFrameBytes = 5120;

bool all_zero(std::span<const std::uint8_t> bytes) {
    return std::ranges::all_of(bytes, [](std::uint8_t b) { return b == 0; });
}

std::size_t longest_zero_run(std::span<const std::uint8_t> bytes) {
    std::size_t best = 0;
    std::size_t run = 0;
    for (const std::uint8_t b : bytes) {
        run = (b == 0) ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}

TEST(FakeRtcMemory, RegionsHaveTheDocumentedSizes) {
    FakeRtcMemory memory;
    EXPECT_EQ(memory.state_region().size(), kStateBytes);
    EXPECT_EQ(memory.frame_region().size(), kFrameBytes);
    // FrameShadow = u32 magic + u32 crc + the 1 bpp frame: it must fit (ARCHITECTURE.md section 6).
    EXPECT_GE(memory.frame_region().size(), 8 + sizeof(gfx::Framebuffer));
}

TEST(FakeRtcMemory, StartsZeroed) {
    FakeRtcMemory memory;
    EXPECT_TRUE(all_zero(memory.state_region()));
    EXPECT_TRUE(all_zero(memory.frame_region()));
}

TEST(FakeRtcMemory, ContentPersistsAcrossAccessesAndRegionsAreDisjoint) {
    FakeRtcMemory memory;
    hal::RtcMemory& rtc = memory;
    std::ranges::fill(rtc.state_region(), std::uint8_t{0xA5});
    std::ranges::fill(rtc.frame_region(), std::uint8_t{0x5A});

    const std::span<std::uint8_t> state_again = rtc.state_region();
    const std::span<std::uint8_t> frame_again = rtc.frame_region();
    EXPECT_EQ(state_again.data(), memory.state_region().data()) << "stable address";
    EXPECT_TRUE(std::ranges::all_of(state_again, [](std::uint8_t b) { return b == 0xA5; }));
    EXPECT_TRUE(std::ranges::all_of(frame_again, [](std::uint8_t b) { return b == 0x5A; }));

    const std::uint8_t* const state_end = state_again.data() + state_again.size();
    const std::uint8_t* const frame_end = frame_again.data() + frame_again.size();
    EXPECT_TRUE(state_end <= frame_again.data() || frame_end <= state_again.data());
}

TEST(FakeRtcMemory, TwoInstancesDoNotShareStorage) {
    FakeRtcMemory a;
    FakeRtcMemory b;
    a.state_region()[0] = 0x11;
    EXPECT_EQ(b.state_region()[0], 0);
}

TEST(FakeRtcMemory, RegionsAreAlignedForStructViews) {
    // RtcState/FrameShadow hold 64-bit members and are viewed in place on the device.
    FakeRtcMemory memory;
    EXPECT_EQ(std::bit_cast<std::uintptr_t>(memory.state_region().data()) % 16U, 0U);
    EXPECT_EQ(std::bit_cast<std::uintptr_t>(memory.frame_region().data()) % 16U, 0U);
}

TEST(FakeRtcMemory, ScrambleFillsBothRegionsEntirely) {
    FakeRtcMemory memory;
    memory.scramble();
    EXPECT_FALSE(all_zero(memory.state_region()));
    EXPECT_FALSE(all_zero(memory.frame_region()));
    // Noise everywhere, including the tails: no long run of untouched zero bytes.
    EXPECT_LT(longest_zero_run(memory.state_region()), 16U);
    EXPECT_LT(longest_zero_run(memory.frame_region()), 16U);
}

TEST(FakeRtcMemory, ScrambleOverwritesExistingContent) {
    FakeRtcMemory memory;
    std::ranges::fill(memory.state_region(), std::uint8_t{0xFF});
    std::ranges::fill(memory.frame_region(), std::uint8_t{0xFF});
    memory.scramble();
    EXPECT_FALSE(
        std::ranges::all_of(memory.state_region(), [](std::uint8_t b) { return b == 0xFF; }));
    EXPECT_FALSE(
        std::ranges::all_of(memory.frame_region(), [](std::uint8_t b) { return b == 0xFF; }));
}

TEST(FakeRtcMemory, ScrambleIsDeterministicPerInstanceAndDiffersBetweenCalls) {
    FakeRtcMemory a;
    FakeRtcMemory b;
    a.scramble();
    b.scramble();
    EXPECT_TRUE(std::ranges::equal(a.state_region(), b.state_region()));
    EXPECT_TRUE(std::ranges::equal(a.frame_region(), b.frame_region()));

    const std::vector<std::uint8_t> first(a.state_region().begin(), a.state_region().end());
    a.scramble(); // a second power loss gives different garbage
    EXPECT_FALSE(std::ranges::equal(first, a.state_region()));
}

void expect_random_looking(std::span<const std::uint8_t> region) {
    std::array<std::size_t, 256> histogram{};
    std::size_t set_bits = 0;
    for (const std::uint8_t b : region) {
        ++histogram[b];
        set_bits += static_cast<std::size_t>(std::popcount(b));
    }
    // Almost every byte value occurs (a random 2 KiB misses ~0.1 of 256 values on average), and
    // the bits are balanced within 2 %. The seed is fixed, so this either always holds or never.
    const std::size_t distinct =
        histogram.size() - static_cast<std::size_t>(std::ranges::count(histogram, std::size_t{0}));
    EXPECT_GE(distinct, 240U) << "region of " << region.size() << " bytes";
    const std::size_t percent_set = set_bits * 100U / (region.size() * 8U);
    EXPECT_GE(percent_set, 48U) << "region of " << region.size() << " bytes";
    EXPECT_LE(percent_set, 52U) << "region of " << region.size() << " bytes";
}

TEST(FakeRtcMemory, ScrambledBytesLookRandom) {
    FakeRtcMemory memory;
    memory.scramble();
    expect_random_looking(memory.state_region());
    expect_random_looking(memory.frame_region());
}

TEST(FakeRtcMemory, ScrambleInvalidatesAChecksummedBlock) {
    // The way RtcStore validates state: a stored CRC over the payload.
    FakeRtcMemory memory;
    const std::span<std::uint8_t> state = memory.state_region();
    std::ranges::iota(state, std::uint8_t{0});
    const std::uint32_t before = crc32(state.subspan(4));
    memory.scramble();
    EXPECT_NE(crc32(state.subspan(4)), before);
}

} // namespace
} // namespace qz::testkit
