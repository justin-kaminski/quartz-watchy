// The top-level Menu and the WeatherSettings list (both are plain pick-a-row screens).
#pragma once

#include "screen.hpp"

namespace qz::ui {

class MenuScreen final : public Screen {
public:
    explicit MenuScreen(const FaceSource& faces) noexcept : faces_(faces) {}
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    const FaceSource& faces_;
    std::size_t cursor_ = 0; ///< index among the *visible* items
};

class WeatherSettingsScreen final : public Screen {
public:
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    std::size_t cursor_ = 0;
};

} // namespace qz::ui
