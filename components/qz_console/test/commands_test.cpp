// Command catalog (register_builtin_commands, commands_*.cpp), driven through the real dispatcher
// against FakeDeviceApi. Every response is framed by the dispatcher and checked as a complete
// protocol line with strictly valid JSON.
//
// Structure:
//  * kOkCases / kErrCases: at least one success and one error per command, with the exact answer.
//  * Catalog.EveryRegisteredCommandIsTested: walks the registry and fails when a command lacks a
//    success case or an error case (and when a case names a command that does not exist).
//  * Focused tests: flags, secrecy of Wi-Fi credentials, display dump, radio gating, behaviours.
#include "fake_device_api.hpp"
#include "json_check.hpp"
#include "qz/console/registry.hpp"
#include "qz/core/crc32.hpp"
#include "qz/time/tz.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// NOLINTBEGIN(readability-function-cognitive-complexity) -- gtest macros inflate the metric

namespace qz::console {
namespace {

using test::FakeDeviceApi;

constexpr std::int64_t kClock = 1'700'000'000; // 2023-11-14T22:13:20Z

// ---- harness ---------------------------------------------------------------------------------

struct Reply {
    std::string line;
    std::string status; ///< "OK" or "ERR"
    std::string code;   ///< error token, empty for OK
    std::string json;   ///< the object
};

Reply split_reply(std::string_view line) {
    Reply reply;
    reply.line = std::string(line);
    // "@QZ1 <id> OK <json>" | "@QZ1 <id> ERR <code> <json>"
    std::string_view rest = line.substr(std::string_view("@QZ1 ").size());
    auto take = [&rest]() {
        const std::size_t space = rest.find(' ');
        const std::string_view word = rest.substr(0, space);
        rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        return std::string(word);
    };
    (void)take(); // the id
    reply.status = take();
    if (reply.status == "ERR") {
        reply.code = take();
    }
    reply.json = std::string(rest);
    return reply;
}

/// Framing and JSON rules every answer must meet.
void expect_well_formed(const Reply& reply) {
    SCOPED_TRACE(reply.line);
    ASSERT_TRUE(reply.line.starts_with("@QZ1 "));
    EXPECT_EQ(reply.line.find('\n'), std::string::npos);
    EXPECT_EQ(reply.line.find('\r'), std::string::npos);
    EXPECT_LE(reply.line.size(), kMaxResponseBytes);
    EXPECT_TRUE(reply.status == "OK" || reply.status == "ERR");
    EXPECT_TRUE(test::is_valid_utf8(reply.line));
    EXPECT_TRUE(test::parse_json(reply.json).valid) << "not valid JSON: " << reply.json;
    ASSERT_FALSE(reply.json.empty());
    EXPECT_EQ(reply.json.front(), '{');
    EXPECT_EQ(reply.json.back(), '}');
}

struct Rig {
    explicit Rig(bool radio_compiled = true) : dispatcher(registry, api, radio_compiled) {
        const Status status = register_builtin_commands(registry);
        EXPECT_TRUE(status.has_value());
    }

    Reply run(std::string_view line) {
        test::ExactBuffer request(line);
        std::vector<char> response(kMaxResponseBytes);
        Reply reply = split_reply(dispatcher.handle_line(request.span(), response));
        expect_well_formed(reply);
        return reply;
    }

