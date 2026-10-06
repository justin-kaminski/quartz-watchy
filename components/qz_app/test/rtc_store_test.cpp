// RtcStore (qz/app/rtc_state.hpp): layout pin, validation, corruption detection, commit.
#include "qz/app/rtc_state.hpp"
#include "qz/core/crc32.hpp"
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace qz::app {
namespace {

// ---- layout pin (ARCHITECTURE.md section 6): change = bump kRtcStateVersion ----
static_assert(std::is_trivially_copyable_v<RtcState> && std::is_standard_layout_v<RtcState>);
static_assert(std::is_trivially_copyable_v<FrameShadow> && std::is_standard_layout_v<FrameShadow>);
static_assert(kRtcStateVersion == 1, "layout pinned below belongs to version 1");

static_assert(sizeof(RtcHeader) == 16);
static_assert(offsetof(RtcHeader, magic) == 0 && offsetof(RtcHeader, version) == 4 &&
              offsetof(RtcHeader, size) == 6 && offsetof(RtcHeader, crc32) == 8 &&
              offsetof(RtcHeader, boot_count) == 12);

static_assert(sizeof(time::TimeKeeperState) == 48);
static_assert(sizeof(steps::StepState) == 96);
static_assert(sizeof(power::PowerState) == 72);
static_assert(sizeof(conn::ConnState) == 40);
static_assert(sizeof(model::WeatherReport) == 24);
static_assert(sizeof(DisplayState) == 16);
static_assert(sizeof(WakeTiming) == 24);
static_assert(sizeof(settings::Settings) == 168);
static_assert(sizeof(RingBuffer<model::WakeRecord, kWakeLogCapacity>) == 516);

static_assert(sizeof(RtcState) == 1024);
static_assert(offsetof(RtcState, header) == 0);
static_assert(offsetof(RtcState, time) == 16);
static_assert(offsetof(RtcState, steps) == 64);
static_assert(offsetof(RtcState, power) == 160);
static_assert(offsetof(RtcState, conn) == 232);
static_assert(offsetof(RtcState, weather) == 272);
static_assert(offsetof(RtcState, display) == 296);
static_assert(offsetof(RtcState, wake) == 312);
static_assert(offsetof(RtcState, settings) == 336);
static_assert(offsetof(RtcState, settings_valid) == 504);
static_assert(offsetof(RtcState, face_id) == 505);
static_assert(offsetof(RtcState, reserved) == 506);
static_assert(offsetof(RtcState, wake_log) == 508);

static_assert(sizeof(FrameShadow) == 8 + gfx::kFrameBytes);
static_assert(offsetof(FrameShadow, magic) == 0 && offsetof(FrameShadow, crc32) == 4 &&
              offsetof(FrameShadow, frame) == 8);

// ARCHITECTURE.md section 6: total RTC use <= 7.5 KiB of the 8 KiB slow memory.
static_assert(sizeof(RtcState) + sizeof(FrameShadow) <= 7680);

constexpr std::size_t kStateBytes = sizeof(RtcState);
constexpr std::size_t kFrameBytes = sizeof(FrameShadow);
constexpr std::size_t kCrcFieldOffset = offsetof(RtcHeader, crc32);

/// Deterministic xorshift64* byte source.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed != 0 ? seed : 1) {}
    std::uint64_t next() {
        s_ ^= s_ >> 12U;
        s_ ^= s_ << 25U;
        s_ ^= s_ >> 27U;
        return s_ * 0x2545F4914F6CDD1DULL;
    }
    std::uint8_t byte() { return static_cast<std::uint8_t>(next() >> 56U); }

private:
    std::uint64_t s_;
};

/// A state whose every byte (padding included) is non-trivial pseudo-random content: the
/// strongest input for "each byte matters". Only ever memcpy'd, never interpreted.
RtcState noisy_state(std::uint64_t seed) {
    RtcState s{};
    Rng rng(seed);
    auto* raw = reinterpret_cast<std::uint8_t*>(&s);
    for (std::size_t i = 0; i < sizeof(s); ++i) {
        raw[i] = rng.byte();
    }
    return s;
}

gfx::Framebuffer noisy_frame(std::uint64_t seed) {
    gfx::Framebuffer f;
    Rng rng(seed);
    for (auto& b : f.bits) {
        b = rng.byte();
    }
    return f;
}

std::span<const std::uint8_t> bytes_of(const RtcState& s) {
    return {reinterpret_cast<const std::uint8_t*>(&s), sizeof(s)};
}

template<class T>
void poke(std::span<std::uint8_t> region, std::size_t offset, T value) {
    std::memcpy(region.data() + offset, &value, sizeof(T));
}

class RtcStoreTest : public ::testing::Test {
protected:
    testkit::FakeRtcMemory mem_;
    RtcStore store_{mem_.state_region(), mem_.frame_region()};

    std::span<std::uint8_t> state_image() { return mem_.state_region().first(kStateBytes); }
    std::span<std::uint8_t> frame_image() { return mem_.frame_region().first(kFrameBytes); }
};

TEST(RtcLayout, FitsTheFakeRegionsAndBudget) {
    testkit::FakeRtcMemory mem;
    EXPECT_GE(mem.state_region().size(), sizeof(RtcState));
    EXPECT_GE(mem.frame_region().size(), sizeof(FrameShadow));
    EXPECT_LE(sizeof(RtcState) + sizeof(FrameShadow), 7680U);
}

TEST(RtcLayout, MagicsAreAsciiTags) {
    EXPECT_EQ(kRtcStateMagic, 0x53525A51U); // bytes 'Q' 'Z' 'R' 'S'
    EXPECT_EQ(kFrameShadowMagic, 0x46525A51U);
    EXPECT_NE(kRtcStateMagic, kFrameShadowMagic);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(RtcStoreTest, ScrambledMemoryIsNeverAccepted) {
    for (int i = 0; i < 2000; ++i) {
        mem_.scramble();
        RtcState out = noisy_state(7);
        const RtcState before = out;
        const auto st = store_.load(out);
        ASSERT_FALSE(st) << "iteration " << i;
        EXPECT_EQ(st.error().code, Errc::kCorrupt);
        // Byte-exact "untouched" check on purpose.
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
        EXPECT_EQ(std::memcmp(&before, &out, sizeof(out)), 0) << "out must stay untouched";
        gfx::Framebuffer frame;
        const auto fst = store_.load_frame(frame);
        ASSERT_FALSE(fst);
        EXPECT_EQ(fst.error().code, Errc::kCorrupt);
    }
}

TEST_F(RtcStoreTest, ZeroedAndFreshMemoryIsCorrupt) {
    RtcState out{};
    EXPECT_EQ(store_.load(out).error().code, Errc::kCorrupt);
    gfx::Framebuffer f;
    EXPECT_EQ(store_.load_frame(f).error().code, Errc::kCorrupt);
}

TEST_F(RtcStoreTest, CommitLoadRoundTripKeepsEveryPayloadByte) {
    RtcState const s = noisy_state(1);
    store_.commit(s);
    RtcState out{};
    ASSERT_TRUE(store_.load(out));
    // Header is rewritten by commit; boot_count and the whole payload come back verbatim.
    EXPECT_EQ(out.header.magic, kRtcStateMagic);
    EXPECT_EQ(out.header.version, kRtcStateVersion);
    EXPECT_EQ(out.header.size, sizeof(RtcState));
    EXPECT_EQ(out.header.boot_count, s.header.boot_count);
    EXPECT_EQ(std::memcmp(bytes_of(out).data() + sizeof(RtcHeader),
                          bytes_of(s).data() + sizeof(RtcHeader),
                          sizeof(RtcState) - sizeof(RtcHeader)),
              0);
}

TEST_F(RtcStoreTest, DefaultStateRoundTrips) {
    RtcState const s{};
    store_.commit(s);
    RtcState out = noisy_state(2);
    ASSERT_TRUE(store_.load(out));
    EXPECT_EQ(out.wake.ewma_latency_us, 350'000);
    EXPECT_EQ(out.header.boot_count, 0U);
    EXPECT_EQ(out.wake_log.size(), 0U);
}

TEST_F(RtcStoreTest, CommitRecomputesCrcAndIgnoresCallerHeader) {
    RtcState s = noisy_state(3);
    s.header.magic = 0xDEADBEEF;
    s.header.version = 0x7777;
    s.header.size = 5;
    s.header.crc32 = 0x12345678;
    s.header.boot_count = 41;
    store_.commit(s);
    RtcState out{};
    ASSERT_TRUE(store_.load(out));
    EXPECT_EQ(out.header.boot_count, 41U);

    std::uint32_t stored_crc = 0;
    std::memcpy(&stored_crc, state_image().data() + kCrcFieldOffset, 4);
    const auto image = state_image();
    EXPECT_EQ(stored_crc, crc32(image.subspan(offsetof(RtcHeader, boot_count))));
    EXPECT_NE(stored_crc, 0x12345678U);

    // A second commit of a changed state moves the CRC and stays valid.
    s.power.filtered_mv = static_cast<std::uint16_t>(s.power.filtered_mv + 1);
    s.header.boot_count = 42;
    store_.commit(s);
    std::uint32_t crc2 = 0;
    std::memcpy(&crc2, state_image().data() + kCrcFieldOffset, 4);
    EXPECT_NE(crc2, stored_crc);
    RtcState out2{};
    ASSERT_TRUE(store_.load(out2));
    EXPECT_EQ(out2.power.filtered_mv, s.power.filtered_mv);
    EXPECT_EQ(out2.header.boot_count, 42U);
}

TEST_F(RtcStoreTest, CommitReplacesACorruptRegionCompletely) {
    mem_.scramble();
    RtcState const s = noisy_state(4);
    store_.commit(s);
    RtcState out{};
    EXPECT_TRUE(store_.load(out));
}

TEST_F(RtcStoreTest, ModifyingTheRegionWithoutCommitIsDetected) {
    RtcState const s = noisy_state(5);
    store_.commit(s);
    state_image()[200] ^= 0x01U; // "someone wrote RTC memory behind the store's back"
    RtcState out{};
    const auto st = store_.load(out);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code, Errc::kCorrupt);
    EXPECT_EQ(st.error().detail, static_cast<std::uint16_t>(RtcCorruptReason::kCrc));
}

TEST_F(RtcStoreTest, EverySingleCorruptedByteIsRejected) {
    const RtcState s = noisy_state(6);
    store_.commit(s);
    RtcState sentinel = noisy_state(99);
    const RtcState sentinel_copy = sentinel;
    constexpr std::array<std::uint8_t, 4> kMasks = {0xFF, 0x01, 0x80, 0x5A};
    ASSERT_TRUE(store_.load(sentinel)); // sanity: pristine image loads
    sentinel = sentinel_copy;
    for (std::size_t i = 0; i < kStateBytes; ++i) {
        for (const std::uint8_t mask : kMasks) {
            state_image()[i] ^= mask;
            const auto st = store_.load(sentinel);
            ASSERT_FALSE(st) << "byte " << i << " mask " << static_cast<int>(mask);
            EXPECT_EQ(st.error().code, Errc::kCorrupt);
            // Byte-exact "untouched" check on purpose.
            // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
            ASSERT_EQ(std::memcmp(&sentinel, &sentinel_copy, sizeof(sentinel)), 0)
                << "out modified on a rejected load, byte " << i;
            state_image()[i] ^= mask;
        }
    }
    RtcState out{};
    EXPECT_TRUE(store_.load(out)); // restored image is valid again
}

TEST_F(RtcStoreTest, ByteSwapsAndBurstsAreRejected) {
    const RtcState s = noisy_state(8);
    store_.commit(s);
    Rng rng(123);
    std::vector<std::uint8_t> pristine(state_image().begin(), state_image().end());
    for (int trial = 0; trial < 3000; ++trial) {
        std::ranges::copy(pristine, state_image().begin());
        const auto n = 2U + static_cast<unsigned>(rng.next() % 6U);
        bool changed = false;
        for (unsigned k = 0; k < n; ++k) {
            const std::size_t at =
                sizeof(RtcHeader) + (rng.next() % (kStateBytes - sizeof(RtcHeader)));
            const std::uint8_t v = rng.byte();
            changed = changed || (state_image()[at] != v);
            state_image()[at] = v;
        }
        if (!changed) {
            continue;
        }
        RtcState out{};
        ASSERT_FALSE(store_.load(out)) << "trial " << trial;
    }
}

TEST_F(RtcStoreTest, BytesBeyondTheImageAreIgnored) {
    // The region may be larger than the struct (fake: 2 KiB); the tail is not part of the image.
    const RtcState s = noisy_state(9);
    store_.commit(s);
    ASSERT_GT(mem_.state_region().size(), kStateBytes);
    for (std::size_t i = kStateBytes; i < mem_.state_region().size(); ++i) {
        mem_.state_region()[i] ^= 0xFFU;
    }
    RtcState out{};
    EXPECT_TRUE(store_.load(out));
}

TEST_F(RtcStoreTest, MagicVersionAndSizeMismatchAreReportedSeparately) {
    const RtcState s = noisy_state(10);
    store_.commit(s);
    const std::vector<std::uint8_t> pristine(state_image().begin(), state_image().end());
    const auto restore = [&] {
        std::ranges::copy(pristine, state_image().begin());
    };
    const auto expect = [&](RtcCorruptReason why) {
        RtcState out{};
        const auto st = store_.load(out);
        ASSERT_FALSE(st);
        EXPECT_EQ(st.error().code, Errc::kCorrupt);
        EXPECT_EQ(st.error().detail, static_cast<std::uint16_t>(why));
    };

    poke<std::uint32_t>(state_image(), offsetof(RtcHeader, magic), kFrameShadowMagic);
    expect(RtcCorruptReason::kMagic);
    restore();

    poke<std::uint16_t>(state_image(), offsetof(RtcHeader, version), kRtcStateVersion + 1);
    expect(RtcCorruptReason::kVersion);
    poke<std::uint16_t>(state_image(), offsetof(RtcHeader, version), 0);
    expect(RtcCorruptReason::kVersion);
    restore();

    // An image written by a smaller or larger layout of the same version number.
    poke<std::uint16_t>(state_image(), offsetof(RtcHeader, size), sizeof(RtcState) - 4);
    expect(RtcCorruptReason::kSize);
    poke<std::uint16_t>(state_image(), offsetof(RtcHeader, size), sizeof(RtcState) + 4);
    expect(RtcCorruptReason::kSize);
    restore();

    RtcState out{};
    EXPECT_TRUE(store_.load(out));
}

TEST_F(RtcStoreTest, CrcFieldItselfIsChecked) {
    const RtcState s = noisy_state(11);
    store_.commit(s);
    for (std::size_t i = 0; i < 4; ++i) {
        state_image()[kCrcFieldOffset + i] ^= 0x10U;
        RtcState out{};
        const auto st = store_.load(out);
        ASSERT_FALSE(st);
        EXPECT_EQ(st.error().detail, static_cast<std::uint16_t>(RtcCorruptReason::kCrc));
        state_image()[kCrcFieldOffset + i] ^= 0x10U;
    }
}

TEST_F(RtcStoreTest, BootCountIsProtectedByTheCrc) {
    RtcState s = noisy_state(12);
    s.header.boot_count = 1000;
    store_.commit(s);
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        state_image()[offsetof(RtcHeader, boot_count) + i] ^= 0x01U;
        RtcState out{};
        ASSERT_FALSE(store_.load(out)) << "boot_count byte " << i;
        state_image()[offsetof(RtcHeader, boot_count) + i] ^= 0x01U;
    }
}

TEST(RtcStoreRegions, TooSmallRegionsAreCorruptNotOutOfBounds) {
    std::array<std::uint8_t, kStateBytes - 1> small_state{};
    std::array<std::uint8_t, kFrameBytes - 1> small_frame{};
    RtcStore store(small_state, small_frame);
    RtcState out{};
    const auto st = store.load(out);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code, Errc::kCorrupt);
    EXPECT_EQ(st.error().detail, static_cast<std::uint16_t>(RtcCorruptReason::kRegionTooSmall));
    gfx::Framebuffer f;
    EXPECT_FALSE(store.load_frame(f));
    store.invalidate(); // must not write past the end (ASan)

    RtcStore empty({}, {});
    EXPECT_FALSE(empty.load(out));
    EXPECT_FALSE(empty.load_frame(f));
    empty.invalidate();
}

