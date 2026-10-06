// Test functions of every suite (registered in runner.cpp).
#pragma once

#include "detail.hpp"
#include "qz/selftest/selftest.hpp"

namespace qz::selftest::tests {

// drivers
Outcome display_init(Context& ctx, Detail& d) noexcept;
Outcome display_temp(Context& ctx, Detail& d) noexcept;
Outcome display_full(Context& ctx, Detail& d) noexcept;
Outcome display_partial(Context& ctx, Detail& d) noexcept;
Outcome accel_chip_id(Context& ctx, Detail& d) noexcept;
Outcome accel_features(Context& ctx, Detail& d) noexcept;
Outcome battery_adc(Context& ctx, Detail& d) noexcept;
Outcome buttons_idle(Context& ctx, Detail& d) noexcept;
Outcome usb_pins(Context& ctx, Detail& d) noexcept;
Outcome rtc_clock(Context& ctx, Detail& d) noexcept;
Outcome nvs_roundtrip(Context& ctx, Detail& d) noexcept;
Outcome system_info(Context& ctx, Detail& d) noexcept;
// settings
Outcome settings_defaults(Context& ctx, Detail& d) noexcept;
Outcome settings_reject(Context& ctx, Detail& d) noexcept;
Outcome settings_strings(Context& ctx, Detail& d) noexcept;
Outcome settings_persist(Context& ctx, Detail& d) noexcept;
Outcome settings_restore(Context& ctx, Detail& d) noexcept;
// time
Outcome tz_zones(Context& ctx, Detail& d) noexcept;
Outcome tz_transitions(Context& ctx, Detail& d) noexcept;
Outcome civil_math(Context& ctx, Detail& d) noexcept;
Outcome tz_table_parse(Context& ctx, Detail& d) noexcept;
// screens
Outcome scenes_cover(Context& ctx, Detail& d) noexcept;
Outcome scenes_crc(Context& ctx, Detail& d) noexcept;
// interactive
Outcome button_menu(Context& ctx, Detail& d) noexcept;
Outcome button_back(Context& ctx, Detail& d) noexcept;
Outcome button_up(Context& ctx, Detail& d) noexcept;
Outcome button_down(Context& ctx, Detail& d) noexcept;
Outcome vibration(Context& ctx, Detail& d) noexcept;

} // namespace qz::selftest::tests
