// Shared fixtures for the qz_ui host tests.
#pragma once

#include "qz/ui/ui.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace qz::ui::test {

/// Two faces (ids 0 and 3); remembers which id was rendered last.
class FakeFaces final : public FaceSource {
public:
    [[nodiscard]] std::size_t count() const override { return 2; }
    [[nodiscard]] std::uint8_t id_at(std::size_t index) const override {
        return index == 0 ? 0 : 3;
    }
    [[nodiscard]] std::string_view name_of(std::uint8_t id) const override {
        return id == 0 ? "Alpha" : (id == 3 ? "Bravo" : "");
    }
    void render(std::uint8_t id, const WatchState& /*state*/, gfx::Canvas& canvas) const override {
        last_rendered = id;
        const std::int16_t w = id == 0 ? 10 : 20;
        canvas.fill_rect({0, 0, w, 10}, gfx::Color::kBlack);
    }
    mutable int last_rendered = -1;
};

inline model::InputEvent click(model::Button b) {
    return {b, model::InputKind::kClick, 50, 0};
}
inline model::InputEvent hold(model::Button b) {
    return {b, model::InputKind::kHold, 700, 0};
}
inline model::InputEvent repeat(model::Button b, std::uint32_t held_ms) {
    return {b, model::InputKind::kRepeat, held_ms, 0};
}

/// A Ui plus a populated WatchState/Settings and click helpers.
struct Harness {
    FakeFaces faces;
    Ui ui{faces};
    settings::Settings settings = settings::defaults();
    WatchState state;

    Harness() {
        state.settings = &settings;
        state.time_valid = true;
        state.local.date = {2026, 10, 6};
        state.local.time = {14, 32, 42};
        state.local.weekday = time::Weekday::kTuesday;
        state.steps.goal = settings.step_goal;
        state.tz_label = "UTC";
    }

    ActionList send(const model::InputEvent& e) { return ui.handle(e, state); }
    ActionList click(model::Button b) { return send(test::click(b)); }
    ActionList clicks(model::Button b, int n) {
        ActionList last;
        for (int i = 0; i < n; ++i) {
            last = click(b);
        }
        return last;
    }
    /// Opens the menu from the face and selects row `index` (without entering it).
    void goto_menu_row(int index) {
        ui.reset_to_face();
        (void)click(model::Button::kMenu);
        (void)clicks(model::Button::kDown, index);
    }
    [[nodiscard]] gfx::Framebuffer draw() const {
        gfx::Framebuffer fb;
        gfx::Canvas canvas(fb);
        ui.render(state, canvas);
        return fb;
    }
};

inline int black_pixels(const gfx::Framebuffer& fb) {
    int n = 0;
    for (std::int16_t y = 0; y < gfx::kHeight; ++y) {
        for (std::int16_t x = 0; x < gfx::kWidth; ++x) {
            n += fb.get(x, y) == gfx::Color::kBlack ? 1 : 0;
        }
    }
    return n;
}

} // namespace qz::ui::test
