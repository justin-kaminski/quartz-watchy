// Command registry (registry.hpp): validated registration and longest-match lookup.
#include "qz/console/registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console {
namespace {

constexpr char kWordSeparator = ' ';
constexpr char kIdMarker = '#'; // a request starting with it is parsed as an id, never a command
constexpr unsigned kKnownFlags = kFlagSensitive | kFlagDestructive | kFlagNeedsRadio | kFlagUsbOnly;

/// A word the tokenizer can produce unquoted: printable ASCII, no space, no quote.
constexpr bool is_name_char(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte > 0x20U && byte < 0x7FU && c != '"';
}

constexpr bool is_valid_word(std::string_view word) noexcept {
    return !word.empty() && std::ranges::all_of(word, is_name_char);
}

/// "word" or "word word": single space, nothing else (the tokenizer could never match it).
constexpr bool is_valid_name(std::string_view name) noexcept {
    const std::size_t separator = name.find(kWordSeparator);
    const std::string_view first = name.substr(0, separator);
    const bool second_ok =
        separator == std::string_view::npos || is_valid_word(name.substr(separator + 1));
    return is_valid_word(first) && first.front() != kIdMarker && second_ok;
}

constexpr bool is_valid_definition(const Command& command) noexcept {
    const bool flags_known = (static_cast<unsigned>(command.flags) & ~kKnownFlags) == 0U;
    return is_valid_name(command.name) && command.handler != nullptr &&
           command.min_args <= command.max_args && command.max_args <= kMaxArgs && flags_known;
}

/// How many leading `tokens` the command name ("a" or "a b") matches; 0 = it does not match.
std::size_t words_matched(std::string_view name,
                          std::span<const std::string_view> tokens) noexcept {
    const std::size_t separator = name.find(kWordSeparator);
    if (separator == std::string_view::npos) {
        return !tokens.empty() && tokens[0] == name ? 1U : 0U;
    }
    const bool both = tokens.size() >= 2 && tokens[0] == name.substr(0, separator) &&
                      tokens[1] == name.substr(separator + 1);
    return both ? 2U : 0U;
}

} // namespace

Status Registry::add(const Command& cmd) noexcept {
    if (!is_valid_definition(cmd)) {
        return Errc::kBadArgs;
    }
    const bool duplicate = std::ranges::any_of(
        commands_, [&cmd](const Command& existing) { return existing.name == cmd.name; });
    if (duplicate) {
        return Errc::kBadArgs;
    }
    if (!commands_.push_back(cmd)) {
        return Errc::kNoSpace;
    }
    return ok();
}

const Command* Registry::find(std::span<const std::string_view> tokens,
                              std::size_t* words_used) const noexcept {
    const Command* best = nullptr;
    std::size_t best_words = 0;
    for (const Command& command : commands_) {
        const std::size_t words = words_matched(command.name, tokens);
        if (words > best_words) {
            best = &command;
            best_words = words;
        }
    }
    if (words_used != nullptr) {
        *words_used = best_words;
    }
    return best;
}

std::span<const Command> Registry::all() const noexcept {
    return commands_.span();
}

} // namespace qz::console