    Registry registry;
    FakeDeviceApi api;
    Dispatcher dispatcher;
};

// ---- case tables -----------------------------------------------------------------------------

using Setup = void (*)(FakeDeviceApi&);

void with_clock(FakeDeviceApi& api) {
    api.set_clock(kClock);
}
void with_chicago(FakeDeviceApi& api) {
    ASSERT_TRUE(api.apply_setting(settings::Key::kTimeZone, "America/Chicago").has_value());
}
void with_sync_history(FakeDeviceApi& api) {
    SyncInfo info;
    info.indicator = model::SyncIndicator::kLastFailed;
    info.last_ok = 1'699'990'000;
    info.fail_streak = 2;
    info.last_error = static_cast<std::uint8_t>(static_cast<unsigned>(Errc::kTimeout) + 1U);
    info.next_time_sync = 1'700'000'600;
    api.set_sync_info(info);
}
void with_drift(FakeDeviceApi& api) {
    TimeInfo info;
    info.valid = true;
    info.utc_us = kClock * time::kUsPerSecond;
    info.source = "sntp";
    info.drift_ppb = -1234;
    info.last_sync_utc = kClock;
    api.set_time_info(info);
}
void with_provisioning(FakeDeviceApi& api) {
    api.provisioning = true;
}
void with_wifi(FakeDeviceApi& api) {
    api.set_ssid(FixedString<32>("Home"), true);
}
void with_cold_clock_off_radio(FakeDeviceApi& api) {
    api.identity.radio_compiled = false;
}
void with_small_frame(FakeDeviceApi& api) {
    api.frame_bytes = 100;
}
void fail_busy(FakeDeviceApi& api) {
    api.fail = Error{Errc::kBusy};
}
void fail_io(FakeDeviceApi& api) {
    api.fail = Error{Errc::kIo};
}
void fail_timeout(FakeDeviceApi& api) {
    api.fail = Error{Errc::kTimeout};
}
void fail_battery_low(FakeDeviceApi& api) {
    api.fail = Error{Errc::kBatteryLow};
}

enum class Match : std::uint8_t { kExact, kPrefix };

struct OkCase {
    std::string_view command; ///< registry name this case covers
    std::string_view line;
    std::string_view json; ///< expected object (whole, or its start for kPrefix)
    Setup setup = nullptr;
    Match match = Match::kExact;
};

struct ErrCase {
    std::string_view command;
    std::string_view line;
    std::string_view code;
    Setup setup = nullptr;
};

// Expected values below were reviewed by hand against ARCHITECTURE.md section 16, not copied
// blindly from output.
const OkCase kOkCases[] = {
    {"help",
     "help",
     R"j({"cmds":[{"name":"help","usage":"help [cmd]","flags":""},{"name":"version")j",
     nullptr,
     Match::kPrefix},
    {"version",
     "version",
     R"j({"fw":"1.2.3","git":"abc1234","idf":"v6.1","build":{"radio":true,"tzdata":")j",
     nullptr,
     Match::kPrefix},
    {"status", "status", R"j({"valid":false,"steps":4321,"battery":{"mv":3900},"screen":"face"})j"},
    {"time get",
     "time get",
     R"j({"valid":true,"utc":"2023-11-14T22:13:20Z","local":"2023-11-14T22:13:20+00:00","unix":1700000000,"source":"sntp","drift_ppb":0,"last_sync":null})j",
     with_clock},
    {"time set",
     "time set 2026-03-08T01:30:00Z",
     R"j({"valid":true,"utc":"2026-03-08T01:30:00Z","local":"2026-03-08T01:30:00+00:00","unix":1772933400,"source":"console","drift_ppb":0,"last_sync":null})j"},
    {"time drift",
     "time drift",
     R"j({"drift_ppb":-1234,"source":"sntp","valid":true,"last_sync":"2023-11-14T22:13:20Z"})j",
     with_drift},
    {"tz list",
     "tz list berlin",
     R"j({"zones":[{"name":"Europe/Berlin","label":"Berlin (UTC+1)"}],"count":1})j"},
    {"tz get", "tz get", R"j({"name":"UTC","posix":"UTC0","dst":false})j"},
    {"tz set",
     "tz set America/Chicago",
     R"j({"name":"America/Chicago","posix":"CST6CDT,M3.2.0,M11.1.0","dst":true})j"},
    {"settings list",
     "settings list",
     R"j({"settings":[{"key":"tfmt","value":"24h","type":"enum","choices":["24h","12h"],"help":)j",
     nullptr,
     Match::kPrefix},
    {"settings get",
     "settings get goal",
     R"j({"key":"goal","value":0,"type":"uint","range":{"min":0,"max":50000},"help":"daily step goal, multiple of 500; 0 = off"})j"},
    {"settings set",
     "settings set goal 7500",
     R"j({"key":"goal","value":7500,"type":"uint","range":{"min":0,"max":50000}})j"},
    {"settings reset", "settings reset", R"j({"reset":true})j"},
    {"btn", "btn menu hold", R"j({"screen":"menu"})j"},
    {"steps get", "steps get", R"j({"today":4321,"goal":8000})j"},
    {"steps history",
     "steps history",
     R"j({"today":4321,"goal":8000,"history":[{"date":"2024-10-04","steps":9000},{"date":"2024-10-03","steps":7500},{"date":"2024-10-02","steps":0}]})j"},
    {"steps inject", "steps inject -321", R"j({"today":4000,"goal":8000})j"},
    {"steps reset-today", "steps reset-today", R"j({"today":0,"goal":8000})j"},
    {"battery get",
     "battery get",
     R"j({"mv":3900,"pct":70,"state":"normal","usb":false,"charging":false,"valid":true,"faked":false})j"},
    {"battery fake",
     "battery fake 3300",
     R"j({"mv":3300,"pct":70,"state":"normal","usb":false,"charging":false,"valid":true,"faked":true})j"},
    {"weather get", "weather get", R"j({"report":null,"age_s":0,"freshness":"hidden"})j"},
    {"weather fake",
     "weather fake 215 partly_cloudy 250 120",
     R"j({"report":{"temp_dc":215,"condition":"partly_cloudy","high_dc":250,"low_dc":120,"fetched":1700000000,"faked":true},"age_s":0,"freshness":"fresh"})j",
     with_clock},
    {"weather clear", "weather clear", R"j({"report":null,"age_s":0,"freshness":"hidden"})j"},
    {"weather fetch", "weather fetch", R"j({"report":null,"age_s":0,"freshness":"hidden"})j"},
    {"wifi status",
     "wifi status",
     R"j({"configured":true,"ssid":"Home","has_password":true})j",
     with_wifi},
    {"wifi set",
     "wifi set Home hunter2hunter2",
     R"j({"configured":true,"ssid":"Home","has_password":true})j"},
    {"wifi clear",
     "wifi clear",
     R"j({"configured":false,"ssid":null,"has_password":false})j",
     with_wifi},
    {"sync now",
     "sync now weather",
     R"j({"time":false,"weather":true,"indicator":"ok","last_ok":1700000000,"fail_streak":0,"last_error":null,"next_time_sync":1700086400,"next_weather":1700003600})j"},
    {"sync status",
     "sync status",
     R"j({"indicator":"failed","last_ok":1699990000,"fail_streak":2,"last_error":"timeout","next_time_sync":1700000600,"next_weather":null})j",
     with_sync_history},
    {"provision start", "provision start", R"j({"ssid":"QZ-A1B2","expires_s":300})j"},
    {"provision stop", "provision stop", R"j({"stopped":true})j", with_provisioning},
    {"display refresh",
     "display refresh full",
     R"j({"mode":"full","crc32":"1abd04d4"})j"}, // zlib CRC-32 of the fake's 5000-byte pattern
    {"display dump",
     "display dump",
     R"j({"w":200,"h":200,"fmt":"1bpp-msb","crc32":"1abd04d4","b64":")j",
     nullptr,
     Match::kPrefix},
    {"display crc", "display crc", R"j({"crc32":"1abd04d4"})j"},
    {"screen list", "screen list", R"j({"screens":["face","menu","about"],"current":"face"})j"},
    {"screen get", "screen get", R"j({"screen":"face"})j"},
    {"screen show", "screen show about", R"j({"screen":"about"})j"},
    {"face list",
     "face list",
     R"j({"faces":[{"id":0,"name":"classic"},{"id":3,"name":"minimal"}],"current":0})j"},
    {"face set", "face set minimal", R"j({"id":3,"name":"minimal"})j"},
    {"log wakes",
     "log wakes 2",
     R"j({"total":3,"wakes":[{"utc_s":1000001,"awake_ms":121,"cause":"button","flags":0,"battery_mv":3900,"power":"normal","error":null,"steps_delta":0},{"utc_s":1000002,"awake_ms":122,"cause":"timer","flags":0,"battery_mv":3900,"power":"normal","error":"timeout","steps_delta":0}]})j"},
    {"log clear", "log clear", R"j({"cleared":true})j"},
    {"log level", "log level debug", R"j({"level":"debug"})j"},
    {"diag", "diag clock", R"j({"page":"clock","ok":true})j"},
    {"selftest list", "selftest list", R"j({"tests":["battery.thresholds","power.policy"]})j"},
    {"selftest run",
     "selftest run battery",
     R"j({"results":[{"name":"battery.thresholds","status":"pass","ms":3,"detail":""}]})j"},
    {"vibrate", "vibrate 80", R"j({"ms":80})j"},
    {"sleep", "sleep 30", R"j({"seconds":30})j"},
    {"reboot", "reboot", R"j({"rebooting":true})j"},
    {"factory-reset", "factory-reset confirm", R"j({"reset":true})j"},
};

