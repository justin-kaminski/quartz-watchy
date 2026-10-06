// Catalog part 4: logs, diagnostics, self-test and the actuator / lifecycle commands.
// `sleep`, `reboot` and `factory-reset` only request the action: the app performs it after the
// response line has been sent (DeviceApi contract).
#include "commands_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::console::detail {
namespace {

using Args = std::span<const std::string_view>;
using Tune = CatalogTuning;

// ---- log ----

Status log_wakes(DeviceApi& api, Args args, JsonWriter& out) {
    std::int64_t wanted = Tune::kWakeRowsDefault;
    if (!args.empty()) {
        const Result<std::int64_t> parsed = parse_integer(args[0], 1, Tune::kWakeRowsMax);
        if (!parsed) {
            return Errc::kBadArgs;
        }
        wanted = *parsed;
    }
    const std::size_t total = api.wake_record_count();
    const std::size_t rows = std::min(total, static_cast<std::size_t>(wanted));
    out.field("total", static_cast<std::int64_t>(total));
    out.begin_array("wakes"); // oldest first, the most recent `rows` records
    for (std::size_t i = total - rows; i < total; ++i) {
        const model::WakeRecord record = api.wake_record(i);
        out.begin_object();
        out.field("utc_s", static_cast<std::int64_t>(record.start_utc_s));
        out.field("awake_ms", record.awake_ms);
        out.field("cause", wake_cause_name(record.cause));
        out.field("flags", record.flags);
        out.field("battery_mv", record.battery_mv);
        out.field("power", power_level_name(record.power));
        out.key("error");
        const std::string_view token = stored_error_token(record.error);
        if (token.empty()) {
            out.null();
        } else {
            out.str(token);
        }
        out.field("steps_delta", record.steps_delta);
        out.end_object();
    }
    out.end_array();
    return ok();
}

Status log_clear(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    api.clear_wake_log();
    out.field_bool("cleared", true);
    return ok();
}

struct LevelName {
    std::string_view name;
    LogLevel level;
};
constexpr std::array<LevelName, 5> kLevels{{
    {"error", LogLevel::kError},
    {"warn", LogLevel::kWarn},
    {"info", LogLevel::kInfo},
    {"debug", LogLevel::kDebug},
    {"verbose", LogLevel::kVerbose},
}};

Status log_set_level(DeviceApi& api, Args args, JsonWriter& out) {
    const auto* const found = std::ranges::find(kLevels, args[0], &LevelName::name);
    if (found == kLevels.end()) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(api.set_log_level(found->level));
    out.field("level", found->name);
    return ok();
}

// ---- diag / selftest ----

Status diag(DeviceApi& api, Args args, JsonWriter& out) {
    return api.write_diag(args[0], out);
}

Status selftest_list(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    api.list_selftests(out);
    return ok();
}

Status selftest_run(DeviceApi& api, Args args, JsonWriter& out) {
    return api.run_selftests(args.empty() ? std::string_view{} : args[0], out);
}

// ---- actuators / lifecycle ----

Status vibrate(DeviceApi& api, Args args, JsonWriter& out) {
    std::int64_t ms = Tune::kVibrateDefaultMs;
    if (!args.empty()) {
        const Result<std::int64_t> parsed = parse_integer(args[0], 1, Tune::kVibrateMaxMs);
        if (!parsed) {
            return Errc::kBadArgs;
        }
        ms = *parsed;
    }
    QZ_RETURN_IF_ERROR(api.vibrate(static_cast<std::uint16_t>(ms)));
    out.field("ms", ms);
    return ok();
}

Status sleep_cmd(DeviceApi& api, Args args, JsonWriter& out) {
    const Result<std::int64_t> seconds = parse_integer(args[0], 1, Tune::kSleepMaxS);
    if (!seconds) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(api.request_sleep(static_cast<std::uint32_t>(*seconds)));
    out.field("seconds", *seconds);
    return ok();
}

Status reboot(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.request_reboot());
    out.field_bool("rebooting", true);
    return ok();
}

Status factory_reset(DeviceApi& api, Args args, JsonWriter& out) {
    if (args[0] != "confirm") {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(api.factory_reset());
    out.field_bool("reset", true);
    return ok();
}

constexpr auto kCommands = std::to_array<Command>({
    {"log wakes",
     "log wakes [n]",
     "the last n wake records, oldest first",
     0,
     1,
     kFlagNone,
     log_wakes},
    {"log clear", "log clear", "erase the wake log", 0, 0, kFlagNone, log_clear},
    {"log level",
     "log level <error|warn|info|debug|verbose>",
     "set the runtime log threshold",
     1,
     1,
     kFlagNone,
     log_set_level},
    {"diag",
     "diag <info|power|radio|rtc|nvs|clock|sensors>",
     "diagnostic page",
     1,
     1,
     kFlagNone,
     diag},
    {"selftest list", "selftest list", "available self-tests", 0, 0, kFlagNone, selftest_list},
    {"selftest run", "selftest run [suite|test]", "run self-tests", 0, 1, kFlagNone, selftest_run},
    {"vibrate", "vibrate [ms]", "buzz the motor (1..1000 ms)", 0, 1, kFlagNone, vibrate},
    {"sleep",
     "sleep <seconds>",
     "deep sleep for 1..3600 s even while tethered (answers first)",
     1,
     1,
     kFlagNone,
     sleep_cmd},
    {"reboot", "reboot", "restart the watch (answers first)", 0, 0, kFlagNone, reboot},
    {"factory-reset",
     "factory-reset confirm",
     "erase settings, credentials and history",
     1,
     1,
     kFlagDestructive,
     factory_reset},
});

} // namespace

std::span<const Command> platform_commands() noexcept {
    return kCommands;
}

} // namespace qz::console::detail
