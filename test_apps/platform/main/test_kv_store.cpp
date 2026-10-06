// KvStore round trip on the real NVS partition (owner-run; WP-24 acceptance).
#include "platform_impl.hpp"
#include "unity.h"
#include "unity_test_runner.h"

#include <array>
#include <cstdint>
#include <cstring>

using qz::Errc;
using qz::platform::IdfKvStore;

namespace {
constexpr const char* kNs = "qz_test";
}

TEST_CASE("KvStore: scalar and string and blob round trip", "[qz_platform][kv]") {
    IdfKvStore kv;
    TEST_ASSERT_TRUE(kv.ensure_init().has_value());
    TEST_ASSERT_TRUE(kv.erase_namespace(kNs).has_value());

    TEST_ASSERT_TRUE(kv.set_u32(kNs, "u32", 0xDEADBEEFU).has_value());
    TEST_ASSERT_TRUE(kv.set_i32(kNs, "i32", -123456).has_value());
    TEST_ASSERT_TRUE(kv.set_i64(kNs, "i64", -9'000'000'000'000LL).has_value());
    TEST_ASSERT_TRUE(kv.set_str(kNs, "str", "Europe/Berlin").has_value());
    const std::array<std::uint8_t, 4> blob = {1, 2, 3, 4};
    TEST_ASSERT_TRUE(kv.set_blob(kNs, "blob", blob).has_value());
    TEST_ASSERT_TRUE(kv.commit().has_value());

    const auto u32 = kv.get_u32(kNs, "u32");
    TEST_ASSERT_TRUE(u32.has_value());
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFU, *u32);
    const auto i32 = kv.get_i32(kNs, "i32");
    TEST_ASSERT_TRUE(i32.has_value());
    TEST_ASSERT_EQUAL_INT32(-123456, *i32);
    const auto i64 = kv.get_i64(kNs, "i64");
    TEST_ASSERT_TRUE(i64.has_value());
    TEST_ASSERT_TRUE(*i64 == -9'000'000'000'000LL);

    std::array<char, 16> str{};
    const auto slen = kv.get_str(kNs, "str", str);
    TEST_ASSERT_TRUE(slen.has_value());
    TEST_ASSERT_EQUAL_UINT32(13, *slen);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(str.data(), "Europe/Berlin", 13));

    std::array<std::uint8_t, 8> got{};
    const auto blen = kv.get_blob(kNs, "blob", got);
    TEST_ASSERT_TRUE(blen.has_value());
    TEST_ASSERT_EQUAL_UINT32(4, *blen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(blob.data(), got.data(), 4);

    TEST_ASSERT_TRUE(kv.erase_namespace(kNs).has_value());
}

TEST_CASE("KvStore: missing key, small buffer, bad names", "[qz_platform][kv]") {
    IdfKvStore kv;
    TEST_ASSERT_TRUE(kv.erase_namespace(kNs).has_value());

    const auto missing = kv.get_u32(kNs, "nope");
    TEST_ASSERT_FALSE(missing.has_value());
    TEST_ASSERT_TRUE(missing.error().code == Errc::kNotFound);

    TEST_ASSERT_TRUE(kv.set_str(kNs, "s", "hello").has_value());
    std::array<char, 3> small{};
    const auto too_small = kv.get_str(kNs, "s", small);
    TEST_ASSERT_FALSE(too_small.has_value());
    TEST_ASSERT_TRUE(too_small.error().code == Errc::kNoSpace);

    const std::array<std::uint8_t, 4> blob = {9, 9, 9, 9};
    TEST_ASSERT_TRUE(kv.set_blob(kNs, "b", blob).has_value());
    std::array<std::uint8_t, 2> small_blob{};
    const auto blob_small = kv.get_blob(kNs, "b", small_blob);
    TEST_ASSERT_FALSE(blob_small.has_value());
    TEST_ASSERT_TRUE(blob_small.error().code == Errc::kNoSpace);

    // 16-character key is over the NVS limit.
    const auto bad = kv.set_u32(kNs, "0123456789abcdef", 1);
    TEST_ASSERT_FALSE(bad.has_value());
    TEST_ASSERT_TRUE(bad.error().code == Errc::kBadArgs);
    TEST_ASSERT_TRUE(kv.set_u32("", "k", 1).error().code == Errc::kBadArgs);

    TEST_ASSERT_TRUE(kv.erase_key(kNs, "s").has_value());
    TEST_ASSERT_TRUE(kv.erase_key(kNs, "s").error().code == Errc::kNotFound);
    TEST_ASSERT_TRUE(kv.erase_namespace(kNs).has_value());
    TEST_ASSERT_TRUE(kv.erase_namespace(kNs).has_value()); // already gone: success
}

// Stage 1 writes and restarts; stage 2 (after esp_restart) must still read the values.
static void kv_persist_write() {
    IdfKvStore kv;
    TEST_ASSERT_TRUE(kv.set_u32(kNs, "persist", 0x51515151U).has_value());
    TEST_ASSERT_TRUE(kv.set_str(kNs, "pstr", "survives").has_value());
    TEST_ASSERT_TRUE(kv.commit().has_value());
    qz::platform::IdfSystem().restart();
}

static void kv_persist_verify() {
    IdfKvStore kv;
    const auto v = kv.get_u32(kNs, "persist");
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_EQUAL_UINT32(0x51515151U, *v);
    std::array<char, 16> str{};
    const auto n = kv.get_str(kNs, "pstr", str);
    TEST_ASSERT_TRUE(n.has_value());
    TEST_ASSERT_EQUAL_UINT32(8, *n);
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(str.data(), "survives", 8));
    TEST_ASSERT_TRUE(kv.erase_namespace(kNs).has_value());
}

TEST_CASE_MULTIPLE_STAGES("KvStore: values persist across esp_restart",
                          "[qz_platform][kv]",
                          kv_persist_write,
                          kv_persist_verify);
