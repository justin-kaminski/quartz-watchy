// Screen stack and the static facts of the navigation graph (ARCHITECTURE.md section 15).
#pragma once

#include "qz/core/containers.hpp"
#include "qz/ui/ui.hpp"
#include "tuning.hpp"

#include <cstddef>
#include <cstdint>

namespace qz::ui {

struct Frame {
    ScreenId id = ScreenId::kFace;
    std::uint8_t param = 0;
};

/// The face is always the bottom frame. Fixed storage.
class Navigator {
public:
    Navigator() noexcept { reset(); }
    void reset() noexcept;
    [[nodiscard]] ScreenId current() const noexcept { return frames_[frames_.size() - 1].id; }
    [[nodiscard]] std::uint8_t current_param() const noexcept {
        return frames_[frames_.size() - 1].param;
    }
    [[nodiscard]] std::size_t depth() const noexcept { return frames_.size(); }
    /// False (and unchanged) when the stack is full.
    [[nodiscard]] bool push(ScreenId id, std::uint8_t param) noexcept;
    /// Never pops the face.
    void pop() noexcept;
    /// True when any frame is a menu-tree screen (leaving it needs a full refresh).
    [[nodiscard]] bool contains_menu_tree() const noexcept;

private:
    StaticVector<Frame, tuning::kStackDepth> frames_;
};

/// Menu, editors, pickers, settings, SyncNow ... everything reached through the menu.
[[nodiscard]] bool is_menu_tree(ScreenId id) noexcept;
/// Ancestors (excluding the face) the user passes through to reach `id`, outermost first.
/// Used by Ui::show() to rebuild a realistic stack.
[[nodiscard]] StaticVector<ScreenId, 3> ancestors_of(ScreenId id, std::uint8_t param) noexcept;
/// Idle timeout for the screen on top of the stack.
[[nodiscard]] std::int64_t idle_timeout_ms(ScreenId id) noexcept;

} // namespace qz::ui
