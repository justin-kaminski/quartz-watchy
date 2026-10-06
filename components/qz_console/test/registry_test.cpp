// Registry (registry.hpp): registration rules, longest-match lookup, capacity.
#include "qz/console/registry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qz::console {
namespace {

Status
noop_handler(DeviceApi& /*api*/, std::span<const std::string_view> /*args*/, JsonWriter& /*out*/) {
    return ok();
}

Status
other_handler(DeviceApi& /*api*/, std::span<const std::string_view> /*args*/, JsonWriter& /*out*/) {
    return ok();
}

Command make_command(std::string_view name,
                     std::uint8_t min_args = 0,
                     std::uint8_t max_args = 0,
                     std::uint8_t flags = kFlagNone,
                     Handler handler = noop_handler) {
    return Command{name, "usage text", "help text", min_args, max_args, flags, handler};
}

const Command* find_in(const Registry& registry,
                       std::initializer_list<std::string_view> tokens,
                       std::size_t* words_used = nullptr) {
    return registry.find(std::span<const std::string_view>(tokens.begin(), tokens.size()),
                         words_used);
}

TEST(Registry, FindsOneWordAndTwoWordCommands) {
    Registry registry;
    ASSERT_TRUE(registry.add(make_command("status")).has_value());
    ASSERT_TRUE(registry.add(make_command("time get")).has_value());
    ASSERT_TRUE(registry.add(make_command("time set", 1, 1)).has_value());

    std::size_t words = 99;
    const Command* status = find_in(registry, {"status"}, &words);
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->name, "status");
    EXPECT_EQ(words, 1U);

    const Command* set = find_in(registry, {"time", "set", "2026-01-01T00:00:00Z"}, &words);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->name, "time set");
    EXPECT_EQ(words, 2U);

    const Command* get = find_in(registry, {"time", "get"}, &words);
    ASSERT_NE(get, nullptr);
    EXPECT_EQ(get->name, "time get");
    EXPECT_EQ(words, 2U);
}

TEST(Registry, ExtraTokensAfterTheNameDoNotAffectTheMatch) {
    Registry registry;
    ASSERT_TRUE(registry.add(make_command("echo", 0, 3)).has_value());
    std::size_t words = 0;
    const Command* echo = find_in(registry, {"echo", "a", "b", "c"}, &words);
    ASSERT_NE(echo, nullptr);
    EXPECT_EQ(words, 1U);
}

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Registry, TheLongestMatchWinsWhateverTheRegistrationOrder) {
    for (const bool two_word_first : {true, false}) {
        SCOPED_TRACE(two_word_first);
        Registry registry;
        const Command one = make_command("time", 0, 2);
        const Command two = make_command("time get");
        ASSERT_TRUE(registry.add(two_word_first ? two : one).has_value());
        ASSERT_TRUE(registry.add(two_word_first ? one : two).has_value());

        std::size_t words = 0;
        const Command* both = find_in(registry, {"time", "get"}, &words);
        ASSERT_NE(both, nullptr);
        EXPECT_EQ(both->name, "time get");
        EXPECT_EQ(words, 2U);

        // The one-word command takes the requests the two-word one does not claim.
        const Command* fallback = find_in(registry, {"time", "other"}, &words);
        ASSERT_NE(fallback, nullptr);
        EXPECT_EQ(fallback->name, "time");
        EXPECT_EQ(words, 1U);
    }
}

TEST(Registry, UnknownOrIncompleteRequestsMatchNothing) {
    Registry registry;
    ASSERT_TRUE(registry.add(make_command("status")).has_value());
    ASSERT_TRUE(registry.add(make_command("wifi set", 2, 2)).has_value());

    std::size_t words = 99;
    EXPECT_EQ(find_in(registry, {}, &words), nullptr);
    EXPECT_EQ(words, 0U);
    EXPECT_EQ(find_in(registry, {"nope"}, &words), nullptr);
    EXPECT_EQ(words, 0U);
    EXPECT_EQ(find_in(registry, {"wifi"}, &words), nullptr); // only "wifi set" exists
    EXPECT_EQ(find_in(registry, {"wifi", "clear"}, &words), nullptr);
    EXPECT_EQ(find_in(registry, {"set", "wifi"}, &words), nullptr);
    EXPECT_EQ(words, 0U);
}

TEST(Registry, MatchingIsExactAndCaseSensitive) {
    Registry registry;
    ASSERT_TRUE(registry.add(make_command("status")).has_value());
    ASSERT_TRUE(registry.add(make_command("time get")).has_value());
    EXPECT_EQ(find_in(registry, {"Status"}), nullptr);
    EXPECT_EQ(find_in(registry, {"STATUS"}), nullptr);
    EXPECT_EQ(find_in(registry, {"stat"}), nullptr);
    EXPECT_EQ(find_in(registry, {"status2"}), nullptr);
    EXPECT_EQ(find_in(registry, {"time", "GET"}), nullptr);
    EXPECT_EQ(find_in(registry, {"time", "ge"}), nullptr);
    EXPECT_EQ(find_in(registry, {""}), nullptr);
}

TEST(Registry, WordsUsedPointerIsOptional) {
    Registry registry;
    ASSERT_TRUE(registry.add(make_command("status")).has_value());
    EXPECT_NE(find_in(registry, {"status"}), nullptr);
    EXPECT_EQ(find_in(registry, {"nope"}), nullptr);
}

