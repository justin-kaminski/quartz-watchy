// StaticVector and RingBuffer: heap-free fixed-capacity containers (containers.hpp).
#include "qz/core/containers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <type_traits>
#include <vector>

namespace qz {
namespace {

struct Item {
    std::int32_t id = -1;
    std::uint16_t tag = 0;
    constexpr bool operator==(const Item&) const noexcept = default;
};

template<class Buffer>
std::vector<int> contents_of(const Buffer& buffer) {
    std::vector<int> out;
    out.reserve(buffer.size());
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        out.push_back(buffer[i]);
    }
    return out;
}

// --- StaticVector -------------------------------------------------------------------------------

TEST(StaticVector, StartsEmpty) {
    const StaticVector<int, 4> v;
    EXPECT_EQ(v.size(), 0U);
    EXPECT_TRUE(v.empty());
    EXPECT_FALSE(v.full());
    EXPECT_EQ((StaticVector<int, 4>::capacity()), 4U);
    EXPECT_EQ(v.begin(), v.end());
    EXPECT_TRUE(v.span().empty());
}

TEST(StaticVector, PushBackFillsToCapacityThenRefuses) {
    StaticVector<int, 3> v;
    EXPECT_TRUE(v.push_back(10));
    EXPECT_TRUE(v.push_back(20));
    EXPECT_FALSE(v.full());
    EXPECT_TRUE(v.push_back(30));
    EXPECT_TRUE(v.full());
    EXPECT_FALSE(v.push_back(40));
    EXPECT_FALSE(v.push_back(50));
    ASSERT_EQ(v.size(), 3U);
    EXPECT_EQ(v[0], 10);
    EXPECT_EQ(v[1], 20);
    EXPECT_EQ(v[2], 30);
}

TEST(StaticVector, IndexingIsMutableAndConstCorrect) {
    StaticVector<Item, 4> v;
    EXPECT_TRUE(v.push_back(Item{.id = 1, .tag = 2}));
    v[0].tag = 99;
    const StaticVector<Item, 4>& cv = v;
    EXPECT_EQ(cv[0], (Item{.id = 1, .tag = 99}));
}

TEST(StaticVector, IterationVisitsOnlyLiveElementsInOrder) {
    StaticVector<int, 8> v;
    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(v.push_back(i * i));
    }
    std::vector<int> seen;
    for (const int x : v) {
        seen.push_back(x);
    }
    EXPECT_EQ(seen, (std::vector<int>{1, 4, 9, 16, 25}));
    for (int& x : v) {
        x += 1;
    }
    EXPECT_EQ(v[4], 26);
    EXPECT_EQ(std::accumulate(v.begin(), v.end(), 0), 2 + 5 + 10 + 17 + 26);
}

TEST(StaticVector, SpanViewsTheLiveElements) {
    StaticVector<std::uint8_t, 6> v;
    EXPECT_TRUE(v.push_back(7));
    EXPECT_TRUE(v.push_back(8));
    const std::span<const std::uint8_t> s = v.span();
    ASSERT_EQ(s.size(), 2U);
    EXPECT_EQ(s[0], 7);
    EXPECT_EQ(s[1], 8);
    EXPECT_EQ(s.data(), v.begin());
}

TEST(StaticVector, PopBackRemovesTheNewest) {
    StaticVector<int, 3> v;
    EXPECT_TRUE(v.push_back(1));
    EXPECT_TRUE(v.push_back(2));
    v.pop_back();
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0], 1);
    v.pop_back();
    EXPECT_TRUE(v.empty());
    EXPECT_TRUE(v.push_back(3));
    EXPECT_EQ(v[0], 3);
}

TEST(StaticVector, ClearEmptiesAndAllowsReuse) {
    StaticVector<int, 2> v;
    EXPECT_TRUE(v.push_back(1));
    EXPECT_TRUE(v.push_back(2));
    EXPECT_TRUE(v.full());
    v.clear();
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0U);
    EXPECT_TRUE(v.push_back(5));
    EXPECT_EQ(v[0], 5);
}

TEST(StaticVector, CopiesAreIndependent) {
    StaticVector<int, 4> a;
    EXPECT_TRUE(a.push_back(1));
    EXPECT_TRUE(a.push_back(2));
    StaticVector<int, 4> b = a;
    b[0] = 100;
    EXPECT_TRUE(b.push_back(3));
    EXPECT_EQ(a.size(), 2U);
    EXPECT_EQ(a[0], 1);
    EXPECT_EQ(b.size(), 3U);
    EXPECT_EQ(b[0], 100);
}