TEST(RtcStoreRegionsDeathTest, CommitIntoTooSmallRegionIsAProgrammerError) {
    std::array<std::uint8_t, kStateBytes - 1> small_state{};
    std::array<std::uint8_t, kFrameBytes> frame{};
    RtcStore store(small_state, frame);
    const RtcState s{};
    EXPECT_DEATH(store.commit(s), "");
    std::array<std::uint8_t, kStateBytes> state{};
    std::array<std::uint8_t, kFrameBytes - 1> small_frame{};
    RtcStore store2(state, small_frame);
    EXPECT_DEATH(store2.commit_frame(gfx::Framebuffer{}), "");
}

TEST(RtcStoreRegions, MisalignedRegionsWork) {
    // RTC memory alignment is not assumed: UBSan's alignment check must stay quiet.
    std::vector<std::uint8_t> state_buf(kStateBytes + 3);
    std::vector<std::uint8_t> frame_buf(kFrameBytes + 3);
    const std::span<std::uint8_t> state_span(state_buf.data() + 1, kStateBytes);
    const std::span<std::uint8_t> frame_span(frame_buf.data() + 3, kFrameBytes);
    RtcStore store{state_span, frame_span};
    const RtcState s = noisy_state(13);
    store.commit(s);
    RtcState out{};
    ASSERT_TRUE(store.load(out));
    EXPECT_EQ(out.header.boot_count, s.header.boot_count);
    const gfx::Framebuffer f = noisy_frame(14);
    store.commit_frame(f);
    gfx::Framebuffer g;
    ASSERT_TRUE(store.load_frame(g));
    EXPECT_EQ(g.bits, f.bits);
}

