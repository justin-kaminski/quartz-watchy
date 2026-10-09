// Catalog part 2: input, steps, battery, weather, screens, faces and the display.
#include "commands_internal.hpp"
#include "qz/core/crc32.hpp"
#include "qz/settings/settings.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace qz::console::detail {
namespace {

using Args = std::span<const std::string_view>;
using Tune = CatalogTuning;

// ---- btn ----

Status btn(DeviceApi& api, Args args, JsonWriter& out) {
    const Result<model::Button> button = parse_button(args[0]);
    if (!button) {
        return Errc::kBadArgs;
    }
    model::InputKind kind = model::InputKind::kClick;
    if (args.size() > 1) {
        const Result<model::InputKind> parsed = parse_input_kind(args[1]);
        if (!parsed) {
            return Errc::kBadArgs;
        }
        kind = *parsed;
    }
    model::InputEvent event;
    event.button = *button;
    event.kind = kind;
    switch (kind) {
        case model::InputKind::kClick:
            event.held_ms = 0;
            break;
        case model::InputKind::kHold:
            event.held_ms = Tune::kHoldMs;
            break;
        case model::InputKind::kRepeat:
            event.held_ms = Tune::kRepeatMs;
            break;
    }
    // t_us stays 0: the app stamps injected events with its raw RTC time when it routes them.
    QZ_RETURN_IF_ERROR(api.inject(event));
    out.field("screen", api.current_screen());
    return ok();
}

// ---- steps ----

void write_today(JsonWriter& out, const model::StepsSummary& steps) {
    out.field("today", static_cast<std::int64_t>(steps.today));
    out.field("goal", static_cast<std::int64_t>(steps.goal));
}

Status steps_get(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_today(out, api.steps());
    return ok();
}

Status steps_history(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    const model::StepsSummary steps = api.steps();
    write_today(out, steps);
    const std::size_t count = std::min<std::size_t>(steps.history_count, steps.history.size());
    out.begin_array("history");
    for (std::size_t i = 0; i < count; ++i) {
        std::array<char, 12> date{};
        out.begin_object();
        out.field("date", format_date(date, steps.history[i].day));
        out.field("steps", static_cast<std::int64_t>(steps.history[i].steps));
        out.end_object();
    }
    out.end_array();
    return ok();
}

Status steps_inject(DeviceApi& api, Args args, JsonWriter& out) {
    const Result<std::int64_t> delta =
        parse_integer(args[0], -Tune::kMaxStepDelta, Tune::kMaxStepDelta);
    if (!delta) {
        return Errc::kBadArgs;
    }
    QZ_RETURN_IF_ERROR(api.inject_steps(static_cast<std::int32_t>(*delta)));
    write_today(out, api.steps());
    return ok();
}

Status steps_reset_today(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.reset_steps_today());
    write_today(out, api.steps());
    return ok();
}

// ---- battery ----

void write_battery(JsonWriter& out, const model::BatteryStatus& battery) {
    out.field("mv", battery.mv);
    out.field("pct", battery.percent);
    out.field("state", power_level_name(battery.level));
    out.field_bool("usb", battery.usb_present);
    out.field_bool("charging", battery.charging);
    out.field_bool("valid", battery.valid);
    out.field_bool("faked", battery.faked);
}

Status battery_get(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_battery(out, api.battery());
    return ok();
}

Status battery_fake(DeviceApi& api, Args args, JsonWriter& out) {
    std::optional<std::uint16_t> mv;
    if (args[0] != "off") {
        const Result<std::int64_t> parsed =
            parse_integer(args[0], Tune::kFakeMvMin, Tune::kFakeMvMax);
        if (!parsed) {
            return Errc::kBadArgs;
        }
        mv = static_cast<std::uint16_t>(*parsed);
    }
    QZ_RETURN_IF_ERROR(api.fake_battery_mv(mv));
    write_battery(out, api.battery());
    return ok();
}

// ---- weather ----

void write_weather(JsonWriter& out, const WeatherInfo& weather) {
    out.key("report");
    if (weather.report.valid == 0) {
        out.null();
    } else {
        const model::WeatherReport& report = weather.report;
        out.begin_object();
        out.field("temp_dc", report.temp_dc);
        out.field("condition", weather_condition_name(report.condition));
        if (report.has_high_low != 0) {
            out.field("high_dc", report.high_dc);
            out.field("low_dc", report.low_dc);
        }
        out.field("fetched", static_cast<std::int64_t>(report.fetched_utc));
        out.field_bool("faked", report.faked != 0);
        out.end_object();
    }
    out.field("age_s", static_cast<std::int64_t>(weather.age_s));
    out.field("freshness", freshness_name(weather.freshness));
}

Status weather_get(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_weather(out, api.weather());
    return ok();
}

