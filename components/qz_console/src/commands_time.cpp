// Catalog part 1: `time`, `tz` and `settings` commands (ARCHITECTURE.md section 16).
// Handlers write into an already-open JSON object; the dispatcher frames the line.
#include "commands_internal.hpp"
#include "qz/settings/settings.hpp"
#include "qz/time/civil.hpp"
#include "qz/time/timekeeper.hpp"
#include "qz/time/tz.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace qz::console::detail {
namespace {

using Args = std::span<const std::string_view>;

constexpr std::int64_t kUsPerS = time::kUsPerSecond;

/// Floor division: an RTC instant before 1970 must not round toward zero.
std::int64_t floor_seconds(time::UnixMicros us) noexcept {
    const std::int64_t quotient = us / kUsPerS;
    return (us % kUsPerS < 0) ? quotient - 1 : quotient;
}

/// Writes `key` as an ISO-8601 UTC string, or null when `t` is 0 (never).
void write_optional_utc(JsonWriter& out, std::string_view key, time::UnixSeconds t) {
    out.key(key);
    if (t == 0) {
        out.null();
        return;
    }
    std::array<char, 24> text{};
    out.str(format_utc_iso(text, t));
}

// ---- time ----

void write_time_fields(JsonWriter& out, const TimeInfo& info) {
    out.field_bool("valid", info.valid);
    out.key("utc");
    if (!info.valid) {
        out.null();
        out.key("local").null();
        out.key("unix").null();
    } else {
        std::array<char, 32> text{};
        const time::UnixSeconds seconds = floor_seconds(info.utc_us);
        out.str(format_utc_iso(text, seconds));
        out.key("local").str(format_local_iso(text, info.local));
        out.field("unix", seconds);
    }
    out.field("source", info.source.empty() ? std::string_view("none") : info.source);
    out.field("drift_ppb", info.drift_ppb);
    write_optional_utc(out, "last_sync", info.last_sync_utc);
}

Status time_get(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_time_fields(out, api.time_info());
    return ok();
}

Status time_set(DeviceApi& api, Args args, JsonWriter& out) {
    const Result<time::UnixSeconds> parsed = time::parse_iso8601(args[0], api.timezone());
    if (!parsed) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(api.set_time_utc(*parsed));
    write_time_fields(out, api.time_info());
    return ok();
}

Status time_drift(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    const TimeInfo info = api.time_info();
    out.field("drift_ppb", info.drift_ppb);
    out.field("source", info.source.empty() ? std::string_view("none") : info.source);
    out.field_bool("valid", info.valid);
    write_optional_utc(out, "last_sync", info.last_sync_utc);
    return ok();
}

// ---- tz ----

void write_zone_fields(JsonWriter& out, const settings::Settings& settings) {
    out.field("name", settings.tz_name.view());
    out.field("posix", settings.tz_posix.view());
}

Status tz_list(DeviceApi& /*api*/, Args args, JsonWriter& out) {
    const std::string_view filter = args.empty() ? std::string_view{} : args[0];
    std::int64_t count = 0;
    out.begin_array("zones");
    for (const time::TzEntry& zone : time::builtin_zones()) {
        if (!contains_ignore_case(zone.name, filter) && !contains_ignore_case(zone.label, filter)) {
            continue;
        }
        out.begin_object();
        out.field("name", zone.name);
        out.field("label", zone.label);
        out.end_object();
        ++count;
    }
    out.end_array();
    out.field("count", count);
    return ok();
}

Status tz_get(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_zone_fields(out, api.current_settings());
    out.field_bool("dst", api.timezone().has_dst());
    return ok();
}

Status tz_set(DeviceApi& api, Args args, JsonWriter& out) {
    if (time::find_zone(args[0]) == nullptr) {
        return Errc::kNotFound;
    }
    QZ_RETURN_IF_ERROR(api.apply_setting(settings::Key::kTimeZone, args[0]));
    write_zone_fields(out, api.current_settings());
    out.field_bool("dst", api.timezone().has_dst());
    return ok();
}

// ---- settings ----

/// key, typed value, type and the legal values of one setting.
void write_setting(JsonWriter& out,
                   const settings::Settings& values,
                   const settings::KeyInfo& info) {
    std::array<char, 80> text{};
    const std::string_view formatted(text.data(), settings::format_value(values, info.key, text));
    out.field("key", info.name);
    out.key("value");
    switch (info.type) {
        case settings::ValueType::kBool:
            out.boolean(formatted == "on");
            break;
        case settings::ValueType::kUInt: {
            const Result<std::int64_t> number =
                parse_integer(formatted, 0, std::numeric_limits<std::int64_t>::max());
            if (number) {
                out.num(*number);
            } else {
                out.str(formatted);
            }
            break;
        }
        case settings::ValueType::kEnum:
        case settings::ValueType::kDegrees:
        case settings::ValueType::kZoneName:
            out.str(formatted);
            break;
    }
    switch (info.type) {
        case settings::ValueType::kBool:
            out.field("type", "bool");
            break;
        case settings::ValueType::kEnum:
            out.field("type", "enum");
            break;
        case settings::ValueType::kUInt:
            out.field("type", "uint");
            break;
        case settings::ValueType::kDegrees:
            out.field("type", "degrees");
            break;
        case settings::ValueType::kZoneName:
            out.field("type", "zone");
            break;
    }
    const bool has_choices = info.type != settings::ValueType::kBool && !info.choices.empty();
    if (has_choices) {
        out.begin_array("choices");
        for (const std::string_view choice : info.choices) {
            out.str(choice);
        }
        out.end_array();
    } else if (info.type == settings::ValueType::kUInt ||
               info.type == settings::ValueType::kDegrees) {
        out.key("range").begin_object();
        out.field("min", info.min);
        out.field("max", info.max);
        out.end_object();
    }
}

Status settings_list(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    const settings::Settings& values = api.current_settings();
    out.begin_array("settings");
    for (const settings::KeyInfo& info : settings::schema()) {
        out.begin_object();
        write_setting(out, values, info);
        out.field("help", info.help);
        out.end_object();
    }
    out.end_array();
    return ok();
}

Status settings_get(DeviceApi& api, Args args, JsonWriter& out) {
    const settings::KeyInfo* info = settings::find_key(args[0]);
    if (info == nullptr) {
        return Errc::kNotFound;
    }
    write_setting(out, api.current_settings(), *info);
    out.field("help", info->help);
    return ok();
}

Status settings_set(DeviceApi& api, Args args, JsonWriter& out) {
    const settings::KeyInfo* info = settings::find_key(args[0]);
    if (info == nullptr) {
        return Errc::kNotFound;
    }
    QZ_RETURN_IF_ERROR(api.apply_setting(info->key, args[1]));
    write_setting(out, api.current_settings(), *info);
    return ok();
}

Status settings_reset(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.reset_settings());
    out.field_bool("reset", true);
    return ok();
}