TEST(StaticVector, TriviallyCopyableElementsGiveATriviallyCopyableContainer) {
    // Required for placement in RTC memory.
    static_assert(std::is_trivially_copyable_v<StaticVector<std::uint16_t, 8>>);
    static_assert(std::is_trivially_copyable_v<StaticVector<Item, 3>>);
    StaticVector<Item, 3> a;
    EXPECT_TRUE(a.push_back(Item{.id = 5, .tag = 6}));
    StaticVector<Item, 3> b;
    std::memcpy(&b, &a, sizeof(a));
    ASSERT_EQ(b.size(), 1U);
    EXPECT_EQ(b[0], (Item{.id = 5, .tag = 6}));
}

TEST(StaticVector, ZeroCapacityIsAlwaysEmptyAndFull) {
    StaticVector<int, 0> v;
    EXPECT_TRUE(v.empty());
    EXPECT_TRUE(v.full());
    EXPECT_FALSE(v.push_back(1));
    EXPECT_EQ(v.begin(), v.end());
}

TEST(StaticVector, WorksInConstantExpressions) {
    constexpr auto build = [] {
        StaticVector<int, 4> v;
        (void)v.push_back(3); // capacity is 4: cannot fail
        (void)v.push_back(4);
        v.pop_back();
        return v;
    };
    constexpr auto v = build();
    static_assert(v.size() == 1 && v[0] == 3);
    SUCCEED();
}

TEST(StaticVectorDeathTest, IndexAtSizeAborts) {
    StaticVector<int, 4> v;
    EXPECT_TRUE(v.push_back(1));
    EXPECT_DEATH(static_cast<void>(v[1]), "QZ_ASSERT.*i < size_");
}

TEST(StaticVectorDeathTest, ConstIndexBeyondSizeAborts) {
    const StaticVector<int, 4> v;
    EXPECT_DEATH(static_cast<void>(v[0]), "QZ_ASSERT.*i < size_");
}

TEST(StaticVectorDeathTest, PopBackOnEmptyAborts) {
    StaticVector<int, 4> v;
    EXPECT_DEATH(v.pop_back(), "QZ_ASSERT.*size_ > 0");
}

// --- RingBuffer ---------------------------------------------------------------------------------

TEST(RingBuffer, StartsEmpty) {
    const RingBuffer<int, 4> r;
    EXPECT_EQ(r.size(), 0U);
    EXPECT_TRUE(r.empty());
    EXPECT_EQ((RingBuffer<int, 4>::capacity()), 4U);
}

TEST(RingBuffer, IndexZeroIsOldestAndNewestIsTheLastPush) {
    RingBuffer<int, 4> r;
    r.push(10);
    EXPECT_EQ(r.size(), 1U);
    EXPECT_EQ(r[0], 10);
    EXPECT_EQ(r.newest(), 10);
    r.push(20);
    r.push(30);
    ASSERT_EQ(r.size(), 3U);
    EXPECT_EQ(r[0], 10);
    EXPECT_EQ(r[1], 20);
    EXPECT_EQ(r[2], 30);
    EXPECT_EQ(r.newest(), 30);
    EXPECT_FALSE(r.empty());
}

TEST(RingBuffer, PushingPastCapacityOverwritesTheOldest) {
    RingBuffer<int, 4> r;
    for (int i = 1; i <= 4; ++i) {
        r.push(i);
    }
    EXPECT_EQ(contents_of(r), (std::vector<int>{1, 2, 3, 4}));

    r.push(5);
    EXPECT_EQ(contents_of(r), (std::vector<int>{2, 3, 4, 5}));

    for (int i = 6; i <= 11; ++i) {
        r.push(i);
    }
    EXPECT_EQ(contents_of(r), (std::vector<int>{8, 9, 10, 11}));
    EXPECT_EQ(r.newest(), 11);
}

TEST(RingBuffer, EveryRotationOfTheHeadKeepsOrder) {
    // Walk the write position through every slot, several times round.
    RingBuffer<int, 5> r;
    std::vector<int> expected;
    for (int value = 0; value < ((3 * 5) + 2); ++value) {
        r.push(value);
        expected.push_back(value);
        if (expected.size() > 5) {
            expected.erase(expected.begin());
        }
        ASSERT_EQ(contents_of(r), expected) << "after pushing " << value;
        EXPECT_EQ(r.newest(), value);
    }
}

