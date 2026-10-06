// qz_golden: regenerates / verifies the scene goldens (docs/TEST_PLAN.md layer G).
//   qz_golden --update   write components/qz_selftest/golden/*.png and src/golden_crc.inc
//   qz_golden --check    compare byte for byte; exit 1 on any difference
// Options: --golden-dir DIR, --crc-inc FILE, --out-dir DIR (failure artifacts for --check).
#include "golden_support.hpp"

#include <iostream>

#ifndef QZ_GOLDEN_DIR
#error "QZ_GOLDEN_DIR must be defined by the build"
#endif
#ifndef QZ_GOLDEN_CRC_INC
#error "QZ_GOLDEN_CRC_INC must be defined by the build"
#endif

namespace {

namespace golden = qz::golden;

constexpr int kExitMismatch = 1;
constexpr int kExitUsage = 2;

int usage() {
    std::cerr << "usage: qz_golden --update | --check [--golden-dir DIR] [--crc-inc FILE]"
                 " [--out-dir DIR]\n";
    return kExitUsage;
}

int update(const golden::fs::path& dir,
           const golden::fs::path& inc,
           const qz::ui::FaceSource& faces) {
    std::size_t written = 0;
    for (const qz::selftest::Scene& scene : qz::selftest::scenes()) {
        const auto fb = golden::render(scene, faces);
        const auto png = fb ? golden::encode(*fb) : std::nullopt;
        if (!png) {
            std::cerr << "render failed: " << scene.name << "\n";
            return kExitUsage;
        }
        const auto old = golden::read_file(golden::png_path(dir, scene.name));
        if (!old || *old != *png) {
            if (!golden::write_file(golden::png_path(dir, scene.name), *png)) {
                std::cerr << "cannot write " << golden::png_path(dir, scene.name) << "\n";
                return kExitUsage;
            }
            ++written;
        }
    }
    for (const std::string& orphan : golden::orphan_goldens(dir)) {
        std::error_code ec;
        golden::fs::remove(dir / orphan, ec);
        std::cout << "removed stale golden " << orphan << "\n";
    }
    const auto text = golden::crc_table_text(faces);
    if (!text) {
        std::cerr << "render failed while building the CRC table\n";
        return kExitUsage;
    }
    const auto old_inc = golden::read_file(inc);
    const bool inc_changed = !old_inc || std::string(old_inc->begin(), old_inc->end()) != *text;
    if (inc_changed && !golden::write_text(inc, *text)) {
        std::cerr << "cannot write " << inc << "\n";
        return kExitUsage;
    }
    std::cout << "goldens: " << qz::selftest::scenes().size() << " scenes, " << written
              << " PNG(s) rewritten, CRC table " << (inc_changed ? "rewritten" : "unchanged")
              << "\n";
    return 0;
}

int check(const golden::fs::path& dir,
          const golden::fs::path& inc,
          const golden::fs::path& out_dir,
          const qz::ui::FaceSource& faces) {
    int bad = 0;
    for (const qz::selftest::Scene& scene : qz::selftest::scenes()) {
        const golden::SceneCheck r = golden::check_scene(scene, faces, dir, out_dir);
        if (!r.ok) {
            std::cout << "FAIL " << scene.name << ": " << r.message << "\n";
            ++bad;
        }
    }
    for (const std::string& orphan : golden::orphan_goldens(dir)) {
        std::cout << "FAIL stale golden without a scene: " << orphan << "\n";
        ++bad;
    }
    const auto text = golden::crc_table_text(faces);
    const auto committed = golden::read_file(inc);
    if (!text || !committed || std::string(committed->begin(), committed->end()) != *text) {
        std::cout << "FAIL golden_crc.inc is stale (run qz_golden --update)\n";
        ++bad;
    }
    std::cout << "goldens: " << qz::selftest::scenes().size() << " scenes, " << bad
              << " problem(s)\n";
    return bad == 0 ? 0 : kExitMismatch;
}

} // namespace

int main(int argc, char** argv) {
    bool do_update = false;
    bool do_check = false;
    golden::fs::path dir = QZ_GOLDEN_DIR;
    golden::fs::path inc = QZ_GOLDEN_CRC_INC;
    golden::fs::path out_dir = "golden_failures";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg =
            argv[i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const bool has_value = i + 1 < argc;
        if (arg == "--update") {
            do_update = true;
        } else if (arg == "--check") {
            do_check = true;
        } else if (arg == "--golden-dir" && has_value) {
            dir = argv[++i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        } else if (arg == "--crc-inc" && has_value) {
            inc = argv[++i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        } else if (arg == "--out-dir" && has_value) {
            out_dir = argv[++i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        } else {
            return usage();
        }
    }
    if (do_update == do_check) {
        return usage();
    }
    const qz::faces::Registry faces;
    return do_update ? update(dir, inc, faces) : check(dir, inc, out_dir, faces);
}
