// Explicit face registry (ARCHITECTURE.md section 15.1). Adding a face: one source file, one row
// in kTable, scenes + goldens. Ids are persisted in settings: never renumber or reuse one.
#include "qz/faces/registry.hpp"

#include "face_common.hpp"

#include <algorithm>
#include <array>

namespace qz::faces {

namespace {

constexpr std::uint8_t kDefaultFaceId = 0;

constexpr std::array<FaceDescriptor, 7> kTable{{
    {kDefaultFaceId, "default", &render_default_face},
    {1, "minimal", &render_minimal_face},
    {2, "analog", &render_analog_face},
    {3, "stacked", &render_stacked_face},
    {4, "words", &render_words_face},
    {5, "dashboard", &render_dashboard_face},
    {6, "progress", &render_progress_face},
}};

/// Descriptor for `id`; unknown ids fall back to the default face (row 0).
[[nodiscard]] const FaceDescriptor& lookup(std::uint8_t id) noexcept {
    for (const FaceDescriptor& d : kTable) {
        if (d.id == id) {
            return d;
        }
    }
    return kTable[0];
}

} // namespace

std::span<const FaceDescriptor> descriptors() noexcept {
    return kTable;
}

bool is_registered(std::uint8_t id) noexcept {
    return std::ranges::any_of(kTable, [id](const FaceDescriptor& d) { return d.id == id; });
}

std::size_t Registry::count() const {
    return kTable.size();
}

std::uint8_t Registry::id_at(std::size_t index) const {
    return index < kTable.size() ? kTable[index].id : kDefaultFaceId;
}

std::string_view Registry::name_of(std::uint8_t id) const {
    return is_registered(id) ? lookup(id).name : std::string_view{};
}

void Registry::render(std::uint8_t id, const ui::WatchState& state, gfx::Canvas& canvas) const {
    lookup(id).render(state, canvas);
}

} // namespace qz::faces
