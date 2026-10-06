// FakeKvStore contract: typed get/set, limits, counters, fault injection, leak search.
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace qz::testkit {
namespace {

using settings::test::code_of;

TEST(FakeKvStore, TypedRoundTrips) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32("ns", "u", 0xDEADBEEFU));
    ASSERT_TRUE(kv.set_i32("ns", "i", -123456));
    ASSERT_TRUE(kv.set_i64("ns", "l", -9'000'000'000'000LL));
    ASSERT_TRUE(kv.set_str("ns", "s", "hello"));
    const std::array<std::uint8_t, 3> blob{0, 255, 7};
    ASSERT_TRUE(kv.set_blob("ns", "b", blob));
    EXPECT_EQ(*kv.get_u32("ns", "u"), 0xDEADBEEFU);
    EXPECT_EQ(*kv.get_i32("ns", "i"), -123456);
    EXPECT_EQ(*kv.get_i64("ns", "l"), -9'000'000'000'000LL);
    std::array<char, 16> text{};
    const auto n = kv.get_str("ns", "s", text);
    ASSERT_TRUE(n);
    EXPECT_EQ(std::string(text.data(), *n), "hello");
    std::array<std::uint8_t, 8> out{};
    const auto m = kv.get_blob("ns", "b", out);
    ASSERT_TRUE(m);
    EXPECT_EQ(*m, 3U);
    EXPECT_EQ(out[1], 255);
    ASSERT_TRUE(kv.set_str("ns", "empty", ""));
    EXPECT_EQ(*kv.get_str("ns", "empty", text), 0U);
}

TEST(FakeKvStore, ErrorsMatchNvsSemantics) {
    FakeKvStore kv;
    EXPECT_EQ(code_of(kv.get_u32("ns", "missing")), Errc::kNotFound);
    ASSERT_TRUE(kv.set_str("ns", "s", "abcdef"));
    EXPECT_EQ(code_of(kv.get_u32("ns", "s")), Errc::kCorrupt); // type mismatch
    std::array<char, 3> small{};
    EXPECT_EQ(code_of(kv.get_str("ns", "s", small)), Errc::kNoSpace);
    ASSERT_TRUE(kv.set_u32("ns", "u", 1));
    EXPECT_EQ(code_of(kv.get_i32("ns", "u")), Errc::kCorrupt); // u32 vs i32 are distinct types
    std::array<std::uint8_t, 1> tiny{};
    ASSERT_TRUE(kv.set_blob("ns", "b", std::array<std::uint8_t, 2>{1, 2}));
    EXPECT_EQ(code_of(kv.get_blob("ns", "b", tiny)), Errc::kNoSpace);
    EXPECT_EQ(code_of(kv.erase_key("ns", "nope")), Errc::kNotFound);
}

TEST(FakeKvStore, NamesAreLimitedToFifteenCharacters) {
    FakeKvStore kv;
    EXPECT_TRUE(kv.set_u32("123456789012345", "123456789012345", 1));
    EXPECT_EQ(code_of(kv.set_u32("1234567890123456", "k", 1)), Errc::kBadArgs);
    EXPECT_EQ(code_of(kv.set_u32("ns", "1234567890123456", 1)), Errc::kBadArgs);
    EXPECT_EQ(code_of(kv.set_u32("", "k", 1)), Errc::kBadArgs);
    EXPECT_EQ(code_of(kv.set_u32("ns", "", 1)), Errc::kBadArgs);
    EXPECT_EQ(code_of(kv.get_u32("ns", "1234567890123456")), Errc::kBadArgs);
    EXPECT_EQ(code_of(kv.erase_namespace("1234567890123456")), Errc::kBadArgs);
    EXPECT_EQ(kv.write_count(), 1U);
}

