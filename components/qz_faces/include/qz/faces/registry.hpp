// Watch faces and the explicit registry table (ARCHITECTURE.md section 15.1).
// Adding a face: one file in src/, one row in the registry table, scenes + goldens.
#pragma once

#include "qz/gfx/framebuffer.hpp"
#include "qz/ui/ui.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::faces {

/// A face is a pure renderer. Must handle every WatchState variant: time invalid, 12/24h,
/// weather fresh/stale/hidden, sync indicators, saver/charging/critical marks, goal on/off.
struct FaceDescriptor {
    std::uint8_t id;       ///< stable, persisted in settings; 0 = default face
    std::string_view name; ///< console/menu name
    void (*render)(const ui::WatchState& state, gfx::Canvas& canvas) noexcept;
};

/// Compile-time table, ids unique (checked by a host test).
[[nodiscard]] std::span<const FaceDescriptor> descriptors() noexcept;
[[nodiscard]] bool is_registered(std::uint8_t id) noexcept;

/// ui::FaceSource over descriptors().
class Registry final : public ui::FaceSource {
public:
    [[nodiscard]] std::size_t count() const override;
    [[nodiscard]] std::uint8_t id_at(std::size_t index) const override;
    [[nodiscard]] std::string_view name_of(std::uint8_t id) const override;
    void render(std::uint8_t id, const ui::WatchState& state, gfx::Canvas& canvas) const override;
};

} // namespace qz::faces
