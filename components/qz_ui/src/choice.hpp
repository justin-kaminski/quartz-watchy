// Radio lists: ChoiceScreen (small enum/interval/face settings) and ZonePicker (time zones).
#pragma once

#include "screen.hpp"

namespace qz::ui {

class ChoiceScreen final : public Screen {
public:
    explicit ChoiceScreen(const FaceSource& faces) noexcept : faces_(faces) {}
    /// `param` = ChoiceKind; the current setting value is pre-selected.
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    [[nodiscard]] std::size_t count() const noexcept;
    void fill_row(std::size_t index, ListRow& row) const noexcept;
    [[nodiscard]] FixedString<48> value_at(std::size_t index) const noexcept;

    const FaceSource& faces_;
    ChoiceKind kind_ = ChoiceKind::kHourFormat;
    std::size_t selected_ = 0;
    std::size_t current_ = 0; ///< index of the saved value (radio mark)
};

/// Time-zone list over time::builtin_zones() (sorted by offset); saves the IANA name.
class ZonePicker final : public Screen {
public:
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    std::size_t selected_ = 0;
    std::size_t current_ = 0;
};

} // namespace qz::ui
