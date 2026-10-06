// Suite "drivers": device checks written against the HAL and driver interfaces (section 18).
#include "detail.hpp"
#include "qz/bma423/accel.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/power/power.hpp"
#include "qz/ssd1681/panel.hpp"
#include "tests.hpp"
#include "tuning.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace qz::selftest::tests {

namespace {

/// Two scratch frames for the display tests (10 kB of static storage, not stack: the app task
/// stack is small). Self-tests run on the app task only, never concurrently.
struct Frames {
    gfx::Framebuffer a;
    gfx::Framebuffer b;
};

Frames& frames() noexcept {
    static Frames f;
    return f;
}

/// Border + caption: a pattern that makes a refresh visible to the owner.
void draw_pattern(gfx::Framebuffer& fb, std::string_view caption, std::int16_t bar_w) noexcept {
    fb.clear();
    gfx::Canvas c(fb);
    c.rect({0, 0, gfx::kWidth, gfx::kHeight}, gfx::Color::kBlack);
    c.rect({4, 4, gfx::kWidth - 8, gfx::kHeight - 8}, gfx::Color::kBlack);
    c.text_aligned(0,
                   100,
                   gfx::kWidth,
                   caption,
                   gfx::font(gfx::FontId::kMedium),
                   gfx::Align::kCenter,
                   gfx::Color::kBlack);
    c.fill_rect({20, 140, bar_w, 20}, gfx::Color::kBlack);
}

} // namespace

Outcome display_init(Context& ctx, Detail& d) noexcept {
    if (ctx.panel == nullptr) {
        return skip(d, "no panel");
    }
    const Status s = ctx.panel->init();
    if (!s) {
        return fail_error(d, "init", s.error());
    }
    return pass(d, "reset + busy cycle ok");
}

Outcome display_temp(Context& ctx, Detail& d) noexcept {
    if (ctx.panel == nullptr) {
        return skip(d, "no panel");
    }
    if (const Status s = ctx.panel->init(); !s) {
        return fail_error(d, "init", s.error());
    }
    const Result<std::int16_t> t = ctx.panel->temperature_dc();
    if (!t) {
        return t.error().code == Errc::kUnsupported ? skip(d, "no bus read support")
                                                    : fail_error(d, "temp", t.error());
    }
    Text text;
    text.put("panel ").deci(*t).put(" C");
    if (*t < tuning::kPanelTempMinDc || *t > tuning::kPanelTempMaxDc) {
        return fail(d, text.view());
    }
    return pass(d, text.view());
}

Outcome display_full(Context& ctx, Detail& d) noexcept {
    if (ctx.panel == nullptr) {
        return skip(d, "no panel");
    }
    Frames& f = frames();
    f.a.clear();
    draw_pattern(f.b, "SELF-TEST FULL", 160);
    if (const Status s = ctx.panel->init(); !s) {
        return fail_error(d, "init", s.error());
    }
    if (const Status s = ctx.panel->update(f.a, f.b, ssd1681::UpdateMode::kFull); !s) {
        return fail_error(d, "full update", s.error());
    }
    return pass(d, "full refresh ok");
}

Outcome display_partial(Context& ctx, Detail& d) noexcept {
    if (ctx.panel == nullptr) {
        return skip(d, "no panel");
    }
    Frames& f = frames();
    draw_pattern(f.a, "SELF-TEST FULL", 160);
    draw_pattern(f.b, "SELF-TEST PARTIAL", 80);
    if (const Status s = ctx.panel->init(); !s) {
        return fail_error(d, "init", s.error());
    }
    if (const Status s = ctx.panel->update(f.a, f.b, ssd1681::UpdateMode::kPartial); !s) {
        return fail_error(d, "partial update", s.error());
    }
    return pass(d, "partial refresh ok");
}

Outcome accel_chip_id(Context& ctx, Detail& d) noexcept {
    if (ctx.accel == nullptr) {
        return skip(d, "no accelerometer");
    }
    const Result<std::uint8_t> id = ctx.accel->chip_id();
    if (!id) {
        return fail_error(d, "chip id", id.error());
    }
    Text text;
    text.put("id ").num(*id);
    return *id == bma423::kChipId ? pass(d, text.view()) : fail(d, text.view());
}

Outcome accel_features(Context& ctx, Detail& d) noexcept {
    if (ctx.accel == nullptr) {
        return skip(d, "no accelerometer");
    }
    const Result<bool> engine = ctx.accel->feature_engine_ok();
    if (!engine) {
        return fail_error(d, "feature engine", engine.error());
    }
    if (!*engine) {
        return fail(d, "feature config not loaded");
    }
    // The step counter is enabled by init(); that it answers is all a static check can show
    // (counting is verified by walking: HARDWARE_BRINGUP B7).
    const Result<std::uint32_t> steps = ctx.accel->step_count();
    if (!steps) {
        return fail_error(d, "step counter", steps.error());
    }
    Text text;
    text.put("steps ").num(static_cast<std::int64_t>(*steps));
    return pass(d, text.view());
}

Outcome battery_adc(Context& ctx, Detail& d) noexcept {
    if (ctx.battery_adc == nullptr) {
        return skip(d, "no adc");
    }
    const Result<std::uint16_t> pin = ctx.battery_adc->read_pin_mv();
    if (!pin) {
        return fail_error(d, "adc", pin.error());
    }
    const std::uint16_t mv =
        power::battery_mv_from_pin(*pin, tuning::kBatteryDividerNum, tuning::kBatteryDividerDen);
    Text text;
    text.put("battery ").num(mv).put(" mV");
    const bool in_range = mv >= tuning::kBatteryMinMv && mv <= tuning::kBatteryMaxMv;
    return in_range ? pass(d, text.view()) : fail(d, text.view());
}

