// qz_sim - host simulator, face-only mode (WP-23, milestone M1).
//
// Renders one watch face for a scripted state to a PNG, using the real qz_time zone engine, the
// real face registry and the real framebuffer PNG encoder. No device, no ESP-IDF.
//
//   qz_sim --face default|minimal|analog|stacked|words|dashboard|progress
//          --time 2026-10-06T08:15:00 --tz America/Chicago
//          --hour-format 12|24 --steps 7421 --goal 10000 --battery 76 [--charging]
//          [--power normal|low|saver|critical] --weather 'temp_c=18,code=61,age_min=20'
//          [--sync ok|never|failed|stale] [--time-invalid] [--utc] --scale 1|2|3 --out face.png
//
//   --time is local wall time in --tz (a DST gap is shifted forward); with --utc it is a UTC
//   instant converted to local. --scale 1 writes the 1-bit 200x200 PNG of gfx::encode_png; 2 and 3
//   write an 8-bit grayscale nearest-neighbour upscale. `--weather` also accepts high_c and low_c.
//
// Exit codes: 0 ok, 1 render/write failure, 2 usage error (usage text on stderr).
// Not yet implemented (need qz_app): --press, --scene, --console.
#include "qz_sim.hpp"

#include <fstream>
#include <iostream>
#include <span>
#include <string>

int main(int argc, char** argv) {
    // argv[1..] as a span of const char*.
    const std::span<char*> all(argv, static_cast<std::size_t>(argc));
    std::vector<const char*> rest;
    for (std::size_t i = 1; i < all.size(); ++i) {
        rest.push_back(all[i]);
    }
    const qz::sim::ParseOutcome parsed = qz::sim::parse_args(rest);
    if (!parsed.ok()) {
        std::cerr << "qz_sim: " << parsed.error << "\n" << qz::sim::usage_text();
        return 2;
    }
    if (parsed.args.help) {
        std::cout << qz::sim::usage_text();
        return 0;
    }
    const auto png = qz::sim::render_png(parsed.args);
    if (!png) {
        std::cerr << "qz_sim: render failed (" << qz::to_token(png.error().code) << ")\n";
        return 1;
    }
    std::ofstream out(parsed.args.out, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png->data()),
              static_cast<std::streamsize>(png->size()));
    out.close();
    if (!out) {
        std::cerr << "qz_sim: cannot write " << parsed.args.out << "\n";
        return 1;
    }
    std::cout << "wrote " << parsed.args.out << " (" << png->size() << " bytes)\n";
    return 0;
}