/// `<temp_dc> <code> [hi lo]` (args[0..3]) into `report`.
Status parse_report(Args args, model::WeatherReport& report) noexcept {
    if (args.size() == 3) {
        return Errc::kBadArgs; // hi and lo come as a pair
    }
    const Result<std::int64_t> temp = parse_integer(args[0], Tune::kTempMinDc, Tune::kTempMaxDc);
    const Result<model::WeatherCondition> condition = parse_weather_condition(args[1]);
    if (!temp || !condition) {
        return Errc::kBadArgs;
    }
    report.temp_dc = static_cast<std::int16_t>(*temp);
    report.condition = *condition;
    report.valid = 1;
    if (args.size() >= 4) {
        const Result<std::int64_t> high =
            parse_integer(args[2], Tune::kTempMinDc, Tune::kTempMaxDc);
        const Result<std::int64_t> low = parse_integer(args[3], Tune::kTempMinDc, Tune::kTempMaxDc);
        if (!high || !low || *high < *low) {
            return Errc::kBadArgs;
        }
        report.high_dc = static_cast<std::int16_t>(*high);
        report.low_dc = static_cast<std::int16_t>(*low);
        report.has_high_low = 1;
    }
    return ok();
}

Status weather_fake(DeviceApi& api, Args args, JsonWriter& out) {
    model::WeatherReport report;
    QZ_RETURN_IF_ERROR(parse_report(args, report));
    report.faked = 1;
    const TimeInfo now = api.time_info();
    report.fetched_utc = now.valid ? now.utc_us / time::kUsPerSecond : 0;
    QZ_RETURN_IF_ERROR(api.fake_weather(report));
    write_weather(out, api.weather());
    return ok();
}

/// `weather push <temp_dc> <code> <hi> <lo> [observed_unix]`: hi/lo are required (the page always
/// has them); the observation time defaults to now and needs a valid clock either way.
Status weather_push(DeviceApi& api, Args args, JsonWriter& out) {
    model::WeatherReport report;
    QZ_RETURN_IF_ERROR(parse_report(args.first(4), report));
    const TimeInfo now = api.time_info();
    if (!now.valid) {
        return Errc::kNoTime;
    }
    const time::UnixSeconds now_s = now.utc_us / time::kUsPerSecond;
    report.fetched_utc = now_s;
    if (args.size() == 5) {
        const Result<std::int64_t> observed =
            parse_integer(args[4], now_s - Tune::kPushMaxAgeS, now_s + Tune::kPushMaxSkewS);
        if (!observed) {
            return Errc::kBadArgs;
        }
        report.fetched_utc = std::min<time::UnixSeconds>(*observed, now_s);
    }
    QZ_RETURN_IF_ERROR(api.push_weather(report));
    write_weather(out, api.weather());
    return ok();
}

Status phone_forget(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.forget_phones());
    out.field_bool("forgotten", true);
    return ok();
}

Status weather_clear(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.clear_weather());
    write_weather(out, api.weather());
    return ok();
}

Status weather_fetch(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(require_radio(api));
    QZ_RETURN_IF_ERROR(api.sync_now(false, true));
    write_weather(out, api.weather());
    return ok();
}

// ---- screens and faces ----

Status screen_list(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    out.begin_array("screens");
    for (std::size_t i = 0; i < api.screen_count(); ++i) {
        out.str(api.screen_name_at(i));
    }
    out.end_array();
    out.field("current", api.current_screen());
    return ok();
}

Status screen_get(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    out.field("screen", api.current_screen());
    return ok();
}

Status screen_show(DeviceApi& api, Args args, JsonWriter& out) {
    QZ_RETURN_IF_ERROR(api.show_screen(args[0]));
    out.field("screen", api.current_screen());
    return ok();
}

Status face_list(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    out.begin_array("faces");
    for (std::size_t i = 0; i < api.face_count(); ++i) {
        const std::uint8_t id = api.face_id_at(i);
        out.begin_object();
        out.field("id", id);
        out.field("name", api.face_name(id));
        out.end_object();
    }
    out.end_array();
    out.field("current", api.current_settings().face_id);
    return ok();
}

/// Resolves a face given as id or name; kNotFound when no registered face matches.
Result<std::uint8_t> resolve_face(const DeviceApi& api, std::string_view text) {
    const Result<std::int64_t> number = parse_integer(text, 0, 255);
    for (std::size_t i = 0; i < api.face_count(); ++i) {
        const std::uint8_t id = api.face_id_at(i);
        const std::int64_t wide_id = id;
        const bool same_number = number.has_value() && number.value_or(-1) == wide_id;
        if (same_number || api.face_name(id) == text) {
            return id;
        }
    }
    return Errc::kNotFound;
}