Outcome buttons_idle(Context& ctx, Detail& d) noexcept {
    if (ctx.io == nullptr) {
        return skip(d, "no board io");
    }
    const std::uint8_t mask = ctx.io->pressed_buttons();
    if (mask != 0) {
        Text text;
        text.put("buttons held, mask ").num(mask);
        return fail(d, text.view());
    }
    return pass(d, "all released");
}

Outcome usb_pins(Context& ctx, Detail& d) noexcept {
    if (ctx.io == nullptr) {
        return skip(d, "no board io");
    }
    const bool usb = ctx.io->usb_present();
    const bool stat = ctx.io->charging();
    Text text;
    text.put("usb ").num(usb ? 1 : 0).put(" stat ").num(stat ? 1 : 0);
    // STAT is high whenever USB is present, charging or full [R1 s5]; STAT without USB is an
    // electrical contradiction (swapped pins or a floating input).
    return (stat && !usb) ? fail(d, text.view()) : pass(d, text.view());
}

Outcome rtc_clock(Context& ctx, Detail& d) noexcept {
    if (ctx.system == nullptr) {
        return skip(d, "no system");
    }
    const hal::SlowClockInfo info = ctx.system->slow_clock();
    if (!info.external_crystal) {
        return fail(d, "slow clock is not the 32 kHz crystal");
    }
    if (info.measured_hz == 0) {
        return skip(d, "slow clock not calibrated");
    }
    const std::int64_t diff =
        static_cast<std::int64_t>(info.measured_hz) - tuning::kSlowClockNominalHz;
    const std::int64_t abs_diff = diff < 0 ? -diff : diff;
    Text text;
    text.put("Hz ").num(info.measured_hz);
    const bool in_tolerance =
        abs_diff * 1'000'000 <= tuning::kSlowClockTolerancePpm * tuning::kSlowClockNominalHz;
    return in_tolerance ? pass(d, text.view()) : fail(d, text.view());
}

Outcome nvs_roundtrip(Context& ctx, Detail& d) noexcept {
    if (ctx.kv == nullptr) {
        return skip(d, "no kv store");
    }
    hal::KvStore& kv = *ctx.kv;
    constexpr std::string_view ns = tuning::kTestNamespace;
    constexpr std::uint32_t kMarker = 0xA5A55A5AU;
    constexpr std::string_view kText = "selftest";
    constexpr std::array<std::uint8_t, 4> kBlob{1, 2, 3, 4};

    if (const Status s = kv.set_u32(ns, "u32", kMarker); !s) {
        return fail_error(d, "write u32", s.error());
    }
    if (const Status s = kv.set_str(ns, "str", kText); !s) {
        return fail_error(d, "write str", s.error());
    }
    if (const Status s = kv.set_blob(ns, "blob", kBlob); !s) {
        return fail_error(d, "write blob", s.error());
    }
    if (const Status s = kv.commit(); !s) {
        return fail_error(d, "commit", s.error());
    }

    const Result<std::uint32_t> u = kv.get_u32(ns, "u32");
    if (!u) {
        return fail_error(d, "read u32", u.error());
    }
    std::array<char, 16> str{};
    const Result<std::size_t> n = kv.get_str(ns, "str", str);
    if (!n) {
        return fail_error(d, "read str", n.error());
    }
    std::array<std::uint8_t, 8> blob{};
    const Result<std::size_t> bn = kv.get_blob(ns, "blob", blob);
    if (!bn) {
        return fail_error(d, "read blob", bn.error());
    }
    if (*u != kMarker || std::string_view(str.data(), *n) != kText || *bn != kBlob.size() ||
        !std::equal(kBlob.begin(), kBlob.end(), blob.begin())) {
        return fail(d, "read-back differs from write");
    }

    // erase: key, then the whole test namespace; the key must be gone.
    if (const Status s = kv.erase_key(ns, "u32"); !s) {
        return fail_error(d, "erase key", s.error());
    }
    const Result<std::uint32_t> gone = kv.get_u32(ns, "u32");
    if (gone || gone.error().code != Errc::kNotFound) {
        return fail(d, "erased key still readable");
    }
    if (const Status s = kv.erase_namespace(ns); !s) {
        return fail_error(d, "erase ns", s.error());
    }
    if (const Status s = kv.commit(); !s) {
        return fail_error(d, "commit", s.error());
    }
    const Result<std::size_t> gone_str = kv.get_str(ns, "str", str);
    if (gone_str) {
        return fail(d, "erased namespace still readable");
    }
    return pass(d, "u32 str blob write/read/erase");
}

Outcome system_info(Context& ctx, Detail& d) noexcept {
    if (ctx.system == nullptr) {
        return skip(d, "no system");
    }
    const hal::FirmwareInfo fw = ctx.system->firmware();
    if (fw.version.empty()) {
        return fail(d, "empty firmware version");
    }
    const hal::HeapInfo heap = ctx.system->heap();
    if (heap.free_bytes == 0) {
        return fail(d, "heap reports 0 bytes free");
    }
    Text text;
    text.put("fw ").put(fw.version).put(" heap ").num(heap.free_bytes / 1024U).put(" kB");
    return pass(d, text.view());
}

} // namespace qz::selftest::tests
