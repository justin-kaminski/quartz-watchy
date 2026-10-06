// Command registry + dispatcher shared by the device binding and the host simulator.
#pragma once

#include "qz/console/device_api.hpp"
#include "qz/console/protocol.hpp"
#include "qz/core/containers.hpp"
#include "qz/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console {

// NOLINTNEXTLINE(cppcoreguidelines-use-enum-class): bit flags combined with |
enum CommandFlag : std::uint8_t {
    kFlagNone = 0,
    kFlagSensitive = 1U << 0U,   ///< arguments never logged/echoed; history disabled
    kFlagDestructive = 1U << 1U, ///< requires a literal "confirm" or is listed as D in docs
    kFlagNeedsRadio = 1U << 2U,  ///< kUnsupported when the radio is compiled out
};

/// Handler writes its result fields into an already-open JSON object.
using Handler = Status (*)(DeviceApi& api, std::span<const std::string_view> args, JsonWriter& out);

struct Command {
    std::string_view name;  ///< "status", "time set", ... (1-2 words)
    std::string_view usage; ///< "time set <ISO-8601>"
    std::string_view help;
    std::uint8_t min_args;
    std::uint8_t max_args;
    std::uint8_t flags;
    Handler handler;
};

inline constexpr std::size_t kMaxCommands = 96;

class Registry {
public:
    /// kBadArgs on duplicate name, kNoSpace when full.
    Status add(const Command& cmd) noexcept;
    /// Longest match: two-word name first, then one-word.
    [[nodiscard]] const Command* find(std::span<const std::string_view> tokens,
                                      std::size_t* words_used) const noexcept;
    [[nodiscard]] std::span<const Command> all() const noexcept;

private:
    StaticVector<Command, kMaxCommands> commands_;
};

/// Registers the full v1 catalog (ARCHITECTURE.md section 16).
Status register_builtin_commands(Registry& registry) noexcept;

/// Parses one line, runs the command, formats exactly one response line into `response`.
/// Never logs arguments of kFlagSensitive commands. Not thread-safe (app task).
class Dispatcher {
public:
    Dispatcher(const Registry& registry, DeviceApi& api, bool radio_compiled) noexcept;
    std::string_view handle_line(std::span<char> line, std::span<char> response) noexcept;

private:
    const Registry& registry_;
    DeviceApi& api_;
    bool radio_compiled_;
};

} // namespace qz::console
