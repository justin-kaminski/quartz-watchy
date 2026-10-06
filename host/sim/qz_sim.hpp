// Host simulator library (WP-23, face-only mode): argument parsing, scripted WatchState
// construction and rendering of a watch face to a PNG. Host only; may use the standard library.
// Time is derived through the real qz_time TimeZone engine, the face through the real registry.
#pragma once

#include "qz/core/result.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/model/types.hpp"
#include "qz/time/civil.hpp"
#include "qz/ui/ui.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qz::sim {

/// Optional `--weather` value; `has_high_low` is set when both high_c and low_c were given.
struct WeatherSpec {
    int temp_c = 0;
    int high_c = 0;
    int low_c = 0;
    bool has_high_low = false;
    int wmo_code = 0;
    std::uint32_t age_min = 0;
    constexpr bool operator==(const WeatherSpec&) const noexcept = default;
};

/// Command line, fully parsed and range-checked. Defaults render a plausible morning.
struct Args {
    std::string face = "default";
    time::CivilDate date{2026, 10, 6};
    time::CivilTime time{8, 15, 0};
    bool time_is_utc = false; ///< --utc: --time is a UTC instant, converted to local
    bool time_valid = true;   ///< --time-invalid clears it
    std::string tz = "America/Chicago";
    model::HourFormat hour_format = model::HourFormat::k24h;
    std::uint32_t steps = 0;
    std::uint32_t goal = 10000;
    std::uint8_t battery = 100;
    bool charging = false;
    model::PowerLevel power = model::PowerLevel::kNormal;
    std::optional<WeatherSpec> weather;
    model::SyncIndicator sync = model::SyncIndicator::kNone;
    int scale = 1; ///< 1..3, nearest-neighbour
    std::string out = "face.png";
    bool help = false;
};

struct ParseOutcome {
    Args args;
    std::string error; ///< empty = success
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// `argv` excludes the program name. Unknown or malformed arguments give a non-empty error.
[[nodiscard]] ParseOutcome parse_args(std::span<const char* const> argv);
[[nodiscard]] std::string_view usage_text() noexcept;

/// The WatchState plus the instants it was derived from.
struct Scene {
    ui::WatchState state;
    time::UnixSeconds utc = 0; ///< meaningful when state.time_valid
};
/// Builds the WatchState. Fails (kBadArgs) on an unknown zone. A local time inside a DST gap is
/// not an error: it is resolved with GapPolicy::kEarlier (shifted forward by the gap length).
[[nodiscard]] Result<Scene> build_scene(const Args& args);
[[nodiscard]] Result<gfx::Framebuffer> render_frame(const Args& args);
/// PNG bytes of the face: 200x200 1-bit via gfx::encode_png at scale 1, 8-bit grayscale otherwise.
[[nodiscard]] Result<std::vector<std::uint8_t>> render_png(const Args& args);

} // namespace qz::sim
