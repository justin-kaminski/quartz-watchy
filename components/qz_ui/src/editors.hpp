// Editors work on a private copy and emit their Action(s) only on save (ARCHITECTURE.md section
// 15).
#pragma once

#include "screen.hpp"

namespace qz::ui {

/// Fields Y-M-D h:m; MENU = next field / save on the last, BACK = previous field / cancel on the
/// first. Saves one kSetTime (seconds zeroed).
class TimeDateEditor final : public Screen {
public:
    static constexpr std::uint8_t kFieldCount = 5;
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    void step(int delta) noexcept;
    time::CivilDate date_{};
    time::CivilTime time_{};
    std::uint8_t field_ = 0;
};

/// 0 (off)..50000 in steps of 500. Saves one kSetSetting(goal).
class StepGoalEditor final : public Screen {
public:
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    std::uint32_t goal_ = 0;
};

/// Digit editor for latitude (sign + DD.dd) and longitude (sign + DDD.dd). MENU = next position
/// / save on the last, BACK = previous / cancel on the first. Saves two kSetSetting actions
/// (lat, lon): a location is a pair, and the settings schema has one key per coordinate.
class LocationEditor final : public Screen {
public:
    static constexpr std::uint8_t kPositionCount = 11; ///< lat sign, 4 digits, lon sign, 5 digits
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    void step(int delta) noexcept;
    std::int32_t lat_units_ = 0; ///< magnitude, 0.01 degree units
    std::int32_t lon_units_ = 0;
    bool lat_neg_ = false;
    bool lon_neg_ = false;
    std::uint8_t pos_ = 0;
};

} // namespace qz::ui
