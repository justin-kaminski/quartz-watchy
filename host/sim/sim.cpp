#include "qz/core/crc32.hpp"
#include "qz/faces/registry.hpp"
#include "qz/time/tz.hpp"
#include "qz/weather/provider.hpp"
#include "qz_sim.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>

namespace qz::sim {
namespace {

constexpr std::string_view kUsage =
    "usage: qz_sim [options]\n"
    "  --face NAME            registered face (default|minimal), default: default\n"
    "  --time YYYY-MM-DDTHH:MM:SS  local wall time (default 2026-10-06T08:15:00)\n"
    "  --utc                  interpret --time as UTC and convert to local\n"
    "  --time-invalid         face shows the clock-not-set state\n"
    "  --tz IANA_NAME         built-in zone, e.g. America/Chicago (default)\n"
    "  --hour-format 12|24    default 24\n"
    "  --steps N --goal N     steps today / goal (0 = none), default 0 / 10000\n"
    "  --battery 0..100 [--charging] [--power normal|low|saver|critical]\n"
    "  --weather 'temp_c=18,code=61,age_min=20[,high_c=21,low_c=9]'  (WMO code)\n"
    "  --sync ok|never|failed|stale\n"
    "  --scale 1|2|3          nearest-neighbour upscale (default 1)\n"
    "  --out FILE             PNG path (default face.png)\n"
    "  --help\n";

constexpr int kMinYear = 1970;
constexpr int kMaxYear = 2099;
constexpr std::int64_t kMaxCount = 999'999;
constexpr int kWeatherIntervalMin = 60; ///< freshness uses the default sync interval [ASSUMED]
constexpr int kBatteryEmptyMv = 3300;   ///< linear mV for a given percent [ASSUMED]
constexpr int kBatteryMvPer10Pct = 90;

template<class T>
bool parse_int(std::string_view s, std::int64_t lo, std::int64_t hi, T& out) {
    std::int64_t v = 0;
    const std::span<const char> chars(s);
    const char* end = chars.data() + chars.size();
    const auto r =
        std::from_chars(chars.data(), end, v); // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (s.empty() || r.ec != std::errc{} || r.ptr != end || v < lo || v > hi) {
        return false;
    }
    out = static_cast<T>(v);
    return true;
}

bool parse_time(std::string_view s, time::CivilDate& date, time::CivilTime& t) {
    // YYYY-MM-DDTHH:MM:SS
    if (s.size() != 19 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':') {
        return false;
    }
    std::int32_t year = 0;
    std::uint8_t month = 0;
    std::uint8_t day = 0;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    if (!parse_int(s.substr(0, 4), kMinYear, kMaxYear, year) ||
        !parse_int(s.substr(5, 2), 1, 12, month) || !parse_int(s.substr(8, 2), 1, 31, day) ||
        !parse_int(s.substr(11, 2), 0, 23, hour) || !parse_int(s.substr(14, 2), 0, 59, minute) ||
        !parse_int(s.substr(17, 2), 0, 59, second)) {
        return false;
    }
    const time::CivilDate d{static_cast<std::int16_t>(year), month, day};
    const time::CivilTime tm{hour, minute, second};
    if (!time::is_valid(d, tm)) {
        return false;
    }
    date = d;
    t = tm;
    return true;
}

bool parse_weather(std::string_view s, WeatherSpec& w) {
    WeatherSpec out;
    bool have_temp = false;
    bool have_high = false;
    bool have_low = false;
    while (!s.empty()) {
        const std::size_t comma = s.find(',');
        const std::string_view item = s.substr(0, comma);
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            return false;
        }
        const std::string_view key = item.substr(0, eq);
        const std::string_view val = item.substr(eq + 1);
        bool ok = false;
        if (key == "temp_c") {
            ok = parse_int(val, -90, 60, out.temp_c);
            have_temp = ok;
        } else if (key == "high_c") {
            ok = parse_int(val, -90, 60, out.high_c);
            have_high = ok;
        } else if (key == "low_c") {
            ok = parse_int(val, -90, 60, out.low_c);
            have_low = ok;
        } else if (key == "code") {
            ok = parse_int(val, 0, 99, out.wmo_code);
        } else if (key == "age_min") {
            ok = parse_int(val, 0, 100'000, out.age_min);
        }
        if (!ok) {
            return false;
        }
    }
    out.has_high_low = have_high && have_low;
    if (!have_temp || have_high != have_low) {
        return false;
    }
    w = out;
    return true;
}

struct NamedPower {
    std::string_view name;
    model::PowerLevel level;
};
constexpr std::array<NamedPower, 4> kPowers{{{"normal", model::PowerLevel::kNormal},
                                             {"low", model::PowerLevel::kLow},
                                             {"saver", model::PowerLevel::kSaver},
                                             {"critical", model::PowerLevel::kCritical}}};
struct NamedSync {
    std::string_view name;
    model::SyncIndicator sync;
};
constexpr std::array<NamedSync, 4> kSyncs{{{"ok", model::SyncIndicator::kOk},
                                           {"never", model::SyncIndicator::kNeverSynced},
                                           {"failed", model::SyncIndicator::kLastFailed},
                                           {"stale", model::SyncIndicator::kStale}}};

bool is_face(std::string_view name) {
    const auto all = faces::descriptors();
    return std::ranges::any_of(all,
                               [name](const faces::FaceDescriptor& d) { return d.name == name; });
}

/// Returns 1 = applied, 0 = invalid value, -1 = option not handled here.
int apply_text_value(Args& a, std::string_view opt, std::string_view v) {
    bool ok = false;
    if (opt == "--face") {
        ok = is_face(v);
        if (ok) {
            a.face = std::string(v);
        }
    } else if (opt == "--time") {
        ok = parse_time(v, a.date, a.time);
    } else if (opt == "--tz") {
        ok = time::find_zone(v) != nullptr;
        if (ok) {
            a.tz = std::string(v);
        }
    } else if (opt == "--hour-format") {
        ok = v == "12" || v == "24";
        a.hour_format = v == "12" ? model::HourFormat::k12h : model::HourFormat::k24h;
    } else if (opt == "--scale") {
        ok = parse_int(v, 1, 3, a.scale);
    } else if (opt == "--out") {
        ok = !v.empty();
        if (ok) {
            a.out = std::string(v);
        }
    } else {
        return -1;
    }
    return ok ? 1 : 0;
}

int apply_data_value(Args& a, std::string_view opt, std::string_view v) {
    bool ok = false;
    if (opt == "--steps") {
        ok = parse_int(v, 0, kMaxCount, a.steps);
    } else if (opt == "--goal") {
        ok = parse_int(v, 0, kMaxCount, a.goal);
    } else if (opt == "--battery") {
        ok = parse_int(v, 0, 100, a.battery);
    } else if (opt == "--power") {
        const auto* const it = std::ranges::find(kPowers, v, &NamedPower::name);
        ok = it != kPowers.end();
        if (ok) {
            a.power = it->level;
        }
    } else if (opt == "--weather") {
        WeatherSpec w;
        ok = parse_weather(v, w);
        if (ok) {
            a.weather = w;
        }
    } else if (opt == "--sync") {
        const auto* const it = std::ranges::find(kSyncs, v, &NamedSync::name);
        ok = it != kSyncs.end();
        if (ok) {
            a.sync = it->sync;
        }
    } else {
        return -1;
    }
    return ok ? 1 : 0;
}

/// Applies one `--option value` pair. Returns an error message, empty on success.
std::string apply_value(Args& a, std::string_view opt, std::string_view v) {
    int r = apply_text_value(a, opt, v);
    if (r < 0) {
        r = apply_data_value(a, opt, v);
    }
    if (r < 0) {
        return "unknown argument: " + std::string(opt);
    }
    return r == 1 ? std::string{} : "invalid value for " + std::string(opt) + ": " + std::string(v);
}

class VectorSink final : public gfx::ByteSink {
public:
    explicit VectorSink(std::vector<std::uint8_t>& out) : out_(out) {}
    Status write(std::span<const std::uint8_t> bytes) override {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
        return {};
    }

private:
    std::vector<std::uint8_t>& out_;
};

void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24U));
    v.push_back(static_cast<std::uint8_t>(x >> 16U));
    v.push_back(static_cast<std::uint8_t>(x >> 8U));
    v.push_back(static_cast<std::uint8_t>(x));
}

