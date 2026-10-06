// WP-14 stand-in for the system screens that WP-15 draws (see screen.hpp). The input behaviour
// below is part of the navigation graph (ARCHITECTURE.md section 15) and stays when WP-15 swaps
// render() for the real pages.
#include "screen.hpp"

namespace qz::ui {
namespace {

using model::Button;

std::string_view title_of(ScreenId id) noexcept {
    switch (id) {
        case ScreenId::kStepsHistory:
            return "Steps";
        case ScreenId::kWeatherDetail:
            return "Weather";
        case ScreenId::kSyncNow:
            return "Sync now";
        case ScreenId::kProvisioning:
            return "Wi-Fi setup";
        case ScreenId::kDiagnostics:
            return "Diagnostics";
        case ScreenId::kAbout:
            return "About";
        case ScreenId::kFactoryReset:
            return "Factory reset";
        case ScreenId::kChargeMe:
            return "Charge me";
        case ScreenId::kStatusOverlay:
            return "Status";
        case ScreenId::kFace:
        case ScreenId::kMenu:
        case ScreenId::kTimeDateEditor:
        case ScreenId::kTimezonePicker:
        case ScreenId::kChoice:
        case ScreenId::kWeatherSettings:
        case ScreenId::kLocationEditor:
        case ScreenId::kStepGoalEditor:
        case ScreenId::kCount:
            break;
    }
    return {};
}

} // namespace

void SystemScreen::enter(const WatchState& /*state*/, std::uint8_t /*param*/) noexcept {
    page_ = 0;
}

void SystemScreen::render(const WatchState& /*state*/, gfx::Canvas& canvas) const noexcept {
    draw_title_bar(canvas, title_of(id_));
    if (id_ == ScreenId::kDiagnostics) {
        TextBuilder<16> p;
        p.put("page ").put_uint(page_ + 1U).put("/").put_uint(tuning::kDiagPageCount);
        draw_caption(canvas, 100, p.view());
    }
}

Outcome SystemScreen::handle(const model::InputEvent& event, const WatchState& state) noexcept {
    switch (id_) {
        case ScreenId::kSyncNow:
            if (is_click(event, Button::kMenu)) {
                Outcome o;
                if (state.op_phase != OpPhase::kRunning) {
                    o.with(make_action(ActionKind::kSyncNow)); // retry
                }
                return o;
            }
            break;
        case ScreenId::kProvisioning:
            if (is_click(event, Button::kBack)) {
                Outcome o = Outcome::pop();
                o.with(make_action(ActionKind::kStopProvisioning));
                return o;
            }
            return Outcome::none();
        case ScreenId::kDiagnostics:
            if (event.kind == model::InputKind::kClick && list_direction(event) != 0) {
                page_ = static_cast<std::uint8_t>(
                    wrap_index(page_, list_direction(event), tuning::kDiagPageCount));
                return Outcome::none();
            }
            if (is_click(event, Button::kMenu)) {
                Outcome o;
                if (page_ + 1U == tuning::kDiagPageCount) {
                    o.with(make_action(ActionKind::kRunSelfTest));
                }
                return o;
            }
            break;
        case ScreenId::kFactoryReset:
            // Hold MENU 3 s: the recognizer's single MENU long-hold event (GestureTiming).
            if (event.button == Button::kMenu && event.kind == model::InputKind::kRepeat &&
                event.held_ms >= tuning::kFactoryResetHoldMs) {
                Outcome o = Outcome::home();
                o.with(make_action(ActionKind::kFactoryReset));
                return o;
            }
            break;
        case ScreenId::kChargeMe:
            return Outcome::none(); // no input is meaningful while the battery is critical
        case ScreenId::kStepsHistory:
        case ScreenId::kWeatherDetail:
        case ScreenId::kAbout:
        case ScreenId::kStatusOverlay:
        case ScreenId::kFace:
        case ScreenId::kMenu:
        case ScreenId::kTimeDateEditor:
        case ScreenId::kTimezonePicker:
        case ScreenId::kChoice:
        case ScreenId::kWeatherSettings:
        case ScreenId::kLocationEditor:
        case ScreenId::kStepGoalEditor:
        case ScreenId::kCount:
            break;
    }
    if (is_click(event, Button::kBack)) {
        return Outcome::pop();
    }
    return Outcome::none();
}

} // namespace qz::ui