TEST(FakeKvStore, WriteCounterCountsEveryMutation) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32("ns", "a", 1));
    ASSERT_TRUE(kv.set_u32("ns", "a", 1)); // identical value still counts: callers must compare
    ASSERT_TRUE(kv.set_u32("ns", "b", 2));
    EXPECT_EQ(kv.write_count(), 3U);
    ASSERT_TRUE(kv.erase_key("ns", "a"));
    EXPECT_EQ(kv.write_count(), 4U);
    EXPECT_FALSE(kv.erase_key("ns", "a")); // already gone: no wear
    EXPECT_EQ(kv.write_count(), 4U);
    EXPECT_EQ(kv.commit_count(), 0U);
    ASSERT_TRUE(kv.commit());
    ASSERT_TRUE(kv.commit());
    EXPECT_EQ(kv.commit_count(), 2U);
    EXPECT_EQ(kv.write_count(), 4U); // commit is not a mutation
    (void)kv.get_u32("ns", "b");
    EXPECT_EQ(kv.write_count(), 4U); // reads are free
}

TEST(FakeKvStore, EraseNamespaceIsExactAndIdempotent) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32("qz_set", "a", 1));
    ASSERT_TRUE(kv.set_u32("qz_set", "b", 1));
    ASSERT_TRUE(kv.set_u32("qz_set2", "a", 1)); // prefix sibling must survive
    ASSERT_TRUE(kv.set_u32("qz_s", "a", 1));
    EXPECT_EQ(kv.entry_count("qz_set"), 2U);
    const std::uint32_t before = kv.write_count();
    ASSERT_TRUE(kv.erase_namespace("qz_set"));
    EXPECT_EQ(kv.entry_count("qz_set"), 0U);
    EXPECT_EQ(kv.entry_count("qz_set2"), 1U);
    EXPECT_EQ(kv.entry_count("qz_s"), 1U);
    EXPECT_EQ(kv.write_count(), before + 2);
    ASSERT_TRUE(kv.erase_namespace("qz_set")); // empty/absent namespace: ok, no wear
    EXPECT_EQ(kv.write_count(), before + 2);
}

TEST(FakeKvStore, ContainsTextSearchesValuesAndKeyNames) {
    FakeKvStore kv;
    EXPECT_FALSE(kv.contains_text("secret"));
    EXPECT_FALSE(kv.contains_text("")); // empty needle never "matches"
    ASSERT_TRUE(kv.set_str("a", "k1", "xx-secret-xx"));
    EXPECT_TRUE(kv.contains_text("secret"));
    EXPECT_FALSE(kv.contains_text("Secret"));
    ASSERT_TRUE(kv.set_blob("b", "k2", std::array<std::uint8_t, 4>{'p', 'w', 'n', 'd'}));
    EXPECT_TRUE(kv.contains_text("pwnd"));
    ASSERT_TRUE(kv.set_u32("c", "needle_key", 1));
    EXPECT_TRUE(kv.contains_text("needle_key"));
    ASSERT_TRUE(kv.erase_namespace("a"));
    EXPECT_FALSE(kv.contains_text("secret"));
}

TEST(FakeKvStore, WriteFaultInjection) {
    FakeKvStore kv;
    kv.fail_writes_after(2);
    ASSERT_TRUE(kv.set_u32("ns", "a", 1));
    ASSERT_TRUE(kv.set_str("ns", "b", "x"));
    EXPECT_EQ(code_of(kv.set_u32("ns", "c", 1)), Errc::kIo);
    EXPECT_EQ(code_of(kv.erase_key("ns", "a")), Errc::kIo);
    EXPECT_EQ(code_of(kv.erase_namespace("ns")), Errc::kIo);
    EXPECT_EQ(kv.entry_count("ns"), 2U); // failed writes change nothing
    EXPECT_EQ(kv.write_count(), 2U);
    kv.fail_writes_after(-1);
    EXPECT_TRUE(kv.set_u32("ns", "c", 1));
}

} // namespace
} // namespace qz::testkit