TEST_F(RtcStoreTest, InvalidateKillsBothImagesAndKeepsNeighbourBytes) {
    RtcState const s = noisy_state(15);
    store_.commit(s);
    store_.commit_frame(noisy_frame(16));
    store_.invalidate();
    RtcState out{};
    const auto st = store_.load(out);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().code, Errc::kCorrupt);
    gfx::Framebuffer f;
    ASSERT_FALSE(store_.load_frame(f));
    // And the regions are reusable.
    store_.commit(s);
    EXPECT_TRUE(store_.load(out));
}

// ---- frame shadow ----

TEST_F(RtcStoreTest, FrameRoundTrip) {
    const gfx::Framebuffer f = noisy_frame(20);
    store_.commit_frame(f);
    gfx::Framebuffer out;
    ASSERT_TRUE(store_.load_frame(out));
    EXPECT_EQ(out.bits, f.bits);
    EXPECT_EQ(out.crc32(), f.crc32());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_F(RtcStoreTest, EveryByteOfTheFrameShadowIsProtected) {
    const gfx::Framebuffer f = noisy_frame(21);
    store_.commit_frame(f);
    gfx::Framebuffer sentinel = noisy_frame(22);
    const gfx::Framebuffer sentinel_copy = sentinel;
    // 5008 bytes x 3 masks: exhaustive (the budget allows a bounded sample; we do not need it).
    for (std::size_t i = 0; i < kFrameBytes; ++i) {
        for (const std::uint8_t mask :
             {std::uint8_t{0xFF}, std::uint8_t{0x01}, std::uint8_t{0x80}}) {
            frame_image()[i] ^= mask;
            const auto st = store_.load_frame(sentinel);
            ASSERT_FALSE(st) << "byte " << i;
            ASSERT_EQ(st.error().code, Errc::kCorrupt);
            ASSERT_EQ(sentinel.bits, sentinel_copy.bits) << "out modified, byte " << i;
            frame_image()[i] ^= mask;
        }
    }
    gfx::Framebuffer out;
    EXPECT_TRUE(store_.load_frame(out));
}

TEST_F(RtcStoreTest, FrameMagicAndCrcMismatchAreDistinguished) {
    store_.commit_frame(noisy_frame(23));
    gfx::Framebuffer out;
    poke<std::uint32_t>(frame_image(), offsetof(FrameShadow, magic), kRtcStateMagic);
    auto st = store_.load_frame(out);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().detail, static_cast<std::uint16_t>(RtcCorruptReason::kMagic));
    poke<std::uint32_t>(frame_image(), offsetof(FrameShadow, magic), kFrameShadowMagic);
    frame_image()[offsetof(FrameShadow, frame) + 10] ^= 1U;
    st = store_.load_frame(out);
    ASSERT_FALSE(st);
    EXPECT_EQ(st.error().detail, static_cast<std::uint16_t>(RtcCorruptReason::kCrc));
}

TEST_F(RtcStoreTest, StateAndFrameFailIndependently) {
    const RtcState s = noisy_state(24);
    store_.commit(s);
    store_.commit_frame(noisy_frame(25));
    frame_image()[100] ^= 0xFFU;
    RtcState out{};
    EXPECT_TRUE(store_.load(out)) << "a bad frame shadow must not cost the state";
    gfx::Framebuffer f;
    EXPECT_FALSE(store_.load_frame(f));
    store_.commit_frame(noisy_frame(26));
    state_image()[300] ^= 0xFFU;
    EXPECT_FALSE(store_.load(out));
    EXPECT_TRUE(store_.load_frame(f)) << "a bad state must not cost the frame shadow";
}

TEST_F(RtcStoreTest, FrameCommitReplacesCorruptRegionAndSecondCommitWins) {
    mem_.scramble();
    const gfx::Framebuffer a = noisy_frame(27);
    const gfx::Framebuffer b = noisy_frame(28);
    store_.commit_frame(a);
    store_.commit_frame(b);
    gfx::Framebuffer out;
    ASSERT_TRUE(store_.load_frame(out));
    EXPECT_EQ(out.bits, b.bits);
}

} // namespace
} // namespace qz::app