const ErrCase kErrCases[] = {
    {"help", "help nosuchcmd", "not_found"},
    {"version", "version now", "bad_args"},
    {"status", "status all", "bad_args"},
    {"time get", "time get now", "bad_args"},
    {"time set", "time set yesterday", "bad_args"},
    {"time drift", "time drift now", "bad_args"},
    {"tz list", "tz list a b", "bad_args"},
    {"tz get", "tz get utc", "bad_args"},
    {"tz set", "tz set Mars/Olympus_Mons", "not_found"},
    {"settings list", "settings list all", "bad_args"},
    {"settings get", "settings get nosuchkey", "not_found"},
    {"settings set", "settings set goal 123", "bad_args"},
    {"settings reset", "settings reset", "io", fail_io},
    {"btn", "btn fire", "bad_args"},
    {"steps get", "steps get 7", "bad_args"},
    {"steps history", "steps history 7", "bad_args"},
    {"steps inject", "steps inject lots", "bad_args"},
    {"steps reset-today", "steps reset-today", "busy", fail_busy},
    {"battery get", "battery get 1", "bad_args"},
    {"battery fake", "battery fake 100", "bad_args"},
    {"weather get", "weather get now", "bad_args"},
    {"weather fake", "weather fake 215 rain 250", "bad_args"},
    {"weather clear", "weather clear", "io", fail_io},
    {"weather fetch", "weather fetch", "timeout", fail_timeout},
    {"wifi status", "wifi status all", "bad_args"},
    {"wifi set", "wifi set Home short", "bad_args"},
    {"wifi clear", "wifi clear", "io", fail_io},
    {"sync now", "sync now moon", "bad_args"},
    {"sync status", "sync status all", "bad_args"},
    {"provision start", "provision start", "busy", fail_busy},
    {"provision stop", "provision stop", "invalid_state"},
    {"display refresh", "display refresh sometimes", "bad_args"},
    {"display dump", "display dump", "internal", with_small_frame},
    {"display crc", "display crc now", "bad_args"},
    {"screen list", "screen list all", "bad_args"},
    {"screen get", "screen get now", "bad_args"},
    {"screen show", "screen show nowhere", "not_found"},
    {"face list", "face list all", "bad_args"},
    {"face set", "face set 9", "not_found"},
    {"log wakes", "log wakes 0", "bad_args"},
    {"log clear", "log clear all", "bad_args"},
    {"log level", "log level loud", "bad_args"},
    {"diag", "diag nothing", "not_found"},
    {"selftest list", "selftest list all", "bad_args"},
    {"selftest run", "selftest run nothing", "not_found"},
    {"vibrate", "vibrate 1001", "bad_args"},
    {"sleep", "sleep 0", "bad_args"},
    {"reboot", "reboot", "io", fail_io},
    {"factory-reset", "factory-reset", "bad_args"},
};

TEST(Catalog, OkCasesAnswerExactly) {
    for (const OkCase& c : kOkCases) {
        SCOPED_TRACE(std::string(c.command) + ": " + std::string(c.line));
        Rig rig;
        if (c.setup != nullptr) {
            c.setup(rig.api);
        }
        const Reply reply = rig.run(c.line);
        EXPECT_EQ(reply.status, "OK") << reply.line;
        if (c.match == Match::kExact) {
            EXPECT_EQ(reply.json, c.json);
        } else {
            EXPECT_TRUE(reply.json.starts_with(c.json)) << reply.json;
        }
    }
}

TEST(Catalog, ErrorCasesAnswerWithTheCode) {
    for (const ErrCase& c : kErrCases) {
        SCOPED_TRACE(std::string(c.command) + ": " + std::string(c.line));
        Rig rig;
        if (c.setup != nullptr) {
            c.setup(rig.api);
        }
        const Reply reply = rig.run(c.line);
        EXPECT_EQ(reply.status, "ERR") << reply.line;
        EXPECT_EQ(reply.code, c.code);
        EXPECT_NE(reply.json.find(R"("msg":")"), std::string::npos) << reply.json;
    }
}

// ---- registry introspection -------------------------------------------------------------------

/// A case belongs to `command` when its line is that command followed by nothing or a space.
bool line_runs(std::string_view line, std::string_view command) {
    return line.starts_with(command) &&
           (line.size() == command.size() || line[command.size()] == ' ');
}

TEST(Catalog, EveryRegisteredCommandIsTested) {
    Registry registry;
    ASSERT_TRUE(register_builtin_commands(registry).has_value());
    ASSERT_FALSE(registry.all().empty());
    for (const Command& command : registry.all()) {
        const auto has_ok = std::ranges::any_of(kOkCases, [&command](const OkCase& c) {
            return c.command == command.name && line_runs(c.line, command.name);
        });
        const auto has_err = std::ranges::any_of(kErrCases, [&command](const ErrCase& c) {
            return c.command == command.name && line_runs(c.line, command.name);
        });
        EXPECT_TRUE(has_ok) << "command without a success test: " << command.name;
        EXPECT_TRUE(has_err) << "command without an error test: " << command.name;
    }
}

TEST(Catalog, NoCaseNamesAnUnregisteredCommand) {
    Registry registry;
    ASSERT_TRUE(register_builtin_commands(registry).has_value());
    auto registered = [&registry](std::string_view name) {
        return std::ranges::any_of(registry.all(),
                                   [name](const Command& c) { return c.name == name; });
    };
    for (const OkCase& c : kOkCases) {
        EXPECT_TRUE(registered(c.command)) << c.command;
    }
    for (const ErrCase& c : kErrCases) {
        EXPECT_TRUE(registered(c.command)) << c.command;
    }
}

TEST(Catalog, RegistersTheWholeV1Catalog) {
    Registry registry;
    ASSERT_TRUE(register_builtin_commands(registry).has_value());
    // Every row of ARCHITECTURE.md section 16 (plus `log level` from section 19).
    constexpr std::array kExpected =
        std::to_array<std::string_view>({"help",           "version",
                                         "status",         "time get",
                                         "time set",       "time drift",
                                         "tz list",        "tz get",
                                         "tz set",         "settings list",
                                         "settings get",   "settings set",
                                         "settings reset", "btn",
                                         "steps get",      "steps history",
                                         "steps inject",   "steps reset-today",
                                         "battery get",    "battery fake",
                                         "weather get",    "weather fake",
                                         "weather clear",  "weather fetch",
                                         "wifi status",    "wifi set",
                                         "wifi clear",     "sync now",
                                         "sync status",    "provision start",
                                         "provision stop", "display refresh",
                                         "display dump",   "display crc",
                                         "screen list",    "screen get",
                                         "screen show",    "face list",
                                         "face set",       "log wakes",
                                         "log clear",      "log level",
                                         "diag",           "selftest list",
                                         "selftest run",   "vibrate",
                                         "sleep",          "reboot",
                                         "factory-reset"});
    EXPECT_EQ(registry.all().size(), kExpected.size());
    for (const std::string_view name : kExpected) {
        std::array<std::string_view, 2> tokens{};
        const std::size_t space = name.find(' ');
        tokens[0] = name.substr(0, space);
        std::size_t count = 1;
        if (space != std::string_view::npos) {
            tokens[1] = name.substr(space + 1);
            count = 2;
        }
        std::size_t words = 0;
        const Command* found =
            registry.find(std::span<const std::string_view>(tokens.data(), count), &words);
        ASSERT_NE(found, nullptr) << name;
        EXPECT_EQ(found->name, name);
    }
}

TEST(Catalog, RegisteringTwiceIsRejectedAsDuplicate) {
    Registry registry;
    ASSERT_TRUE(register_builtin_commands(registry).has_value());
    const Status again = register_builtin_commands(registry);
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, Errc::kBadArgs);
}