void put_chunk(std::vector<std::uint8_t>& png,
               std::string_view tag,
               std::span<const std::uint8_t> data) {
    put_be32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t crc_from = png.size();
    png.insert(png.end(), tag.begin(), tag.end());
    png.insert(png.end(), data.begin(), data.end());
    put_be32(png, crc32(std::span<const std::uint8_t>(png).subspan(crc_from)));
}

std::uint32_t adler32(std::span<const std::uint8_t> data) {
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16U) | a;
}

/// 8-bit grayscale PNG of the framebuffer upscaled by `scale` (nearest neighbour), zlib stored
/// blocks (no compression; host review images only).
std::vector<std::uint8_t> encode_scaled_png(const gfx::Framebuffer& fb, int scale) {
    const auto side = static_cast<std::size_t>(gfx::kWidth) * static_cast<std::size_t>(scale);
    std::vector<std::uint8_t> raw;
    raw.reserve((side + 1U) * side);
    for (std::size_t y = 0; y < side; ++y) {
        raw.push_back(0); // filter: none
        for (std::size_t x = 0; x < side; ++x) {
            const bool ink =
                fb.get(static_cast<std::int16_t>(x / static_cast<std::size_t>(scale)),
                       static_cast<std::int16_t>(y / static_cast<std::size_t>(scale))) ==
                gfx::Color::kBlack;
            raw.push_back(ink ? 0x00 : 0xFF);
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    constexpr std::size_t kMaxStored = 65'535;
    for (std::size_t off = 0; off < raw.size(); off += kMaxStored) {
        const std::size_t n = std::min(kMaxStored, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0); // BFINAL, BTYPE = stored
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8U));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8U));
        z.insert(z.end(),
                 raw.begin() + static_cast<std::ptrdiff_t>(off),
                 raw.begin() + static_cast<std::ptrdiff_t>(off + n));
    }
    put_be32(z, adler32(raw));

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(side));
    put_be32(ihdr, static_cast<std::uint32_t>(side));
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0}); // 8-bit, grayscale, deflate, filter 0, no interlace
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", {});
    return png;
}

} // namespace

