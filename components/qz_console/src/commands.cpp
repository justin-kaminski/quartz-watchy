// Command catalog entry point (ARCHITECTURE.md section 16): `help`, `version`, `status`, and the
// registration of every built-in command. The other commands live in commands_*.cpp, each file
// exporting its table (commands_internal.hpp).
//
// `help` reads the same static tables the registry is filled from, so it needs no pointer to the
// Registry (handlers only receive the DeviceApi).
#include "commands_internal.hpp"
#include "qz/console/registry.hpp"
#include "qz/settings/settings.hpp"
#include "qz/time/tz.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console {
namespace {

using Args = std::span<const std::string_view>;

Status help(DeviceApi& api, Args args, JsonWriter& out);
Status version(DeviceApi& api, Args args, JsonWriter& out);
Status status(DeviceApi& api, Args args, JsonWriter& out);

constexpr auto kMetaCommands = std::to_array<Command>({
    {"help", "help [cmd]", "list the commands, or describe one", 0, 2, kFlagNone, help},
    {"version", "version", "firmware, build and protocol identity", 0, 0, kFlagNone, version},
    {"status",
     "status",
     "time, battery, connectivity, weather, screen and power state",
     0,
     0,
     kFlagNone,
     status},
});

constexpr std::size_t kTableCount = 5;

/// All built-in tables, in registration order.
std::array<std::span<const Command>, kTableCount> tables() noexcept {
    return {{kMetaCommands,
             detail::time_commands(),
             detail::device_commands(),
             detail::radio_commands(),
             detail::platform_commands()}};
}

/// "S", "D", "R" letters in a fixed order (ARCHITECTURE.md section 16 flag legend).
std::string_view flag_letters(std::span<char> out, const Command& command) noexcept {
    std::size_t len = 0;
    if ((command.flags & kFlagSensitive) != 0) {
        out[len++] = 'S';
    }
    if ((command.flags & kFlagDestructive) != 0) {
        out[len++] = 'D';
    }
    if ((command.flags & kFlagNeedsRadio) != 0) {
        out[len++] = 'R';
    }
    return {out.data(), len};
}

std::string_view first_word(std::string_view name) noexcept {
    return name.substr(0, name.find(' '));
}

Status help(DeviceApi& /*api*/, Args args, JsonWriter& out) {
    // The command asked about: one or two words joined by a space.
    std::array<char, 48> wanted{};
    std::size_t len = 0;
    for (const std::string_view word : args) {
        const std::size_t needed = word.size() + (len == 0 ? 0U : 1U);
        if (len + needed > wanted.size()) {
            return Errc::kNotFound;
        }
        if (len != 0) {
            wanted[len++] = ' ';
        }
        for (const char c : word) {
            wanted[len++] = c;
        }
    }
    const std::string_view query(wanted.data(), len);

    // Exact match: full description.
    for (const auto table : tables()) {
        for (const Command& command : table) {
            if (!query.empty() && command.name == query) {
                std::array<char, 4> flags{};
                out.field("name", command.name);
                out.field("usage", command.usage);
                out.field("help", command.help);
                out.field("flags", flag_letters(flags, command));
                return ok();
            }
        }
    }
    // No arguments: every command. One word that is a command family ("time"): its members.
    bool any = false;
    out.begin_array("cmds");
    for (const auto table : tables()) {
        for (const Command& command : table) {
            if (!query.empty() && first_word(command.name) != query) {
                continue;
            }
            std::array<char, 4> flags{};
            out.begin_object();
            out.field("name", command.name);
            out.field("usage", command.usage);
            out.field("flags", flag_letters(flags, command));
            out.end_object();
            any = true;
        }
    }
    out.end_array();
    return any ? ok() : Status(Errc::kNotFound);
}

Status version(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    const FirmwareIdentity identity = api.firmware();
    out.field("fw", identity.version);
    out.field("git", identity.git_hash);
    out.field("idf", identity.idf_version);
    out.key("build").begin_object();
    out.field_bool("radio", identity.radio_compiled);
    out.field("tzdata", time::tzdata_version());
    out.field("settings_schema", settings::kSchemaVersion);
    out.end_object();
    out.field("proto", kProtocolVersion);
    return ok();
}

Status status(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    api.write_status(out);
    return ok();
}

} // namespace

Status register_builtin_commands(Registry& registry) noexcept {
    for (const auto table : tables()) {
        for (const Command& command : table) {
            QZ_RETURN_IF_ERROR(registry.add(command));
        }
    }
    return ok();
}

} // namespace qz::console
