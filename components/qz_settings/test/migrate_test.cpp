// Migration framework (src/migrate.hpp) with a v0 -> v1 fixture and multi-step chains.
#include "../src/migrate.hpp"
#include "qz/settings/settings.hpp"
#include "qz/testkit/fakes.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace qz::settings {
namespace {

using test::code_of;
using testkit::FakeKvStore;

constexpr std::string_view kNs = "qz_set";

// ---- fixture: a hypothetical v0 layout that stored the clock as bool `h24` (1 = 24 h) ----

Status fixture_v0_to_v1(hal::KvStore& kv) noexcept {
    const auto h24 = kv.get_u32(kNs, "h24");
    if (!h24) {
        // Idempotent replay: already renamed (or never stored).
        return h24.error().code == Errc::kNotFound ? ok() : Status(h24.error());
    }
    QZ_RETURN_IF_ERROR(kv.set_u32(kNs, "tfmt", *h24 != 0 ? 0 : 1));
    return kv.erase_key(kNs, "h24");
}

Status fixture_v1_to_v2(hal::KvStore& kv) noexcept {
    // Reads what v0->v1 produced: proves the steps run in order.
    const auto tfmt = kv.get_u32(kNs, "tfmt");
    if (!tfmt) {
        return tfmt.error();
    }
    return kv.set_u32(kNs, "tfmt_copy", *tfmt);
}

Status fixture_fail(hal::KvStore& /*kv*/) noexcept {
    return Errc::kIo;
}

const std::array<MigrationStep, 2> kChain{{{0, &fixture_v0_to_v1}, {1, &fixture_v1_to_v2}}};

TEST(Migration, V0ToV1FixtureRenamesKeyAndStampsVersion) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "h24", 0)); // 12-hour user
    ASSERT_TRUE(run_migrations(kv, kNs, 0, 1, kChain));
    ASSERT_TRUE(kv.get_u32(kNs, "tfmt"));
    EXPECT_EQ(*kv.get_u32(kNs, "tfmt"), 1U); // 12h
    EXPECT_EQ(code_of(kv.get_u32(kNs, "h24")), Errc::kNotFound);
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), 1U);
    EXPECT_EQ(kv.commit_count(), 1U);
}

TEST(Migration, ChainRunsEveryStepInOrderAndCommitsOnce) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "h24", 1));
    ASSERT_TRUE(run_migrations(kv, kNs, 0, 2, kChain));
    EXPECT_EQ(*kv.get_u32(kNs, "tfmt"), 0U);
    EXPECT_EQ(*kv.get_u32(kNs, "tfmt_copy"), 0U);
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), 2U);
    EXPECT_EQ(kv.commit_count(), 1U);

    // Starting from v1 runs only the second step.
    FakeKvStore from_v1;
    ASSERT_TRUE(from_v1.set_u32(kNs, "tfmt", 1));
    ASSERT_TRUE(run_migrations(from_v1, kNs, 1, 2, kChain));
    EXPECT_EQ(*from_v1.get_u32(kNs, "tfmt_copy"), 1U);
    EXPECT_EQ(*from_v1.get_u32(kNs, "ver"), 2U);
}

TEST(Migration, CurrentVersionIsANoOp) {
    FakeKvStore kv;
    ASSERT_TRUE(run_migrations(kv, kNs, 2, 2, kChain));
    EXPECT_EQ(kv.write_count(), 0U);
    EXPECT_EQ(kv.commit_count(), 0U);
}

TEST(Migration, NewerDataIsRefused) {
    FakeKvStore kv;
    EXPECT_EQ(code_of(run_migrations(kv, kNs, 3, 2, kChain)), Errc::kInvalidState);
    EXPECT_EQ(kv.write_count(), 0U);
}

TEST(Migration, MissingStepKeepsLastCompletedVersion) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "h24", 1));
    EXPECT_EQ(code_of(run_migrations(kv, kNs, 0, 3, kChain)), Errc::kCorrupt);
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), 2U); // steps 0 and 1 completed, 2 -> 3 does not exist
    EXPECT_EQ(kv.commit_count(), 0U);
    EXPECT_EQ(code_of(run_migrations(kv, kNs, 0, 1, std::span<const MigrationStep>{})),
              Errc::kCorrupt);
}

TEST(Migration, FailingStepStopsTheChain) {
    const std::array<MigrationStep, 2> steps{{{0, &fixture_v0_to_v1}, {1, &fixture_fail}}};
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "h24", 1));
    EXPECT_EQ(code_of(run_migrations(kv, kNs, 0, 2, steps)), Errc::kIo);
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), 1U);
    EXPECT_EQ(kv.commit_count(), 0U);

    const std::array<MigrationStep, 1> null_step{{{0, nullptr}}};
    EXPECT_EQ(code_of(run_migrations(kv, kNs, 0, 1, null_step)), Errc::kCorrupt);
}

TEST(Migration, StepsAreIdempotentAcrossAPowerLossReplay) {
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "h24", 0));
    // Power loss after the step but before the ver bump: simulate by running the step directly.
    ASSERT_TRUE(fixture_v0_to_v1(kv));
    ASSERT_TRUE(run_migrations(kv, kNs, 0, 1, kChain)); // replay from the stale ver
    EXPECT_EQ(*kv.get_u32(kNs, "tfmt"), 1U);
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), 1U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Migration, ProductionTableCoversEveryVersionUpToCurrent) {
    const auto steps = settings_migrations();
    for (std::uint16_t v = 0; v < kSchemaVersion; ++v) {
        bool found = false;
        for (const MigrationStep& step : steps) {
            found = found || (step.from == v && step.apply != nullptr);
        }
        EXPECT_TRUE(found) << "no migration from v" << v;
    }
    FakeKvStore kv;
    ASSERT_TRUE(kv.set_u32(kNs, "goal", 1500));
    ASSERT_TRUE(run_migrations(kv, kNs, 0, kSchemaVersion, steps));
    EXPECT_EQ(*kv.get_u32(kNs, "ver"), kSchemaVersion);
    EXPECT_EQ(*kv.get_u32(kNs, "goal"), 1500U); // v0 -> v1 keeps every key
}

} // namespace
} // namespace qz::settings
