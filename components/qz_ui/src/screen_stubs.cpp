// System screens: input behaviour (the navigation graph, ARCHITECTURE.md section 15) lives here;
// drawing is in system_screens.cpp (WP-15).
#include "screen.hpp"
#include "system_screens.hpp"

namespace qz::ui {
namespace {

using model::Button;

} // namespace

void SystemScreen::enter(const WatchState& /*state*/, std::uint8_t /*param*/) noexcept {
    page_ = 0;
}

void SystemScreen::render(const WatchState& state, gfx::Canvas& canvas) const noexcept {
    render_system_screen(id_, page_, state, canvas);
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
        case ScreenId::kPhoneSync:
            if (is_click(event, Button::kBack)) {
                Outcome o = Outcome::pop();
                o.with(make_action(ActionKind::kStopPhoneSync));
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
        case ScreenId::kPhone:
        case ScreenId::kCount:
            break;
    }
    if (is_click(event, Button::kBack)) {
        return Outcome::pop();
    }
    return Outcome::none();
}

} // namespace qz::ui