std::string_view usage_text() noexcept {
    return kUsage;
}

ParseOutcome parse_args(std::span<const char* const> argv) {
    ParseOutcome r;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string_view opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            r.args.help = true;
        } else if (opt == "--charging") {
            r.args.charging = true;
        } else if (opt == "--time-invalid") {
            r.args.time_valid = false;
        } else if (opt == "--utc") {
            r.args.time_is_utc = true;
        } else if (!opt.starts_with("--")) {
            r.error = "unexpected argument: " + std::string(opt);
        } else if (i + 1 >= argv.size()) {
            r.error = "missing value for " + std::string(opt);
        } else {
            r.error = apply_value(r.args, opt, argv[i + 1]);
            ++i;
        }
        if (!r.ok()) {
            break;
        }
    }
    return r;
}

Result<Scene> build_scene(const Args& args) {
    const time::TzEntry* zone = time::find_zone(args.tz);
    if (zone == nullptr) {
        return Errc::kBadArgs;
    }
    const Result<time::TimeZone> tz = time::TimeZone::parse(zone->posix);
    if (!tz) {
        return tz.error();
    }
    Scene scene;
    ui::WatchState& s = scene.state;

    // UTC instant: either given directly (--utc) or resolved from local wall time (DST gap ->
    // shifted forward, overlap -> first occurrence).
    if (args.time_is_utc) {
        const std::int64_t tod_s = (std::int64_t{args.time.hour} * 3600) +
                                   (std::int64_t{args.time.minute} * 60) + args.time.second;
        scene.utc = (time::days_from_civil(args.date) * time::kSecondsPerDay) + tod_s;
    } else {
        const Result<time::UnixSeconds> utc =
            tz->to_utc(args.date, args.time, time::GapPolicy::kEarlier);
        if (!utc) {
            return utc.error();
        }
        scene.utc = *utc;
    }
    s.time_valid = args.time_valid;
    if (args.time_valid) {
        s.local = tz->to_local(scene.utc);
    }
    s.hour_format = args.hour_format;
    s.tz_label = zone->label;

    s.steps.today = args.steps;
    s.steps.goal = args.goal;

    s.battery.valid = true;
    s.battery.percent = args.battery;
    s.battery.mv =
        static_cast<std::uint16_t>(kBatteryEmptyMv + ((args.battery * kBatteryMvPer10Pct) / 10));
    s.battery.level = args.power;
    s.battery.usb_present = args.charging;
    s.battery.charging = args.charging;
    s.power = args.power;

    s.sync = args.sync;
    s.conn_mode = model::ConnectivityMode::kOff;
    if (args.weather) {
        s.conn_mode = model::ConnectivityMode::kTimeWeather;
    } else if (args.sync != model::SyncIndicator::kNone) {
        s.conn_mode = model::ConnectivityMode::kTimeOnly;
    }
    if (args.weather) {
        const WeatherSpec& w = *args.weather;
        s.weather.valid = 1;
        s.weather.temp_dc = static_cast<std::int16_t>(w.temp_c * 10);
        s.weather.has_high_low = w.has_high_low ? 1 : 0;
        s.weather.high_dc = static_cast<std::int16_t>(w.high_c * 10);
        s.weather.low_dc = static_cast<std::int16_t>(w.low_c * 10);
        s.weather.condition = weather::condition_from_wmo(w.wmo_code);
        s.weather_age_s = w.age_min * 60U;
        s.weather.fetched_utc = scene.utc - static_cast<time::UnixSeconds>(s.weather_age_s);
        s.weather_freshness = weather::freshness(
            s.weather, scene.utc, static_cast<std::uint16_t>(kWeatherIntervalMin), args.time_valid);
    }
    return scene;
}

Result<gfx::Framebuffer> render_frame(const Args& args) {
    const Result<Scene> scene = build_scene(args);
    if (!scene) {
        return scene.error();
    }
    const auto all = faces::descriptors();
    const auto it = std::ranges::find(all, args.face, &faces::FaceDescriptor::name);
    if (it == all.end()) {
        return Errc::kBadArgs;
    }
    gfx::Framebuffer fb;
    gfx::Canvas canvas(fb);
    it->render(scene->state, canvas);
    return fb;
}

Result<std::vector<std::uint8_t>> render_png(const Args& args) {
    const Result<gfx::Framebuffer> fb = render_frame(args);
    if (!fb) {
        return fb.error();
    }
    if (args.scale > 1) {
        return encode_scaled_png(*fb, args.scale);
    }
    std::vector<std::uint8_t> bytes;
    VectorSink sink(bytes);
    const Status st = gfx::encode_png(*fb, sink);
    if (!st) {
        return st.error();
    }
    return bytes;
}

} // namespace qz::sim