TEST(Catalog, FlagsFollowTheCatalogLegend) {
    Registry registry;
    ASSERT_TRUE(register_builtin_commands(registry).has_value());
    // S sensitive, D destructive, R needs radio (ARCHITECTURE.md section 16). Every wifi command is
    // sensitive: nothing about credentials is ever logged or echoed.
    const std::map<std::string_view, std::uint8_t> special = {
        {"settings reset", kFlagDestructive},
        {"steps reset-today", kFlagDestructive},
        {"weather fetch", kFlagNeedsRadio},
        {"wifi status", kFlagSensitive},
        {"wifi set", kFlagSensitive},
        {"wifi clear", kFlagSensitive | kFlagDestructive},
        {"sync now", kFlagNeedsRadio},
        {"provision start", kFlagNeedsRadio},
        {"factory-reset", kFlagDestructive},
    };
    for (const Command& command : registry.all()) {
        const auto found = special.find(command.name);
        const std::uint8_t expected =
            found == special.end() ? std::uint8_t{kFlagNone} : found->second;
        EXPECT_EQ(command.flags, expected) << command.name;
        EXPECT_FALSE(command.usage.empty()) << command.name;
        EXPECT_FALSE(command.help.empty()) << command.name;
        EXPECT_TRUE(command.usage.starts_with(command.name)) << command.name;
        EXPECT_LE(command.min_args, command.max_args) << command.name;
    }
}

TEST(Catalog, HelpListsEveryCommandAndDescribesOne) {
    Rig rig;
    const Reply all = rig.run("help");
    ASSERT_EQ(all.status, "OK");
    std::size_t names = 0;
    for (std::size_t pos = all.json.find(R"({"name":")"); pos != std::string::npos;
         pos = all.json.find(R"({"name":")", pos + 1)) {
        ++names;
    }
    EXPECT_EQ(names, rig.registry.all().size());
    for (const Command& command : rig.registry.all()) {
        EXPECT_NE(all.json.find("\"" + std::string(command.name) + "\""), std::string::npos)
            << command.name;
    }
    // One command: usage, help text, flags.
    const Reply one = rig.run("help wifi set");
    EXPECT_EQ(
        one.json,
        R"j({"name":"wifi set","usage":"wifi set <ssid> <password>","help":"store credentials; \"\" as password for an open network","flags":"S"})j");
    EXPECT_TRUE(rig.run("help factory-reset").json.contains(R"("flags":"D")"));
    EXPECT_NE(rig.run("help sync now").json.find(R"("flags":"R")"), std::string::npos);
    // A family word lists its members.
    const Reply family = rig.run("help wifi");
    EXPECT_EQ(family.status, "OK");
    EXPECT_NE(family.json.find("wifi clear"), std::string::npos);
    EXPECT_EQ(family.json.find("time get"), std::string::npos);
    EXPECT_EQ(rig.run("help time nothing").code, "not_found");
}

// ---- time and zones ---------------------------------------------------------------------------

TEST(CatalogTime, TimeGetBeforeAnyTimeIsKnownReportsInvalid) {
    Rig rig;
    EXPECT_EQ(
        rig.run("time get").json,
        R"j({"valid":false,"utc":null,"local":null,"unix":null,"source":"none","drift_ppb":0,"last_sync":null})j");
}

TEST(CatalogTime, TimeSetWithoutOffsetIsLocalToTheSelectedZone) {
    Rig rig;
    with_chicago(rig.api);
    const Reply reply = rig.run("time set 2026-07-01T12:00");
    ASSERT_EQ(reply.status, "OK") << reply.line;
    EXPECT_EQ(
        reply.json,
        R"j({"valid":true,"utc":"2026-07-01T17:00:00Z","local":"2026-07-01T12:00:00-05:00","unix":1782925200,"source":"console","drift_ppb":0,"last_sync":null})j");
}

TEST(CatalogTime, TimeSetWithOffsetAndFailurePropagation) {
    Rig rig;
    EXPECT_NE(
        rig.run("time set 2026-01-01T00:00:00+02:00").json.find(R"("utc":"2025-12-31T22:00:00Z")"),
        std::string::npos);
    fail_busy(rig.api);
    const Reply busy = rig.run("time set 2026-01-01T00:00:00Z");
    EXPECT_EQ(busy.code, "busy");
}

TEST(CatalogTime, TzListUnfilteredFitsTheResponseLimitAndFilterMatchesLabels) {
    Rig rig;
    const Reply all = rig.run("tz list");
    ASSERT_EQ(all.status, "OK") << all.line.substr(0, 80);
    const std::string expected_count = "\"count\":" + std::to_string(time::builtin_zones().size());
    EXPECT_NE(all.json.find(expected_count), std::string::npos);
    // Filter on the label, case-insensitively.
    const Reply none = rig.run("tz list zzzz-no-such-zone");
    EXPECT_EQ(none.json, R"j({"zones":[],"count":0})j");
    EXPECT_NE(rig.run("tz list pago").json.find("Pacific/Pago_Pago"), std::string::npos);
}

TEST(CatalogTime, TzSetChangesTheSetting) {
    Rig rig;
    ASSERT_EQ(rig.run("tz set Europe/Berlin").status, "OK");
    EXPECT_EQ(rig.api.current_settings().tz_name.view(), "Europe/Berlin");
    EXPECT_EQ(rig.run("tz get").json,
              R"j({"name":"Europe/Berlin","posix":"CET-1CEST,M3.5.0,M10.5.0/3","dst":true})j");
    EXPECT_EQ(rig.run("tz set europe/berlin").code, "not_found"); // names are exact
}