TEST(RingBuffer, CapacityOfOneKeepsOnlyTheLastValue) {
    RingBuffer<int, 1> r;
    r.push(1);
    r.push(2);
    r.push(3);
    ASSERT_EQ(r.size(), 1U);
    EXPECT_EQ(r[0], 3);
    EXPECT_EQ(r.newest(), 3);
}

TEST(RingBuffer, ClearForgetsEverythingAndRestartsOrdering) {
    RingBuffer<int, 3> r;
    for (int i = 0; i < 7; ++i) {
        r.push(i);
    }
    r.clear();
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.size(), 0U);
    r.push(100);
    r.push(200);
    ASSERT_EQ(r.size(), 2U);
    EXPECT_EQ(r[0], 100);
    EXPECT_EQ(r[1], 200);
}

/// First index whose stored byte differs from what the push sequence implies (size() if none).
std::size_t first_mismatch(const RingBuffer<std::uint8_t, 65535>& r, std::uint32_t pushes) {
    const std::size_t oldest = pushes - r.size();
    for (std::size_t i = 0; i < r.size(); ++i) {
        if (r[i] != static_cast<std::uint8_t>((oldest + i) % 251U)) {
            return i;
        }
    }
    return r.size();
}

TEST(RingBuffer, IndexingWorksAcrossTheUint16CounterRange) {
    // The largest permitted capacity: the head counter reaches 65534 and wraps to 0.
    static RingBuffer<std::uint8_t, 65535> r;
    r.clear();
    constexpr std::uint32_t kPushes = 65535U + 4000U;
    for (std::uint32_t i = 0; i < kPushes; ++i) {
        r.push(static_cast<std::uint8_t>(i % 251U));
    }
    ASSERT_EQ(r.size(), 65535U);
    EXPECT_EQ(first_mismatch(r, kPushes), r.size());
    EXPECT_EQ(r.newest(), static_cast<std::uint8_t>((kPushes - 1U) % 251U));
}

TEST(RingBuffer, LayoutIsTheArrayPlusTwoUint16AndSurvivesAMemcpy) {
    // "Layout (array + two uint16_t) is stable for RTC memory": persisted by copying bytes.
    static_assert(std::is_trivially_copyable_v<RingBuffer<std::uint32_t, 4>>);
    static_assert(sizeof(RingBuffer<std::uint32_t, 4>) ==
                  (4 * sizeof(std::uint32_t)) + (2 * sizeof(std::uint16_t)));

    RingBuffer<std::uint32_t, 4> a;
    for (std::uint32_t i = 1; i <= 6; ++i) {
        a.push(i * 1000U);
    }
    RingBuffer<std::uint32_t, 4> b;
    std::memcpy(&b, &a, sizeof(a));
    ASSERT_EQ(b.size(), 4U);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(b[i], a[i]);
    }
    b.push(7000U);
    EXPECT_EQ(b.newest(), 7000U);
    EXPECT_EQ(a.newest(), 6000U) << "the copy must not alias the original";
}

TEST(RingBuffer, StoresStructures) {
    RingBuffer<Item, 2> r;
    r.push(Item{.id = 1, .tag = 10});
    r.push(Item{.id = 2, .tag = 20});
    r.push(Item{.id = 3, .tag = 30});
    EXPECT_EQ(r[0], (Item{.id = 2, .tag = 20}));
    EXPECT_EQ(r.newest(), (Item{.id = 3, .tag = 30}));
}

TEST(RingBuffer, WorksInConstantExpressions) {
    constexpr auto build = [] {
        RingBuffer<int, 2> r;
        r.push(1);
        r.push(2);
        r.push(3);
        return r;
    };
    constexpr auto r = build();
    static_assert(r.size() == 2 && r[0] == 2 && r.newest() == 3);
    SUCCEED();
}

TEST(RingBufferDeathTest, IndexAtSizeAborts) {
    RingBuffer<int, 4> r;
    r.push(1);
    EXPECT_DEATH(static_cast<void>(r[1]), "QZ_ASSERT.*i < count_");
}

TEST(RingBufferDeathTest, NewestOnEmptyAborts) {
    const RingBuffer<int, 4> r;
    EXPECT_DEATH(static_cast<void>(r.newest()), "QZ_ASSERT.*i < count_");
}

} // namespace
} // namespace qz
