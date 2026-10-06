// Suite "screens": scene coverage and CRC32 of every rendered scene against golden_crc.inc
// (the host golden tool also compares the full PNGs).
#include "detail.hpp"
#include "qz/faces/registry.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "tests.hpp"

#include <cstdint>

namespace qz::selftest::tests {

namespace {

const GoldenCrc* find_golden(std::string_view scene) noexcept {
    for (const GoldenCrc& g : golden_crcs()) {
        if (g.scene == scene) {
            return &g;
        }
    }
    return nullptr;
}

/// Face id a scene selects (its fixture's settings.face_id).
std::uint8_t face_of(const Scene& scene) noexcept {
    settings::Settings backing = settings::defaults();
    ui::WatchState state;
    state.settings = &backing;
    if (scene.build_state != nullptr) {
        scene.build_state(state, backing);
    }
    return backing.face_id;
}

} // namespace

Outcome scenes_cover(Context& /*ctx*/, Detail& d) noexcept {
    const std::span<const Scene> all = scenes();
    for (std::uint8_t id = 0; id < static_cast<std::uint8_t>(ui::ScreenId::kCount); ++id) {
        bool found = false;
        for (const Scene& s : all) {
            found = found || static_cast<std::uint8_t>(s.screen) == id;
        }
        if (!found) {
            return fail(d,
                        Text()
                            .put("no scene for screen ")
                            .put(ui::screen_name(static_cast<ui::ScreenId>(id)))
                            .view());
        }
    }
    for (const faces::FaceDescriptor& face : faces::descriptors()) {
        bool found = false;
        for (const Scene& s : all) {
            found = found || (s.screen == ui::ScreenId::kFace && face_of(s) == face.id);
        }
        if (!found) {
            return fail(d, Text().put("no scene for face ").put(face.name).view());
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            if (all[i].name == all[j].name) {
                return fail(d, Text().put("duplicate scene ").put(all[i].name).view());
            }
        }
    }
    return pass(d,
                Text().num(static_cast<std::int64_t>(all.size())).put(" scenes cover all").view());
}

Outcome scenes_crc(Context& ctx, Detail& d) noexcept {
    if (ctx.faces == nullptr) {
        return skip(d, "no face source");
    }
    static gfx::Framebuffer fb; // 5 kB scratch, app task only (not stack)
    std::int64_t bad = 0;
    Text first_text;
    for (const Scene& scene : scenes()) {
        fb.clear();
        const Status s = render_scene(scene, *ctx.faces, fb);
        const GoldenCrc* golden = find_golden(scene.name);
        const std::uint32_t crc = fb.crc32();
        if (!s || golden == nullptr || golden->crc32 != crc) {
            if (bad == 0) {
                std::string_view why = " crc";
                if (!s) {
                    why = " render";
                } else if (golden == nullptr) {
                    why = " no golden";
                }
                first_text.put(scene.name).put(why);
            }
            ++bad;
        }
    }
    if (bad != 0) {
        return fail(d, Text().num(bad).put(" bad: ").put(first_text.view()).view());
    }
    if (golden_crcs().size() != scenes().size()) {
        return fail(d, "golden table has stale entries");
    }
    return pass(d,
                Text().num(static_cast<std::int64_t>(scenes().size())).put(" scenes match").view());
}

} // namespace qz::selftest::tests