TEST(CatalogTime, VersionReportsTheDataVersions) {
    Rig rig;
    const Reply reply = rig.run("version");
    EXPECT_NE(reply.json.find("\"tzdata\":\"" + std::string(time::tzdata_version()) + "\""),
              std::string::npos);
    EXPECT_NE(reply.json.find(R"("proto":1})"), std::string::npos);
    rig.api.identity.radio_compiled = false;
    EXPECT_NE(rig.run("version").json.find(R"("radio":false)"), std::string::npos);
}

// ---- settings ---------------------------------------------------------------------------------

TEST(CatalogSettings, ListHasOneEntryPerSchemaKey) {
    Rig rig;
    const Reply reply = rig.run("settings list");
    ASSERT_EQ(reply.status, "OK");
    for (const settings::KeyInfo& info : settings::schema()) {
        EXPECT_NE(reply.json.find("{\"key\":\"" + std::string(info.name) + "\""), std::string::npos)
            << info.name;
    }
}

TEST(CatalogSettings, ValueTypesAreJsonTyped) {
    Rig rig;
    EXPECT_NE(rig.run("settings get vib").json.find(R"("value":true,"type":"bool")"),
              std::string::npos);
    EXPECT_NE(
        rig.run("settings get units").json.find(R"("value":"c","type":"enum","choices":["c","f"])"),
        std::string::npos);
    EXPECT_NE(rig.run("settings get sync_h")
                  .json.find(R"("value":24,"type":"uint","choices":["6","12","24","48","168"])"),
              std::string::npos);
    EXPECT_NE(rig.run("settings get tz").json.find(R"("value":"UTC","type":"zone")"),
              std::string::npos);
}

TEST(CatalogSettings, SetIsVisibleToGetAndResetRestoresDefaults) {
    Rig rig;
    ASSERT_EQ(rig.run("settings set goal 7500").status, "OK");
    ASSERT_EQ(rig.run("settings set vib off").status, "OK");
    EXPECT_NE(rig.run("settings get goal").json.find(R"("value":7500)"), std::string::npos);
    EXPECT_NE(rig.run("settings get vib").json.find(R"("value":false)"), std::string::npos);
    ASSERT_EQ(rig.run("settings reset").status, "OK");
    EXPECT_EQ(rig.api.current_settings(), settings::defaults());
}

TEST(CatalogSettings, InvalidValueLeavesTheSettingUntouched) {
    Rig rig;
    EXPECT_EQ(rig.run("settings set tfmt 13h").code, "bad_args");
    EXPECT_EQ(rig.run("settings set goal 51000").code, "bad_args");
    EXPECT_EQ(rig.run("settings set nosuch 1").code, "not_found");
    EXPECT_EQ(rig.api.current_settings(), settings::defaults());
}

// ---- input, steps, battery, weather -----------------------------------------------------------

TEST(CatalogInput, ButtonEventsCarryKindAndHoldDuration) {
    Rig rig;
    ASSERT_EQ(rig.run("btn up").status, "OK");
    ASSERT_EQ(rig.run("btn down click").status, "OK");
    ASSERT_EQ(rig.run("btn back hold").status, "OK");
    ASSERT_EQ(rig.run("btn menu repeat").status, "OK");
    ASSERT_EQ(rig.api.injected.size(), 4U);
    EXPECT_EQ(rig.api.injected[0].button, model::Button::kUp);
    EXPECT_EQ(rig.api.injected[0].kind, model::InputKind::kClick);
    EXPECT_EQ(rig.api.injected[0].held_ms, 0U);
    EXPECT_EQ(rig.api.injected[1].button, model::Button::kDown);
    EXPECT_EQ(rig.api.injected[2].button, model::Button::kBack);
    EXPECT_EQ(rig.api.injected[2].kind, model::InputKind::kHold);
    EXPECT_EQ(rig.api.injected[2].held_ms, 700U);
    EXPECT_EQ(rig.api.injected[3].button, model::Button::kMenu);
    EXPECT_EQ(rig.api.injected[3].kind, model::InputKind::kRepeat);
    EXPECT_EQ(rig.api.injected[3].held_ms, 850U);
    EXPECT_EQ(rig.run("btn menu tap").code, "bad_args");
    EXPECT_EQ(rig.api.injected.size(), 4U); // rejected requests inject nothing
}

TEST(CatalogSteps, InjectAcceptsNegativeAndRejectsOutOfRange) {
    Rig rig;
    EXPECT_NE(rig.run("steps inject 1000").json.find(R"("today":5321)"), std::string::npos);
    EXPECT_NE(rig.run("steps inject -5321").json.find(R"("today":0)"), std::string::npos);
    EXPECT_EQ(rig.run("steps inject 1000001").code, "bad_args");
    EXPECT_EQ(rig.run("steps inject 99999999999999999999").code, "bad_args");
    EXPECT_EQ(rig.run("steps inject 1.5").code, "bad_args");
    EXPECT_EQ(rig.run("steps inject +5").code, "bad_args");
}

TEST(CatalogSteps, HistoryWithNoDaysIsAnEmptyList) {
    Rig rig;
    rig.api.set_steps(model::StepsSummary{});
    EXPECT_EQ(rig.run("steps history").json, R"j({"today":0,"goal":0,"history":[]})j");
}

TEST(CatalogBattery, FakeOffRestoresAndRangeIsChecked) {
    Rig rig;
    ASSERT_EQ(rig.run("battery fake 3000").status, "OK");
    EXPECT_TRUE(rig.api.battery().faked);
    EXPECT_EQ(rig.api.battery().mv, 3000);
    const Reply off = rig.run("battery fake off");
    EXPECT_NE(off.json.find(R"("faked":false)"), std::string::npos);
    EXPECT_FALSE(rig.api.battery().faked);
    EXPECT_EQ(rig.run("battery fake 2499").code, "bad_args");
    EXPECT_EQ(rig.run("battery fake 4601").code, "bad_args");
    EXPECT_EQ(rig.run("battery fake high").code, "bad_args");
}

