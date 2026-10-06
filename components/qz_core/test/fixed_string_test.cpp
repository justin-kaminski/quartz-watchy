// FixedString (heap-free text) and Secret (credential storage that wipes itself).
#include "qz/core/fixed_string.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>

namespace qz {
namespace {

// --- FixedString --------------------------------------------------------------------------------

TEST(FixedString, DefaultIsEmptyAndTerminated) {
    const FixedString<8> s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0U);
    EXPECT_EQ(s.view(), "");
    EXPECT_STREQ(s.c_str(), "");
    EXPECT_EQ((FixedString<8>::capacity()), 8U);
}

TEST(FixedString, ConstructsFromALiteralThatFits) {
    const FixedString<8> s("abc");
    EXPECT_EQ(s.view(), "abc");
    EXPECT_EQ(s.size(), 3U);
    EXPECT_FALSE(s.empty());
    EXPECT_STREQ(s.c_str(), "abc");

    const FixedString<4> exact("abcd");
    EXPECT_EQ(exact.view(), "abcd");
}

TEST(FixedString, AssignCopiesAndTerminates) {
    FixedString<8> s;
    EXPECT_TRUE(s.assign("hello"));
    EXPECT_EQ(s.view(), "hello");
    EXPECT_STREQ(s.c_str(), "hello");
}

TEST(FixedString, AssignReplacesAPreviousLongerValue) {
    FixedString<8> s("longtext");
    EXPECT_TRUE(s.assign("ab"));
    EXPECT_EQ(s.view(), "ab");
    EXPECT_EQ(s.size(), 2U);
    EXPECT_STREQ(s.c_str(), "ab") << "the terminator must follow the new, shorter value";
}

TEST(FixedString, AssignExactlyCapacityFitsOneMoreDoesNot) {
    FixedString<5> s;
    EXPECT_TRUE(s.assign("12345"));
    EXPECT_EQ(s.view(), "12345");

    EXPECT_FALSE(s.assign("123456"));
    EXPECT_TRUE(s.empty()) << "a failed assign leaves the string empty";
    EXPECT_STREQ(s.c_str(), "");
}

TEST(FixedString, AssignOfTheEmptyStringClears) {
    FixedString<4> s("abcd");
    EXPECT_TRUE(s.assign(""));
    EXPECT_TRUE(s.empty());
}

TEST(FixedString, ClearEmpties) {
    FixedString<4> s("abc");
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.view(), "");
    EXPECT_STREQ(s.c_str(), "");
}

TEST(FixedString, EqualityComparesTheTextOnly) {
    const FixedString<8> a("same");
    const FixedString<8> b("same");
    const FixedString<8> c("other");
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);

    // Stale bytes after the terminator must not influence the comparison.
    FixedString<8> stale("longtext");
    EXPECT_TRUE(stale.assign("same"));
    EXPECT_TRUE(stale == a);
}

TEST(FixedString, ZeroCapacityHoldsOnlyTheEmptyString) {
    FixedString<0> s;
    EXPECT_TRUE(s.assign(""));
    EXPECT_FALSE(s.assign("x"));
    EXPECT_TRUE(s.empty());
}

TEST(FixedString, IsTriviallyCopyableAndCopiesAreIndependent) {
    static_assert(std::is_trivially_copyable_v<FixedString<16>>);
    const FixedString<8> a("abc");
    FixedString<8> b = a;
    EXPECT_TRUE(b.assign("xyz"));
    EXPECT_EQ(a.view(), "abc");
    EXPECT_EQ(b.view(), "xyz");
}

TEST(FixedString, WorksInConstantExpressions) {
    constexpr FixedString<8> s("abc");
    static_assert(s.size() == 3 && s.view() == "abc");
    static_assert(s == FixedString<8>("abc"));
    SUCCEED();
}

// --- Secret -------------------------------------------------------------------------------------

constexpr std::string_view kPassword = "hunter2-hunter2";

