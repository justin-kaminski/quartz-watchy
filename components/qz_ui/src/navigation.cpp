#include "navigation.hpp"

#include <algorithm>

namespace qz::ui {

void Navigator::reset() noexcept {
    frames_.clear();
    const bool ok = frames_.push_back(Frame{});
    QZ_ASSERT(ok);
}

bool Navigator::push(ScreenId id, std::uint8_t param) noexcept {
    return frames_.push_back(Frame{id, param});
}

void Navigator::pop() noexcept {
    if (frames_.size() > 1) {
        frames_.pop_back();
    }
}

bool Navigator::contains_menu_tree() const noexcept {
    return std::ranges::any_of(frames_, [](const Frame& f) { return is_menu_tree(f.id); });
}

bool is_menu_tree(ScreenId id) noexcept {
    switch (id) {
        case ScreenId::kFace:
        case ScreenId::kStepsHistory:
        case ScreenId::kWeatherDetail:
        case ScreenId::kChargeMe:
        case ScreenId::kStatusOverlay:
        case ScreenId::kCount:
            return false;
        case ScreenId::kMenu:
        case ScreenId::kTimeDateEditor:
        case ScreenId::kTimezonePicker:
        case ScreenId::kChoice:
        case ScreenId::kWeatherSettings:
        case ScreenId::kLocationEditor:
        case ScreenId::kStepGoalEditor:
        case ScreenId::kSyncNow:
        case ScreenId::kProvisioning:
        case ScreenId::kDiagnostics:
        case ScreenId::kAbout:
        case ScreenId::kFactoryReset:
        case ScreenId::kPhone:
        case ScreenId::kPhoneSync:
            return true;
    }
    return false;
}

StaticVector<ScreenId, 3> ancestors_of(ScreenId id, std::uint8_t param) noexcept {
    StaticVector<ScreenId, 3> chain;
    const auto add = [&chain](ScreenId a) {
        const bool ok = chain.push_back(a);
        QZ_ASSERT(ok);
    };
    switch (id) {
        case ScreenId::kFace:
        case ScreenId::kStepsHistory:
        case ScreenId::kWeatherDetail:
        case ScreenId::kChargeMe:
        case ScreenId::kStatusOverlay:
        case ScreenId::kMenu:
        case ScreenId::kCount:
            break;
        case ScreenId::kLocationEditor:
        case ScreenId::kProvisioning:
            add(ScreenId::kMenu);
            add(ScreenId::kWeatherSettings);
            break;
        case ScreenId::kPhoneSync:
            add(ScreenId::kMenu);
            add(ScreenId::kPhone);
            break;
        case ScreenId::kChoice:
            add(ScreenId::kMenu);
            if (param == static_cast<std::uint8_t>(ChoiceKind::kWeatherInterval)) {
                add(ScreenId::kWeatherSettings);
            }
            break;
        case ScreenId::kTimeDateEditor:
        case ScreenId::kTimezonePicker:
        case ScreenId::kWeatherSettings:
        case ScreenId::kStepGoalEditor:
        case ScreenId::kSyncNow:
        case ScreenId::kDiagnostics:
        case ScreenId::kAbout:
        case ScreenId::kFactoryReset:
        case ScreenId::kPhone:
            add(ScreenId::kMenu);
            break;
    }
    return chain;
}

std::int64_t idle_timeout_ms(ScreenId id) noexcept {
    switch (id) {
        case ScreenId::kFace:
        case ScreenId::kChargeMe:
            return tuning::kIdleFaceMs;
        case ScreenId::kStatusOverlay:
            return tuning::kIdleOverlayMs;
        case ScreenId::kSyncNow:
            return tuning::kIdleSyncMs;
        case ScreenId::kProvisioning:
            return tuning::kIdleProvisioningMs;
        case ScreenId::kPhoneSync:
            return tuning::kIdlePhoneSyncMs;
        case ScreenId::kStepsHistory:
        case ScreenId::kWeatherDetail:
        case ScreenId::kMenu:
        case ScreenId::kTimeDateEditor:
        case ScreenId::kTimezonePicker:
        case ScreenId::kChoice:
        case ScreenId::kWeatherSettings:
        case ScreenId::kLocationEditor:
        case ScreenId::kStepGoalEditor:
        case ScreenId::kDiagnostics:
        case ScreenId::kAbout:
        case ScreenId::kFactoryReset:
        case ScreenId::kPhone:
        case ScreenId::kCount:
            return tuning::kIdleMenuMs;
    }
    return tuning::kIdleMenuMs;
}

} // namespace qz::ui
