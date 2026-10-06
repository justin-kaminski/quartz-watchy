// Self-test framework, canonical UI scenes and golden CRCs (ARCHITECTURE.md section 18).
// Same code runs on the device (real HAL) and on the host (qz_testkit fakes).
#pragma once

#include "qz/bma423/accel.hpp"
#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/board_io.hpp"
#include "qz/hal/kv_store.hpp"
#include "qz/hal/system.hpp"
#include "qz/ssd1681/panel.hpp"
#include "qz/ui/ui.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::selftest {

enum class Outcome : std::uint8_t { kPass, kFail, kSkip };

/// Hardware/services available to tests; null pointers make dependent tests skip.
struct Context {
    ssd1681::Panel* panel = nullptr;
    bma423::Accelerometer* accel = nullptr;
    hal::Adc* battery_adc = nullptr;
    hal::BoardIo* io = nullptr;
    hal::KvStore* kv = nullptr; ///< tests write only to namespace "qz_test"
    hal::System* system = nullptr;
    hal::Delay* delay = nullptr;
    const ui::FaceSource* faces = nullptr;
    bool interactive = false; ///< allow tests that wait for button presses
};

using TestFn = Outcome (*)(Context& ctx, FixedString<64>& detail);

struct TestCase {
    std::string_view suite; ///< "drivers", "screens", "settings", "time", "interactive"
    std::string_view name;
    TestFn fn;
    bool interactive;
};

[[nodiscard]] std::span<const TestCase> all_tests() noexcept;

struct TestReport {
    std::string_view suite;
    std::string_view name;
    Outcome outcome = Outcome::kSkip;
    std::uint32_t duration_ms = 0;
    FixedString<64> detail;
};

class ReportSink {
public:
    virtual ~ReportSink() = default;
    virtual void on_result(const TestReport& report) = 0;
};

struct Summary {
    std::uint16_t passed = 0, failed = 0, skipped = 0;
};

/// Runs tests whose "suite" or "suite/name" matches filter ("" or "all" = every test).
/// `now_ms` measures durations. Non-interactive tests never block on input.
Summary
run(Context& ctx, std::string_view filter, ReportSink& sink, std::uint32_t (*now_ms)()) noexcept;

// ---- scenes (fixtures for goldens, simulator --scene, on-device CRC checks) ----

/// A canonical UI state: start screen + input script + fixture WatchState.
struct Scene {
    std::string_view name; ///< file name of the golden: golden/<name>.png
    ui::ScreenId screen;
    void (*build_state)(ui::WatchState& state, settings::Settings& backing) noexcept;
    std::span<const model::InputEvent> inputs; ///< applied after show(screen)
};

[[nodiscard]] std::span<const Scene> scenes() noexcept;
/// Renders a scene exactly as the device would (same code paths).
Status
render_scene(const Scene& scene, const ui::FaceSource& faces, gfx::Framebuffer& out) noexcept;

struct GoldenCrc {
    std::string_view scene;
    std::uint32_t crc32;
};
/// Generated table (src/golden_crc.inc) from the host goldens.
[[nodiscard]] std::span<const GoldenCrc> golden_crcs() noexcept;

} // namespace qz::selftest