Status face_set(DeviceApi& api, Args args, JsonWriter& out) {
    const Result<std::uint8_t> id = resolve_face(api, args[0]);
    if (!id) {
        return id.error();
    }
    std::array<char, 8> digits{};
    QZ_RETURN_IF_ERROR(api.apply_setting(settings::Key::kFace, format_decimal(digits, *id)));
    out.field("id", *id);
    out.field("name", api.face_name(*id));
    return ok();
}

// ---- display ----

void write_crc(JsonWriter& out, std::span<const std::uint8_t> frame) {
    std::array<char, 8> hex{};
    out.field("crc32", format_hex32(hex, crc32(frame)));
}

Status display_refresh(DeviceApi& api, Args args, JsonWriter& out) {
    bool full = false;
    if (!args.empty()) {
        if (args[0] == "full") {
            full = true;
        } else if (args[0] != "partial") {
            return Errc::kBadArgs;
        }
    }
    QZ_RETURN_IF_ERROR(api.refresh_display(full));
    out.field("mode", full ? "full" : "partial");
    write_crc(out, api.framebuffer());
    return ok();
}

Status display_crc(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    write_crc(out, api.framebuffer());
    return ok();
}

Status display_dump(DeviceApi& api, Args /*args*/, JsonWriter& out) {
    const std::span<const std::uint8_t> frame = api.framebuffer();
    if (frame.size() != Tune::kFramebufferBytes) {
        return Errc::kInternal;
    }
    out.field("w", static_cast<std::int64_t>(Tune::kPanelWidth));
    out.field("h", static_cast<std::int64_t>(Tune::kPanelHeight));
    out.field("fmt", "1bpp-msb");
    write_crc(out, frame);
    out.key("b64").base64(frame);
    return ok();
}

constexpr auto kCommands = std::to_array<Command>({
    {"btn",
     "btn <menu|back|up|down> [click|hold|repeat]",
     "inject a button event; answers with the screen afterwards",
     1,
     2,
     kFlagNone,
     btn},
    {"steps get", "steps get", "today's steps and the goal", 0, 0, kFlagNone, steps_get},
    {"steps history", "steps history", "today and the last days", 0, 0, kFlagNone, steps_history},
    {"steps inject",
     "steps inject <delta>",
     "add (or remove) steps for testing",
     1,
     1,
     kFlagNone,
     steps_inject},
    {"steps reset-today",
     "steps reset-today",
     "zero today's steps",
     0,
     0,
     kFlagDestructive,
     steps_reset_today},
    {"battery get",
     "battery get",
     "battery voltage, percent and power state",
     0,
     0,
     kFlagNone,
     battery_get},
    {"battery fake",
     "battery fake <mv|off>",
     "override the battery voltage (off = real)",
     1,
     1,
     kFlagNone,
     battery_fake},
    {"weather get",
     "weather get",
     "cached weather report and its freshness",
     0,
     0,
     kFlagNone,
     weather_get},
    {"weather fake",
     "weather fake <temp_dc> <code> [hi lo]",
     "install a fake report (code: name or 0..9)",
     2,
     4,
     kFlagNone,
     weather_fake},
    {"weather push",
     "weather push <temp_dc> <code> <hi> <lo> [observed_unix]",
     "store a report computed by the phone page",
     4,
     5,
     kFlagNone,
     weather_push},
    {"weather clear", "weather clear", "drop the cached report", 0, 0, kFlagNone, weather_clear},
    {"phone forget",
     "phone forget",
     "delete every paired phone",
     0,
     0,
     kFlagDestructive,
     phone_forget},
    {"weather fetch",
     "weather fetch",
     "fetch weather now over Wi-Fi",
     0,
     0,
     kFlagNeedsRadio | kFlagUsbOnly,
     weather_fetch},
    {"screen list", "screen list", "screen ids", 0, 0, kFlagNone, screen_list},
    {"screen get", "screen get", "current screen", 0, 0, kFlagNone, screen_get},
    {"screen show", "screen show <id>", "switch to a screen", 1, 1, kFlagNone, screen_show},
    {"face list", "face list", "registered faces", 0, 0, kFlagNone, face_list},
    {"face set", "face set <id|name>", "select the watch face", 1, 1, kFlagNone, face_set},
    {"display refresh",
     "display refresh [full|partial]",
     "refresh the panel",
     0,
     1,
     kFlagNone,
     display_refresh},
    {"display dump",
     "display dump",
     "framebuffer as base64 (200x200, 1bpp, MSB first)",
     0,
     0,
     kFlagNone,
     display_dump},
    {"display crc", "display crc", "CRC-32 of the framebuffer", 0, 0, kFlagNone, display_crc},
});

} // namespace

std::span<const Command> device_commands() noexcept {
    return kCommands;
}

} // namespace qz::console::detail
