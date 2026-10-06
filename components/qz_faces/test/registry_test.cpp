// Registry tests (WP-16): ids unique and stable, names, lookup fallbacks, ui::FaceSource adapter.
#include "qz/faces/registry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string_view>

namespace qz::faces {
namespace {

// gtest macros inflate the cognitive-complexity score of table-driven checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)

ui::WatchState sample_state() {
    ui::WatchState s;
    s.time_valid = true;
    s.local.date = {2026, 10, 6};
    s.local.time = {14, 32, 0};
    s.local.weekday = time::Weekday::kTuesday;
    s.steps.today = 4321;
    s.steps.goal = 10000;
    s.battery.valid = true;
    s.battery.percent = 85;
    s.sync = model::SyncIndicator::kOk;
    return s;
}

gfx::Framebuffer render_with(const FaceDescriptor& d, const ui::WatchState& s) {
    gfx::Framebuffer fb;
    gfx::Canvas canvas(fb);
    d.render(s, canvas);
    return fb;
}

TEST(RegistryTest, IdsAreUniqueAndNamesNonEmptyAndUnique) {
    std::set<std::uint8_t> ids;
    std::set<std::string_view> names;
    for (const FaceDescriptor& d : descriptors()) {
        EXPECT_TRUE(ids.insert(d.id).second) << "duplicate id " << int{d.id};
        EXPECT_FALSE(d.name.empty());
        EXPECT_TRUE(names.insert(d.name).second) << "duplicate name " << d.name;
        EXPECT_NE(d.render, nullptr);
    }
    EXPECT_GE(descriptors().size(), 2U);
}

TEST(RegistryTest, IdsAreStablePersistedValues) {
    // These ids are stored in settings (`face`): changing one silently switches a user's face.
    ASSERT_GE(descriptors().size(), 2U);
    EXPECT_EQ(descriptors()[0].id, 0);
    EXPECT_EQ(descriptors()[0].name, "default");
    EXPECT_EQ(descriptors()[1].id, 1);
    EXPECT_EQ(descriptors()[1].name, "minimal");
}

TEST(RegistryTest, IsRegistered) {
    EXPECT_TRUE(is_registered(0));
    EXPECT_TRUE(is_registered(1));
    EXPECT_FALSE(is_registered(2));
    EXPECT_FALSE(is_registered(255));
}

TEST(RegistryTest, FaceSourceAdapterListsEveryDescriptor) {
    const Registry reg;
    const ui::FaceSource& source = reg;
    ASSERT_EQ(source.count(), descriptors().size());
    for (std::size_t i = 0; i < source.count(); ++i) {
        EXPECT_EQ(source.id_at(i), descriptors()[i].id);
        EXPECT_EQ(source.name_of(source.id_at(i)), descriptors()[i].name);
    }
    EXPECT_EQ(source.name_of(200), "");
    EXPECT_EQ(source.id_at(source.count()), 0) << "out of range index falls back to the default id";
}

TEST(RegistryTest, RenderDispatchesByIdAndUnknownIdUsesFaceZero) {
    const Registry reg;
    const ui::WatchState s = sample_state();
    for (const FaceDescriptor& d : descriptors()) {
        gfx::Framebuffer fb;
        gfx::Canvas canvas(fb);
        reg.render(d.id, s, canvas);
        EXPECT_EQ(fb.bits, render_with(d, s).bits) << d.name;
    }
    gfx::Framebuffer unknown;
    gfx::Canvas canvas(unknown);
    reg.render(99, s, canvas);
    EXPECT_EQ(unknown.bits, render_with(descriptors()[0], s).bits);
}

TEST(RegistryTest, FacesLookDifferent) {
    const ui::WatchState s = sample_state();
    EXPECT_NE(render_with(descriptors()[0], s).bits, render_with(descriptors()[1], s).bits);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::faces