TEST(CatalogWeather, FakeStoresTheReport) {
    Rig rig;
    with_clock(rig.api);
    ASSERT_EQ(rig.run("weather fake -50 snow").status, "OK");
    const model::WeatherReport report = rig.api.weather().report;
    EXPECT_EQ(report.valid, 1);
    EXPECT_EQ(report.faked, 1);
    EXPECT_EQ(report.temp_dc, -50);
    EXPECT_EQ(report.condition, model::WeatherCondition::kSnow);
    EXPECT_EQ(report.has_high_low, 0);
    EXPECT_EQ(report.fetched_utc, kClock);
    // Numeric condition codes work too, and hi/lo come as a pair with hi >= lo.
    ASSERT_EQ(rig.run("weather fake 100 9 120 80").status, "OK");
    EXPECT_EQ(rig.api.weather().report.condition, model::WeatherCondition::kThunder);
    EXPECT_EQ(rig.api.weather().report.has_high_low, 1);
    EXPECT_EQ(rig.run("weather fake 100 rain 80 120").code, "bad_args");
    EXPECT_EQ(rig.run("weather fake 100 hail").code, "bad_args");
    EXPECT_EQ(rig.run("weather fake 100 10").code, "bad_args");
    EXPECT_EQ(rig.run("weather fake 601 clear").code, "bad_args");
    EXPECT_EQ(rig.run("weather fake 1.5 clear").code, "bad_args");
}

TEST(CatalogWeather, FakeWithoutValidTimeStillWorks) {
    Rig rig;
    ASSERT_EQ(rig.run("weather fake 200 clear").status, "OK");
    EXPECT_EQ(rig.api.weather().report.fetched_utc, 0);
}

TEST(CatalogWeather, ClearDropsTheReportAndFetchRequestsAWeatherSync) {
    Rig rig;
    ASSERT_EQ(rig.run("weather fake 200 clear").status, "OK");
    EXPECT_NE(rig.run("weather get").json.find(R"("temp_dc":200)"), std::string::npos);
    ASSERT_EQ(rig.run("weather clear").status, "OK");
    EXPECT_EQ(rig.api.weather().report.valid, 0);
    ASSERT_EQ(rig.run("weather fetch").status, "OK");
    ASSERT_EQ(rig.api.syncs.size(), 1U);
    EXPECT_FALSE(rig.api.syncs[0].first);
    EXPECT_TRUE(rig.api.syncs[0].second);
}

TEST(CatalogWeather, StaleWeatherIsReportedWithItsAge) {
    Rig rig;
    WeatherInfo info;
    info.report.valid = 1;
    info.report.temp_dc = -15;
    info.report.condition = model::WeatherCondition::kFog;
    info.report.fetched_utc = 1000;
    info.freshness = model::WeatherFreshness::kStale;
    info.age_s = 7200;
    rig.api.set_weather(info);
    EXPECT_EQ(
        rig.run("weather get").json,
        R"j({"report":{"temp_dc":-15,"condition":"fog","fetched":1000,"faked":false},"age_s":7200,"freshness":"stale"})j");
}

// ---- Wi-Fi: the password must never leave -----------------------------------------------------

class CatalogWifi : public test::LogCaptureTest {};

TEST_F(CatalogWifi, SetNeverEchoesOrLogsThePassword) {
    Rig rig;
    const std::string password = "p4ss-w0rd-Zq9";
    const Reply reply = rig.run("#a1 wifi set \"Cafe Net\" " + password);
    ASSERT_EQ(reply.status, "OK") << reply.line;
    EXPECT_EQ(reply.json, R"j({"configured":true,"ssid":"Cafe Net","has_password":true})j");
    EXPECT_EQ(reply.line.find(password), std::string::npos);
    EXPECT_EQ(rig.api.password_seen, password); // it did reach the device layer, only there
    EXPECT_FALSE(logged(password));
    EXPECT_FALSE(logged("Zq9"));
    // `wifi status` after the fact, and the help text, do not know it either.
    EXPECT_EQ(rig.run("wifi status").line.find("Zq9"), std::string::npos);
    EXPECT_EQ(rig.run("help wifi").line.find("Zq9"), std::string::npos);
    EXPECT_FALSE(logged("Zq9"));
}

TEST_F(CatalogWifi, RejectedAndFailedSetsDoNotLeakEither) {
    Rig rig;
    // Too short for WPA (8..63): validate_credentials says kBadArgs.
    const Reply short_pw = rig.run("wifi set Home Zq7!");
    EXPECT_EQ(short_pw.code, "bad_args");
    EXPECT_EQ(short_pw.line.find("Zq7"), std::string::npos);
    EXPECT_TRUE(rig.api.password_seen.empty()); // never forwarded
    // Valid credentials but the device layer fails.
    fail_io(rig.api);
    const Reply failed = rig.run("wifi set Home Zq7!-long-enough");
    EXPECT_EQ(failed.code, "io");
    EXPECT_EQ(failed.line.find("Zq7"), std::string::npos);
    EXPECT_FALSE(logged("Zq7"));
}

TEST_F(CatalogWifi, SetValidatesCredentialsBeforeTheDevice) {
    Rig rig;
    EXPECT_EQ(rig.run("wifi set \"\" password123").code, "bad_args"); // empty SSID
    EXPECT_EQ(rig.run("wifi set 0123456789012345678901234567890123 password123").code,
              "bad_args"); // 34 bytes
    EXPECT_TRUE(rig.api.password_seen.empty());
    EXPECT_FALSE(rig.api.wifi_ssid().has_value());
    // An open network is an explicit empty password.
    const Reply open = rig.run("wifi set Guest \"\"");
    ASSERT_EQ(open.status, "OK") << open.line;
    EXPECT_EQ(open.json, R"j({"configured":true,"ssid":"Guest","has_password":false})j");
}

TEST_F(CatalogWifi, ClearForgetsTheNetwork) {
    Rig rig;
    with_wifi(rig.api);
    ASSERT_EQ(rig.run("wifi clear").status, "OK");
    EXPECT_FALSE(rig.api.wifi_ssid().has_value());
    EXPECT_NE(rig.run("wifi status").json.find(R"("configured":false)"), std::string::npos);
}

// ---- radio gating -----------------------------------------------------------------------------

TEST(CatalogRadio, RadioCommandsAnswerUnsupportedWhenCompiledOut) {
    Rig rig(/*radio_compiled=*/false);
    for (const std::string_view line : {"weather fetch", "sync now", "provision start"}) {
        SCOPED_TRACE(line);
        const Reply reply = rig.run(line);
        EXPECT_EQ(reply.status, "ERR");
        EXPECT_EQ(reply.code, "unsupported");
    }
    EXPECT_TRUE(rig.api.syncs.empty());
    EXPECT_FALSE(rig.api.provisioning);
    // Commands without the radio flag keep working.
    EXPECT_EQ(rig.run("sync status").status, "OK");
    EXPECT_EQ(rig.run("provision stop").code, "invalid_state");
}