TEST(Registry, KeepsEveryFieldOfTheCommandAndReturnsPointersIntoAll) {
    Registry registry;
    const Command original{"vibrate",
                           "vibrate [ms]",
                           "buzz the motor",
                           0,
                           1,
                           kFlagDestructive | kFlagNeedsRadio,
                           other_handler};
    ASSERT_TRUE(registry.add(make_command("first")).has_value());
    ASSERT_TRUE(registry.add(original).has_value());

    const Command* found = find_in(registry, {"vibrate"});
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, &registry.all()[1]);
    EXPECT_EQ(found->usage, "vibrate [ms]");
    EXPECT_EQ(found->help, "buzz the motor");
    EXPECT_EQ(found->min_args, 0U);
    EXPECT_EQ(found->max_args, 1U);
    EXPECT_EQ(found->flags, kFlagDestructive | kFlagNeedsRadio);
    EXPECT_EQ(found->handler, &other_handler);
}

TEST(Registry, AllListsCommandsInRegistrationOrder) {
    Registry registry;
    EXPECT_TRUE(registry.all().empty());
    for (const std::string_view name : {"zeta", "alpha", "mid word", "beta"}) {
        ASSERT_TRUE(registry.add(make_command(name)).has_value());
    }
    const std::span<const Command> all = registry.all();
    ASSERT_EQ(all.size(), 4U);
    EXPECT_EQ(all[0].name, "zeta");
    EXPECT_EQ(all[1].name, "alpha");
    EXPECT_EQ(all[2].name, "mid word");
    EXPECT_EQ(all[3].name, "beta");
}

TEST(Registry, DuplicateNamesAreRefusedAndLeaveTheRegistryUnchanged) {
    Registry registry;
    ASSERT_TRUE(registry.add(make_command("status", 0, 0, kFlagNone, noop_handler)).has_value());
    const Status again = registry.add(make_command("status", 0, 1, kFlagSensitive, other_handler));
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, Errc::kBadArgs);
    ASSERT_EQ(registry.all().size(), 1U);
    EXPECT_EQ(registry.all()[0].handler, &noop_handler);
    EXPECT_EQ(registry.all()[0].max_args, 0U);

    // Same words in a different arity are different names.
    EXPECT_TRUE(registry.add(make_command("status extra")).has_value());
}

TEST(Registry, MalformedDefinitionsAreRefused) {
    struct Bad {
        const char* why;
        Command command;
    };
    const std::vector<Bad> cases = {
        {"empty name", make_command("")},
        {"leading space", make_command(" status")},
        {"trailing space", make_command("status ")},
        {"double space", make_command("time  get")},
        {"three words", make_command("a b c")},
        {"tab", make_command("sta\tus")},
        {"quote", make_command("sta\"tus")},
        {"id marker", make_command("#status")},
        {"id marker in the first word of two", make_command("#a b")},
        {"non-ASCII", make_command("st\xC3\xA1tus")},
        {"no handler", make_command("status", 0, 0, kFlagNone, nullptr)},
        {"min above max", make_command("status", 2, 1)},
        {"max above the argument limit",
         make_command("status", 0, static_cast<std::uint8_t>(kMaxArgs + 1))},
        {"unknown flag bit", make_command("status", 0, 0, 0x80)},
    };
    for (const Bad& bad : cases) {
        SCOPED_TRACE(bad.why);
        Registry registry;
        const Status status = registry.add(bad.command);
        ASSERT_FALSE(status.has_value());
        EXPECT_EQ(status.error().code, Errc::kBadArgs);
        EXPECT_TRUE(registry.all().empty());
    }
}

TEST(Registry, ArgumentLimitsAtTheEdgesAreAccepted) {
    Registry registry;
    EXPECT_TRUE(registry.add(make_command("a", 0, 0)).has_value());
    EXPECT_TRUE(
        registry.add(make_command("b", 0, static_cast<std::uint8_t>(kMaxArgs))).has_value());
    EXPECT_TRUE(registry
                    .add(make_command("c",
                                      static_cast<std::uint8_t>(kMaxArgs),
                                      static_cast<std::uint8_t>(kMaxArgs)))
                    .has_value());
    EXPECT_TRUE(
        registry.add(make_command("d", 0, 0, kFlagSensitive | kFlagDestructive | kFlagNeedsRadio))
            .has_value());
}

// Every gtest macro is an if/else, so loops of checks add up.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(Registry, WhenFullTheNextCommandIsNoSpaceAndTheOthersStillResolve) {
    std::vector<std::string> names;
    names.reserve(kMaxCommands);
    for (std::size_t i = 0; i < kMaxCommands; ++i) {
        names.push_back("c" + std::to_string(i));
    }
    Registry registry;
    for (const std::string& name : names) {
        ASSERT_TRUE(registry.add(make_command(name)).has_value()) << name;
    }
    EXPECT_EQ(registry.all().size(), kMaxCommands);

    const Status full = registry.add(make_command("one-more"));
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error().code, Errc::kNoSpace);
    EXPECT_EQ(registry.all().size(), kMaxCommands);

    EXPECT_NE(find_in(registry, {"c0"}), nullptr);
    EXPECT_NE(find_in(registry, {"c95"}), nullptr);
    EXPECT_EQ(find_in(registry, {"one-more"}), nullptr);
    // A duplicate is still reported as a duplicate, not as a full registry.
    EXPECT_EQ(registry.add(make_command("c0")).error().code, Errc::kBadArgs);
}

} // namespace
} // namespace qz::console