/// True if `needle` occurs anywhere in the raw bytes.
template<std::size_t Size>
bool contains(const std::array<unsigned char, Size>& bytes, std::string_view needle) {
    const auto* first = reinterpret_cast<const char*>(bytes.data());
    return std::search(first, first + Size, needle.begin(), needle.end()) != first + Size;
}

template<std::size_t Size>
bool all_zero(const std::array<unsigned char, Size>& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](unsigned char b) { return b == 0; });
}

TEST(Secret, StartsEmpty) {
    const Secret<16> s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.reveal(), "");
}

TEST(Secret, AssignRevealAndClear) {
    Secret<16> s;
    EXPECT_TRUE(s.assign("pa55word"));
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.reveal(), "pa55word");
    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.reveal(), "");
}

TEST(Secret, AssignThatDoesNotFitReportsFalseAndStoresNothing) {
    Secret<4> s;
    EXPECT_TRUE(s.assign("1234"));
    EXPECT_FALSE(s.assign("12345"));
    EXPECT_TRUE(s.empty());
}

TEST(Secret, ClearWipesTheBytesInPlace) {
    alignas(Secret<16>) std::array<unsigned char, sizeof(Secret<16>)> storage{};
    auto* s = std::construct_at(reinterpret_cast<Secret<16>*>(storage.data()));
    ASSERT_TRUE(s->assign(kPassword));
    ASSERT_TRUE(contains(storage, kPassword)) << "sanity: the plaintext is in the object";

    s->clear();
    EXPECT_TRUE(s->empty());
    EXPECT_FALSE(contains(storage, kPassword));
    EXPECT_TRUE(all_zero(storage)) << "clear() must zero the whole object, not just the length";
    std::destroy_at(s);
}

TEST(Secret, DestructionWipesTheBytes) {
    alignas(Secret<16>) std::array<unsigned char, sizeof(Secret<16>)> storage{};
    auto* s = std::construct_at(reinterpret_cast<Secret<16>*>(storage.data()));
    ASSERT_TRUE(s->assign(kPassword));
    ASSERT_TRUE(contains(storage, kPassword));

    std::destroy_at(s); // reading the raw storage afterwards is fine: it is an unsigned char array
    EXPECT_FALSE(contains(storage, kPassword));
    EXPECT_TRUE(all_zero(storage));
}

TEST(Secret, CopiesAreIndependentAndEachWipesItself) {
    alignas(Secret<16>) std::array<unsigned char, sizeof(Secret<16>)> original_storage{};
    alignas(Secret<16>) std::array<unsigned char, sizeof(Secret<16>)> copy_storage{};
    auto* original = std::construct_at(reinterpret_cast<Secret<16>*>(original_storage.data()));
    ASSERT_TRUE(original->assign(kPassword));
    auto* copy = std::construct_at(reinterpret_cast<Secret<16>*>(copy_storage.data()), *original);
    EXPECT_EQ(copy->reveal(), kPassword);

    std::destroy_at(copy);
    EXPECT_TRUE(all_zero(copy_storage));
    EXPECT_EQ(original->reveal(), kPassword) << "wiping the copy must not touch the original";

    std::destroy_at(original);
    EXPECT_TRUE(all_zero(original_storage));
}

TEST(Secret, CopyAssignmentReplacesTheValue) {
    Secret<16> a;
    Secret<16> b;
    ASSERT_TRUE(a.assign("first"));
    ASSERT_TRUE(b.assign("second-value"));
    a = b;
    EXPECT_EQ(a.reveal(), "second-value");
    b.clear();
    EXPECT_EQ(a.reveal(), "second-value");
}

TEST(Secret, OffersNoImplicitConversionOrFormattingPath) {
    static_assert(!std::is_convertible_v<Secret<8>, std::string_view>);
    static_assert(!std::is_convertible_v<Secret<8>, const char*>);
    static_assert(!std::is_convertible_v<Secret<8>, bool>);
    static_assert(!std::is_constructible_v<std::string_view, const Secret<8>&>);
    // A non-trivial destructor is what performs the wipe.
    static_assert(!std::is_trivially_destructible_v<Secret<8>>);
    SUCCEED();
}

} // namespace
} // namespace qz