TEST(CatalogRadio, HandlersAlsoRefuseWhenTheFirmwareIdentityHasNoRadio) {
    Rig rig; // dispatcher believes in the radio, the device layer says it is compiled out
    with_cold_clock_off_radio(rig.api);
    for (const std::string_view line : {"weather fetch", "sync now", "provision start"}) {
        SCOPED_TRACE(line);
        EXPECT_EQ(rig.run(line).code, "unsupported");
    }
    EXPECT_TRUE(rig.api.syncs.empty());
    EXPECT_FALSE(rig.api.provisioning);
}

TEST(CatalogRadio, SyncNowSelectsTheJobs) {
    Rig rig;
    ASSERT_EQ(rig.run("sync now").status, "OK");
    ASSERT_EQ(rig.run("sync now time").status, "OK");
    ASSERT_EQ(rig.run("sync now all").status, "OK");
    ASSERT_EQ(rig.api.syncs.size(), 3U);
    EXPECT_EQ(rig.api.syncs[0], std::make_pair(true, true));
    EXPECT_EQ(rig.api.syncs[1], std::make_pair(true, false));
    EXPECT_EQ(rig.api.syncs[2], std::make_pair(true, true));
    fail_battery_low(rig.api);
    EXPECT_EQ(rig.run("sync now").code, "battery_low");
}

TEST(CatalogRadio, ProvisioningNeverShowsAPassword) {
    Rig rig;
    const Reply start = rig.run("provision start");
    ASSERT_EQ(start.status, "OK");
    EXPECT_EQ(start.json.find("pass"), std::string::npos);
    EXPECT_EQ(rig.run("provision stop").json, R"j({"stopped":true})j");
    EXPECT_FALSE(rig.api.provisioning);
}

// ---- display ----------------------------------------------------------------------------------

