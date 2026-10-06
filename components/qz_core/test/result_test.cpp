// Errc tokens, Error, Result<T>, Status and QZ_RETURN_IF_ERROR (ARCHITECTURE.md sections 2, 16).
#include "qz/core/result.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace qz {
namespace {

/// An Errc built from a raw byte, as it would look after memory corruption.
Errc errc_from_byte(std::uint8_t byte) {
    return std::bit_cast<Errc>(byte);
}

bool is_snake_case(std::string_view token) {
    return std::ranges::all_of(token, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
}

struct TokenCase {
    Errc code;
    std::string_view token;
};

// The console protocol v1 error codes, in declaration order (ARCHITECTURE.md section 16).
constexpr std::array<TokenCase, 14> kTokens = {{
    {.code = Errc::kBadArgs, .token = "bad_args"},
    {.code = Errc::kUnknownCommand, .token = "unknown_cmd"},
    {.code = Errc::kUnsupported, .token = "unsupported"},
    {.code = Errc::kInvalidState, .token = "invalid_state"},
    {.code = Errc::kBusy, .token = "busy"},
    {.code = Errc::kIo, .token = "io"},
    {.code = Errc::kTimeout, .token = "timeout"},
    {.code = Errc::kNotFound, .token = "not_found"},
    {.code = Errc::kNoTime, .token = "no_time"},
    {.code = Errc::kNoCredentials, .token = "no_creds"},
    {.code = Errc::kBatteryLow, .token = "battery_low"},
    {.code = Errc::kCorrupt, .token = "corrupt"},
    {.code = Errc::kNoSpace, .token = "no_space"},
    {.code = Errc::kInternal, .token = "internal"},
}};

TEST(ErrcToken, EveryCodeMapsToItsDocumentedToken) {
    for (const TokenCase& c : kTokens) {
        EXPECT_EQ(to_token(c.code), c.token) << "code " << static_cast<int>(c.code);
    }
}

TEST(ErrcToken, EveryCodeHasAUniqueNonEmptyLowercaseToken) {
    std::set<std::string_view> seen;
    for (const TokenCase& c : kTokens) {
        const std::string_view token = to_token(c.code);
        EXPECT_TRUE(!token.empty() && is_snake_case(token)) << token;
        EXPECT_TRUE(seen.insert(token).second) << "duplicate token " << token;
    }
    EXPECT_EQ(seen.size(), kTokens.size());
}

TEST(ErrcToken, TableCoversTheWholeEnumeration) {
    // Every byte value that is not a named code falls back to "internal", so the number of
    // distinct tokens over all 256 values equals the number of named codes. Adding an Errc
    // without a test-table entry (and a console protocol test) makes this fail.
    std::set<std::string_view> tokens;
    for (unsigned value = 0; value <= 0xFFU; ++value) {
        const std::string_view token = to_token(errc_from_byte(static_cast<std::uint8_t>(value)));
        EXPECT_FALSE(token.empty()) << "value " << value;
        tokens.insert(token);
    }
    EXPECT_EQ(tokens.size(), kTokens.size());
}

TEST(ErrcToken, ValueOutsideTheEnumerationIsInternal) {
    EXPECT_EQ(to_token(errc_from_byte(200)), "internal");
    EXPECT_EQ(to_token(errc_from_byte(0xFF)), "internal");
}

TEST(ErrorValue, DefaultsToInternalWithoutDetail) {
    constexpr Error e;
    static_assert(e.code == Errc::kInternal);
    static_assert(e.detail == 0);
    SUCCEED();
}

TEST(ErrorValue, CarriesCodeAndDetailAndComparesByBoth) {
    constexpr Error a{Errc::kIo, 0x1234};
    EXPECT_EQ(a.code, Errc::kIo);
    EXPECT_EQ(a.detail, 0x1234);
    EXPECT_EQ(a, (Error{Errc::kIo, 0x1234}));
    EXPECT_NE(a, (Error{Errc::kIo, 0x1235}));
    EXPECT_NE(a, (Error{Errc::kBusy, 0x1234}));
    EXPECT_EQ((Error{Errc::kBusy}).detail, 0);
}

TEST(ErrorValue, IsFourBytesAndTriviallyCopyable) {
    static_assert(sizeof(Error) == 4, "Error is documented as 4 bytes");
    static_assert(std::is_trivially_copyable_v<Error>);
    SUCCEED();
}

TEST(ResultValue, HoldsAValue) {
    Result<int> r = 42;
    EXPECT_TRUE(r);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
    EXPECT_EQ(r.value_or(7), 42);
}

TEST(ResultValue, HoldsAnErrorFromErrcOrError) {
    const Result<int> from_code = Errc::kTimeout;
    EXPECT_FALSE(from_code);
    EXPECT_FALSE(from_code.has_value());
    EXPECT_EQ(from_code.error(), Error{Errc::kTimeout});
    EXPECT_EQ(from_code.value_or(7), 7);

    const Result<int> from_error = Error{Errc::kIo, 0x00F0};
    EXPECT_FALSE(from_error);
    EXPECT_EQ(from_error.error().code, Errc::kIo);
    EXPECT_EQ(from_error.error().detail, 0x00F0);
}

TEST(ResultValue, DereferenceAllowsMutationAndMemberAccess) {
    struct Pair {
        int a = 0;
        int b = 0;
    };
    Result<Pair> r = Pair{.a = 1, .b = 2};
    ASSERT_TRUE(r);
    r->a = 10;
    (*r).b = 20;
    const Result<Pair>& cr = r;
    EXPECT_EQ(cr->a, 10);
    EXPECT_EQ((*cr).b, 20);
}

TEST(ResultValue, MovesTheValueOut) {
    Result<std::string> r = std::string("a string long enough to defeat the small string buffer");
    ASSERT_TRUE(r);
    const std::string moved = *std::move(r);
    EXPECT_EQ(moved, "a string long enough to defeat the small string buffer");
}

TEST(ResultValue, WorksInConstantExpressions) {
    constexpr Result<int> good = 5;
    constexpr Result<int> bad = Errc::kBusy;
    static_assert(good.has_value() && *good == 5);
    static_assert(!bad.has_value() && bad.error().code == Errc::kBusy);
    SUCCEED();
}

TEST(StatusValue, DefaultIsSuccess) {
    const Status s;
    EXPECT_TRUE(s);
    EXPECT_TRUE(s.has_value());
}

TEST(StatusValue, OkIsSuccess) {
    constexpr Status s = ok();
    static_assert(s.has_value());
    EXPECT_TRUE(ok());
}

TEST(StatusValue, HoldsAnErrorFromErrcOrError) {
    const Status from_code = Errc::kNotFound;
    EXPECT_FALSE(from_code);
    EXPECT_EQ(from_code.error(), Error{Errc::kNotFound});

    const Status from_error = Error{Errc::kCorrupt, 9};
    EXPECT_FALSE(from_error.has_value());
    EXPECT_EQ(from_error.error().detail, 9);
}

// --- QZ_RETURN_IF_ERROR -----------------------------------------------------------------------

Status run_two(const Status& first, const Status& second, int& reached) {
    reached = 0;
    QZ_RETURN_IF_ERROR(first);
    reached = 1;
    QZ_RETURN_IF_ERROR(second);
    reached = 2;
    return ok();
}

Result<int> compute(const Result<int>& input, int& reached) {
    reached = 0;
    QZ_RETURN_IF_ERROR(input);
    reached = 1;
    return *input + 1;
}

Result<int> status_in_value_function(const Status& s) {
    QZ_RETURN_IF_ERROR(s);
    return 99;
}

TEST(ReturnIfError, ContinuesOnSuccess) {
    int reached = -1;
    const Status result = run_two(ok(), ok(), reached);
    EXPECT_TRUE(result);
    EXPECT_EQ(reached, 2);
}

TEST(ReturnIfError, PropagatesTheFirstErrorWithItsDetail) {
    int reached = -1;
    const Status result = run_two(Error{Errc::kBusy, 7}, Error{Errc::kIo, 1}, reached);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), (Error{Errc::kBusy, 7}));
    EXPECT_EQ(reached, 0);
}