constexpr auto kCommands = std::to_array<Command>({
    {"time get", "time get", "wall-clock time, source and drift", 0, 0, kFlagNone, time_get},
    {"time set",
     "time set <ISO-8601 local or Z>",
     "set the wall clock (local time in the current zone unless Z or an offset is given)",
     1,
     1,
     kFlagNone,
     time_set},
    {"time drift",
     "time drift",
     "measured RTC drift (ppb) and last sync",
     0,
     0,
     kFlagNone,
     time_drift},
    {"tz list",
     "tz list [filter]",
     "built-in time zones, optionally filtered",
     0,
     1,
     kFlagNone,
     tz_list},
    {"tz get", "tz get", "current time zone", 0, 0, kFlagNone, tz_get},
    {"tz set", "tz set <IANA name>", "select a built-in time zone", 1, 1, kFlagNone, tz_set},
    {"settings list",
     "settings list",
     "every setting with value and range",
     0,
     0,
     kFlagNone,
     settings_list},
    {"settings get", "settings get <key>", "one setting", 1, 1, kFlagNone, settings_get},
    {"settings set",
     "settings set <key> <value>",
     "change one setting (validated like the UI)",
     2,
     2,
     kFlagNone,
     settings_set},
    {"settings reset",
     "settings reset",
     "restore every setting to its default",
     0,
     0,
     kFlagDestructive,
     settings_reset},
});

} // namespace

std::span<const Command> time_commands() noexcept {
    return kCommands;
}

} // namespace qz::console::detail