/// Independent RFC 4648 decoder (the writer's own table is not reused).
std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') {
            value = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            value = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            value = c - '0' + 52;
        } else if (c == '+') {
            value = 62;
        } else if (c == '/') {
            value = 63;
        } else if (c == '=') {
            break;
        } else {
            return {};
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(
                static_cast<std::uint8_t>((accumulator >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return out;
}

TEST(CatalogDisplay, DumpDecodesToTheFramebuffer) {
    Rig rig;
    const Reply reply = rig.run("display dump");
    ASSERT_EQ(reply.status, "OK");
    const std::string_view marker = R"("b64":")";
    const std::size_t start = reply.json.find(marker);
    ASSERT_NE(start, std::string::npos);
    const std::size_t from = start + marker.size();
    const std::size_t end = reply.json.find('"', from);
    ASSERT_NE(end, std::string::npos);
    const std::string_view b64 = std::string_view(reply.json).substr(from, end - from);
    EXPECT_EQ(b64.size(), 6668U); // 5000 bytes -> 1667 groups of 4, one partial group padded
    const std::vector<std::uint8_t> bytes = base64_decode(b64);
    ASSERT_EQ(bytes.size(), 5000U);
    const std::span<const std::uint8_t> frame = rig.api.framebuffer();
    EXPECT_TRUE(std::ranges::equal(bytes, frame));
    // The CRC in the same answer is the CRC of those bytes.
    EXPECT_EQ(crc32(bytes), crc32(frame));
    EXPECT_LT(reply.line.size(), kMaxResponseBytes);
}

TEST(CatalogDisplay, CrcChangesWithThePixelsAndRefreshReportsTheMode) {
    Rig rig;
    const Reply before = rig.run("display crc");
    rig.api.frame_bytes = 4999;
    EXPECT_EQ(rig.run("display dump").code, "internal"); // not a 200x200 1 bpp buffer
    EXPECT_NE(rig.run("display crc").json, before.json);
    rig.api.frame_bytes = 5000;
    EXPECT_EQ(rig.run("display crc").json, before.json);
    EXPECT_EQ(rig.run("display refresh").json, R"j({"mode":"partial","crc32":"1abd04d4"})j");
    EXPECT_EQ(rig.run("display refresh partial").status, "OK");
    EXPECT_EQ(rig.run("display refresh full").status, "OK");
    EXPECT_EQ(rig.api.refreshes, (std::vector<bool>{false, false, true}));
    EXPECT_EQ(rig.run("display refresh bogus").code, "bad_args");
    EXPECT_EQ(rig.api.refreshes.size(), 3U);
}

// ---- screens, faces, logs, diagnostics, actuators ---------------------------------------------

TEST(CatalogScreens, ShowSwitchesAndRejectsUnknownIds) {
    Rig rig;
    EXPECT_EQ(rig.run("screen show menu").json, R"j({"screen":"menu"})j");
    EXPECT_EQ(rig.run("screen get").json, R"j({"screen":"menu"})j");
    EXPECT_EQ(rig.run("screen show nowhere").code, "not_found");
    EXPECT_EQ(rig.run("screen get").json, R"j({"screen":"menu"})j");
}

TEST(CatalogFaces, SetAcceptsIdOrNameAndOnlyRegisteredFaces) {
    Rig rig;
    ASSERT_EQ(rig.run("face set 3").status, "OK");
    EXPECT_EQ(rig.api.current_settings().face_id, 3);
    ASSERT_EQ(rig.run("face set classic").status, "OK");
    EXPECT_EQ(rig.api.current_settings().face_id, 0);
    EXPECT_EQ(rig.run("face set 1").code, "not_found"); // ids are sparse: 1 and 2 are not faces
    EXPECT_EQ(rig.run("face set 256").code, "not_found");
    EXPECT_EQ(rig.run("face set fancy").code, "not_found");
    EXPECT_EQ(rig.api.current_settings().face_id, 0);
    ASSERT_EQ(rig.run("face set minimal").status, "OK");
    EXPECT_NE(rig.run("face list").json.find(R"("current":3)"), std::string::npos);
}

TEST(CatalogLog, WakesAreOldestFirstAndClampedToWhatExists) {
    Rig rig;
    const Reply all = rig.run("log wakes");
    EXPECT_EQ(all.json.find(R"("utc_s":1000000)") < all.json.find(R"("utc_s":1000002)"), true);
    const Reply many = rig.run("log wakes 50");
    EXPECT_NE(many.json.find(R"("total":3)"), std::string::npos);
    EXPECT_NE(many.json.find(R"("utc_s":1000000)"), std::string::npos);
    EXPECT_EQ(rig.run("log wakes 65").code, "bad_args");
    EXPECT_EQ(rig.run("log wakes -1").code, "bad_args");
    EXPECT_EQ(rig.run("log wakes many").code, "bad_args");
}

TEST(CatalogLog, FullWakeRingFitsTheLineLimit) {
    Rig rig;
    rig.api.wakes.clear();
    for (std::uint32_t i = 0; i < 200; ++i) {
        model::WakeRecord record;
        record.start_utc_s = 4'000'000'000U + i; // largest field values
        record.awake_ms = 65'535;
        record.cause = model::WakeCause::kTetheredTick;
        record.flags = 255;
        record.battery_mv = 65'535;
        record.power = model::PowerLevel::kCritical;
        record.error = static_cast<std::uint8_t>(static_cast<unsigned>(Errc::kCorrupt) + 1U);
        record.steps_delta = 65'535;
        rig.api.wakes.push_back(record);
    }
    const Reply reply = rig.run("log wakes 64");
    ASSERT_EQ(reply.status, "OK") << reply.line.substr(0, 60);
    EXPECT_NE(reply.json.find(R"("total":200)"), std::string::npos);
    EXPECT_NE(reply.json.find(R"("utc_s":4000000199)"),
              std::string::npos); // the newest is included
    EXPECT_EQ(reply.json.find(R"("utc_s":4000000135)"), std::string::npos); // 65th newest is not
}

TEST(CatalogLog, EmptyLogAndClear) {
    Rig rig;
    ASSERT_EQ(rig.run("log clear").status, "OK");
    EXPECT_TRUE(rig.api.wake_log_cleared);
    EXPECT_EQ(rig.run("log wakes").json, R"j({"total":0,"wakes":[]})j");
}

TEST(CatalogLog, LevelMapsEveryName) {
    Rig rig;
    const std::array<std::pair<std::string_view, LogLevel>, 5> levels{
        {{"error", LogLevel::kError},
         {"warn", LogLevel::kWarn},
         {"info", LogLevel::kInfo},
         {"debug", LogLevel::kDebug},
         {"verbose", LogLevel::kVerbose}}};
    for (const auto& [name, level] : levels) {
        ASSERT_EQ(rig.run("log level " + std::string(name)).status, "OK");
        EXPECT_EQ(rig.api.log_level_set, level);
    }
    rig.api.log_level_set.reset();
    EXPECT_EQ(rig.run("log level DEBUG").code, "bad_args");
    EXPECT_FALSE(rig.api.log_level_set.has_value());
}

TEST(CatalogDiag, EveryPageIsReachable) {
    Rig rig;
    for (const std::string_view page :
         {"info", "power", "radio", "rtc", "nvs", "clock", "sensors"}) {
        SCOPED_TRACE(page);
        const Reply reply = rig.run("diag " + std::string(page));
        EXPECT_EQ(reply.status, "OK");
        EXPECT_NE(reply.json.find(std::string("\"page\":\"") + std::string(page) + "\""),
                  std::string::npos);
    }
    EXPECT_EQ(rig.run("diag").code, "bad_args");
}

TEST(CatalogSelftest, RunWithoutFilterAndWithFailure) {
    Rig rig;
    EXPECT_EQ(rig.run("selftest run").status, "OK");
    EXPECT_EQ(rig.run("selftest run battery.thresholds").status, "OK");
    fail_busy(rig.api);
    EXPECT_EQ(rig.run("selftest run").code, "busy");
}

TEST(CatalogActuators, VibrateDefaultsAndBounds) {
    Rig rig;
    ASSERT_EQ(rig.run("vibrate").json, R"j({"ms":200})j");
    ASSERT_EQ(rig.run("vibrate 1").status, "OK");
    ASSERT_EQ(rig.run("vibrate 1000").status, "OK");
    EXPECT_EQ(rig.api.vibrations, (std::vector<std::uint16_t>{200, 1, 1000}));
    EXPECT_EQ(rig.run("vibrate 0").code, "bad_args");
    EXPECT_EQ(rig.run("vibrate -5").code, "bad_args");
    EXPECT_EQ(rig.run("vibrate 70000").code, "bad_args");
    EXPECT_EQ(rig.api.vibrations.size(), 3U);
}

TEST(CatalogActuators, SleepBounds) {
    Rig rig;
    ASSERT_EQ(rig.run("sleep 1").status, "OK");
    ASSERT_EQ(rig.run("sleep 3600").status, "OK");
    EXPECT_EQ(rig.api.sleeps, (std::vector<std::uint32_t>{1, 3600}));
    EXPECT_EQ(rig.run("sleep 3601").code, "bad_args");
    EXPECT_EQ(rig.run("sleep soon").code, "bad_args");
    EXPECT_EQ(rig.run("sleep").code, "bad_args");
    EXPECT_EQ(rig.api.sleeps.size(), 2U);
}

TEST(CatalogActuators, RebootAndFactoryResetRequestTheActionOnce) {
    Rig rig;
    ASSERT_EQ(rig.run("reboot").status, "OK");
    EXPECT_TRUE(rig.api.reboot_requested);
    // Only the literal "confirm" resets.
    for (const std::string_view line : {"factory-reset",
                                        "factory-reset yes",
                                        "factory-reset CONFIRM",
                                        "factory-reset confirm now"}) {
        SCOPED_TRACE(line);
        EXPECT_EQ(rig.run(line).code, "bad_args");
        EXPECT_FALSE(rig.api.factory_reset_done);
    }
    ASSERT_EQ(rig.run("factory-reset confirm").status, "OK");
    EXPECT_TRUE(rig.api.factory_reset_done);
}

// ---- framing --------------------------------------------------------------------------------

TEST(CatalogFraming, RequestIdIsEchoedOnSuccessAndError) {
    Rig rig;
    test::ExactBuffer ok_line("#r7 version");
    std::vector<char> response(kMaxResponseBytes);
    EXPECT_TRUE(std::string(rig.dispatcher.handle_line(ok_line.span(), response))
                    .starts_with("@QZ1 r7 OK {"));
    test::ExactBuffer err_line("#r8 tz set Nowhere/Land");
    EXPECT_TRUE(std::string(rig.dispatcher.handle_line(err_line.span(), response))
                    .starts_with(R"(@QZ1 r8 ERR not_found {"msg":")"));
}

TEST(CatalogFraming, SuccessLinesRunBackToBackOnOneDevice) {
    // Every success case's line on one device in a row: state carries over (settings changed, face
    // switched, wifi stored, ...) and every answer must still be a well-formed OK line.
    Rig rig;
    with_clock(rig.api);
    for (const OkCase& c : kOkCases) {
        SCOPED_TRACE(c.line);
        const Reply reply = rig.run(c.line);
        EXPECT_EQ(reply.status, "OK") << reply.line;
    }
}

} // namespace
} // namespace qz::console

// NOLINTEND(readability-function-cognitive-complexity)
