// Test registry and runner (ARCHITECTURE.md section 18).
#include "qz/selftest/selftest.hpp"
#include "tests.hpp"

#include <array>

namespace qz::selftest {

namespace {

constexpr std::array kTests = std::to_array<TestCase>({
    {"drivers", "display_init", &tests::display_init, false},
    {"drivers", "display_temp", &tests::display_temp, false},
    {"drivers", "display_full", &tests::display_full, false},
    {"drivers", "display_partial", &tests::display_partial, false},
    {"drivers", "accel_chip_id", &tests::accel_chip_id, false},
    {"drivers", "accel_features", &tests::accel_features, false},
    {"drivers", "battery_adc", &tests::battery_adc, false},
    {"drivers", "buttons_idle", &tests::buttons_idle, false},
    {"drivers", "usb_pins", &tests::usb_pins, false},
    {"drivers", "rtc_clock", &tests::rtc_clock, false},
    {"drivers", "nvs_roundtrip", &tests::nvs_roundtrip, false},
    {"drivers", "system_info", &tests::system_info, false},
    {"settings", "defaults_valid", &tests::settings_defaults, false},
    {"settings", "reject_invalid", &tests::settings_reject, false},
    {"settings", "string_roundtrip", &tests::settings_strings, false},
    {"settings", "persist_roundtrip", &tests::settings_persist, false},
    {"settings", "restore_defaults", &tests::settings_restore, false},
    {"time", "tz_zones", &tests::tz_zones, false},
    {"time", "tz_transitions", &tests::tz_transitions, false},
    {"time", "civil_math", &tests::civil_math, false},
    {"time", "tz_table_parse", &tests::tz_table_parse, false},
    {"screens", "scenes_cover", &tests::scenes_cover, false},
    {"screens", "scenes_crc", &tests::scenes_crc, false},
    {"interactive", "button_menu", &tests::button_menu, true},
    {"interactive", "button_back", &tests::button_back, true},
    {"interactive", "button_up", &tests::button_up, true},
    {"interactive", "button_down", &tests::button_down, true},
    {"interactive", "vibration", &tests::vibration, true},
});

/// "" / "all" = everything; "suite" = one suite; "suite/name" = one test.
bool matches(const TestCase& test, std::string_view filter) noexcept {
    if (filter.empty() || filter == "all") {
        return true;
    }
    const std::size_t slash = filter.find('/');
    if (slash == std::string_view::npos) {
        return test.suite == filter;
    }
    return test.suite == filter.substr(0, slash) && test.name == filter.substr(slash + 1);
}

} // namespace

std::span<const TestCase> all_tests() noexcept {
    return kTests;
}

Summary
run(Context& ctx, std::string_view filter, ReportSink& sink, std::uint32_t (*now_ms)()) noexcept {
    Summary summary;
    for (const TestCase& test : kTests) {
        if (!matches(test, filter)) {
            continue;
        }
        TestReport report;
        report.suite = test.suite;
        report.name = test.name;
        const std::uint32_t start_ms = now_ms != nullptr ? now_ms() : 0U;
        if (test.interactive && !ctx.interactive) {
            report.outcome = Outcome::kSkip;
            (void)report.detail.assign("interactive tests off"); // literal fits
        } else {
            report.outcome = test.fn(ctx, report.detail);
        }
        const std::uint32_t end_ms = now_ms != nullptr ? now_ms() : start_ms;
        report.duration_ms = end_ms - start_ms; // unsigned wrap-around safe
        switch (report.outcome) {
            case Outcome::kPass:
                ++summary.passed;
                break;
            case Outcome::kFail:
                ++summary.failed;
                break;
            case Outcome::kSkip:
                ++summary.skipped;
                break;
        }
        sink.on_result(report);
    }
    return summary;
}

} // namespace qz::selftest