TEST(ReturnIfError, PropagatesALaterError) {
    int reached = -1;
    const Status result = run_two(ok(), Error{Errc::kTimeout, 3}, reached);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), (Error{Errc::kTimeout, 3}));
    EXPECT_EQ(reached, 1);
}

TEST(ReturnIfError, AcceptsResultOfValueTypeAndConvertsToTheCallersReturnType) {
    int reached = -1;
    const Result<int> good = compute(Result<int>{41}, reached);
    ASSERT_TRUE(good);
    EXPECT_EQ(*good, 42);
    EXPECT_EQ(reached, 1);

    const Result<int> bad = compute(Result<int>{Error{Errc::kNoTime, 5}}, reached);
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error(), (Error{Errc::kNoTime, 5}));
    EXPECT_EQ(reached, 0);
}

TEST(ReturnIfError, StatusConvertsIntoAValueReturningFunction) {
    EXPECT_EQ(status_in_value_function(ok()).value_or(-1), 99);
    const Result<int> failed = status_in_value_function(Errc::kUnsupported);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, Errc::kUnsupported);
}

TEST(ReturnIfError, EvaluatesItsArgumentExactlyOnce) {
    int evaluations = 0;
    const auto counted = [&evaluations]() -> Status {
        ++evaluations;
        return Errc::kBusy;
    };
    const auto caller = [&counted]() -> Status {
        QZ_RETURN_IF_ERROR(counted());
        return ok();
    };
    EXPECT_FALSE(caller());
    EXPECT_EQ(evaluations, 1);
}

// --- programmer errors abort (QZ_ASSERT stays on in release) -----------------------------------

TEST(ResultDeathTest, DereferencingAnErrorAborts) {
    const Result<int> r = Errc::kIo;
    EXPECT_DEATH(static_cast<void>(*r), "QZ_ASSERT.*has_value");
}

struct Box {
    int v = 0;
};

TEST(ResultDeathTest, MemberAccessOnAnErrorAborts) {
    const Result<Box> r = Errc::kIo;
    EXPECT_DEATH(static_cast<void>(r->v), "QZ_ASSERT.*has_value");
}

TEST(ResultDeathTest, ErrorOfASuccessAborts) {
    const Result<int> r = 1;
    EXPECT_DEATH(static_cast<void>(r.error()), "QZ_ASSERT.*has_value");
}

TEST(ResultDeathTest, ErrorOfASuccessfulStatusAborts) {
    const Status s = ok();
    EXPECT_DEATH(static_cast<void>(s.error()), "QZ_ASSERT.*has_value");
}

} // namespace
} // namespace qz
