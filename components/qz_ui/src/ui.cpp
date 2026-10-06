// Ui: screen table, stack navigation, global rules (Hold MENU -> face, idle, refresh hint).
#include "qz/ui/ui.hpp"

#include "choice.hpp"
#include "editors.hpp"
#include "menu.hpp"
#include "navigation.hpp"
#include "screen.hpp"

#include <memory>

namespace qz::ui {
namespace {

struct NameEntry {
    ScreenId id;
    std::string_view name;
};
constexpr std::array<NameEntry, static_cast<std::size_t>(ScreenId::kCount)> kNames{{
    {ScreenId::kFace, "face"},
    {ScreenId::kStepsHistory, "steps_history"},
    {ScreenId::kWeatherDetail, "weather_detail"},
    {ScreenId::kMenu, "menu"},
    {ScreenId::kTimeDateEditor, "time_date_editor"},
    {ScreenId::kTimezonePicker, "timezone_picker"},
    {ScreenId::kChoice, "choice"},
    {ScreenId::kWeatherSettings, "weather_settings"},
    {ScreenId::kLocationEditor, "location_editor"},
    {ScreenId::kStepGoalEditor, "step_goal_editor"},
    {ScreenId::kSyncNow, "sync_now"},
    {ScreenId::kProvisioning, "provisioning"},
    {ScreenId::kDiagnostics, "diagnostics"},
    {ScreenId::kAbout, "about"},
    {ScreenId::kFactoryReset, "factory_reset"},
    {ScreenId::kChargeMe, "charge_me"},
    {ScreenId::kStatusOverlay, "status_overlay"},
}};

constexpr bool names_in_enum_order() {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (static_cast<std::size_t>(kNames[i].id) != i) {
            return false;
        }
    }
    return true;
}
static_assert(names_in_enum_order());

constexpr std::int64_t kUsPerMs = 1000;

/// The face: renders the selected face; UP/DOWN/MENU open the glance screens and the menu.
class FaceScreen final : public Screen {
public:
    explicit FaceScreen(const FaceSource& faces) noexcept : faces_(faces) {}
    void enter(const WatchState& /*state*/, std::uint8_t /*param*/) noexcept override {}
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override {
        const std::uint8_t id = state.settings != nullptr ? state.settings->face_id : 0;
        faces_.render(id, state, canvas);
    }
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& /*state*/) noexcept override {
        using model::Button;
        if (is_click(event, Button::kUp)) {
            return Outcome::push(ScreenId::kStepsHistory);
        }
        if (is_click(event, Button::kDown)) {
            return Outcome::push(ScreenId::kWeatherDetail);
        }
        if (is_click(event, Button::kMenu)) {
            return Outcome::push(ScreenId::kMenu);
        }
        if (is_click(event, Button::kBack)) {
            Outcome o;
            o.with(make_action(ActionKind::kFullRefresh));
            return o;
        }
        return Outcome::none();
    }

private:
    const FaceSource& faces_;
};

} // namespace

std::string_view screen_name(ScreenId id) noexcept {
    const auto i = static_cast<std::size_t>(id);
    return i < kNames.size() ? kNames[i].name : std::string_view{};
}

Result<ScreenId> screen_from_name(std::string_view name) noexcept {
    for (const NameEntry& e : kNames) {
        if (e.name == name) {
            return e.id;
        }
    }
    return Errc::kNotFound;
}

// ---- Ui::Impl ------------------------------------------------------------------------------
struct Ui::Impl {
    explicit Impl(const FaceSource& faces) noexcept
        : face(faces), menu(faces), choice(faces), steps(ScreenId::kStepsHistory),
          weather_detail(ScreenId::kWeatherDetail), sync(ScreenId::kSyncNow),
          provisioning(ScreenId::kProvisioning), diagnostics(ScreenId::kDiagnostics),
          about(ScreenId::kAbout), factory_reset(ScreenId::kFactoryReset),
          charge_me(ScreenId::kChargeMe), status(ScreenId::kStatusOverlay) {
        table[static_cast<std::size_t>(ScreenId::kFace)] = &face;
        table[static_cast<std::size_t>(ScreenId::kStepsHistory)] = &steps;
        table[static_cast<std::size_t>(ScreenId::kWeatherDetail)] = &weather_detail;
        table[static_cast<std::size_t>(ScreenId::kMenu)] = &menu;
        table[static_cast<std::size_t>(ScreenId::kTimeDateEditor)] = &time_date;
        table[static_cast<std::size_t>(ScreenId::kTimezonePicker)] = &zones;
        table[static_cast<std::size_t>(ScreenId::kChoice)] = &choice;
        table[static_cast<std::size_t>(ScreenId::kWeatherSettings)] = &weather_settings;
        table[static_cast<std::size_t>(ScreenId::kLocationEditor)] = &location;
        table[static_cast<std::size_t>(ScreenId::kStepGoalEditor)] = &step_goal;
        table[static_cast<std::size_t>(ScreenId::kSyncNow)] = &sync;
        table[static_cast<std::size_t>(ScreenId::kProvisioning)] = &provisioning;
        table[static_cast<std::size_t>(ScreenId::kDiagnostics)] = &diagnostics;
        table[static_cast<std::size_t>(ScreenId::kAbout)] = &about;
        table[static_cast<std::size_t>(ScreenId::kFactoryReset)] = &factory_reset;
        table[static_cast<std::size_t>(ScreenId::kChargeMe)] = &charge_me;
        table[static_cast<std::size_t>(ScreenId::kStatusOverlay)] = &status;
    }

    [[nodiscard]] Screen& screen(ScreenId id) noexcept {
        return *table[static_cast<std::size_t>(id)];
    }
    [[nodiscard]] const Screen& screen(ScreenId id) const noexcept {
        return *table[static_cast<std::size_t>(id)];
    }

    void push(ScreenId id, std::uint8_t param, const WatchState& state) noexcept {
        if (nav.push(id, param)) {
            screen(id).enter(state, param);
        }
    }

    /// Back to the face; leaving the menu tree asks for a full refresh (ghost clean-up).
    void go_home() noexcept {
        if (nav.contains_menu_tree()) {
            hint = RefreshHint::kFull;
        }
        nav.reset();
    }

    ActionList apply(const Outcome& out, const WatchState& state) noexcept {
        switch (out.nav.kind) {
            case Nav::Kind::kNone:
                break;
            case Nav::Kind::kPush:
                push(out.nav.target, out.nav.param, state);
                break;
            case Nav::Kind::kPop: {
                const bool was_menu = is_menu_tree(nav.current());
                nav.pop();
                if (was_menu && nav.current() == ScreenId::kFace) {
                    hint = RefreshHint::kFull;
                }
                break;
            }
            case Nav::Kind::kHome:
                go_home();
                break;
        }
        return out.actions;
    }

    Navigator nav;
    RefreshHint hint = RefreshHint::kPartial;

    FaceScreen face;
    MenuScreen menu;
    TimeDateEditor time_date;
    ZonePicker zones;
    ChoiceScreen choice;
    WeatherSettingsScreen weather_settings;
    LocationEditor location;
    StepGoalEditor step_goal;
    SystemScreen steps;
    SystemScreen weather_detail;
    SystemScreen sync;
    SystemScreen provisioning;
    SystemScreen diagnostics;
    SystemScreen about;
    SystemScreen factory_reset;
    SystemScreen charge_me;
    SystemScreen status;
    std::array<Screen*, static_cast<std::size_t>(ScreenId::kCount)> table{};
};

// ---- Ui ------------------------------------------------------------------------------------
Ui::Ui(const FaceSource& faces) noexcept
    : impl_(std::construct_at(reinterpret_cast<Impl*>(storage_.data()), faces)) {
    static_assert(sizeof(Impl) <= kStorageBytes, "raise Ui::kStorageBytes");
    static_assert(alignof(Impl) <= alignof(std::max_align_t));
}

Ui::~Ui() {
    std::destroy_at(impl_);
}

void Ui::reset_to_face() noexcept {
    impl_->nav.reset();
    impl_->hint = RefreshHint::kPartial;
}

ScreenId Ui::current() const noexcept {
    return impl_->nav.current();
}

Status Ui::show_choice(ChoiceKind kind, const WatchState& state) noexcept {
    if (kind >= ChoiceKind::kCount) {
        return Errc::kBadArgs;
    }
    impl_->go_home();
    const auto param = static_cast<std::uint8_t>(kind);
    for (const ScreenId a : ancestors_of(ScreenId::kChoice, param)) {
        impl_->push(a, 0, state);
    }
    impl_->push(ScreenId::kChoice, param, state);
    return {};
}

Status Ui::show(ScreenId id, const WatchState& state) noexcept {
    if (id >= ScreenId::kCount) {
        return Errc::kBadArgs;
    }
    if (id == ScreenId::kChoice) {
        return show_choice(ChoiceKind::kHourFormat, state);
    }
    impl_->go_home();
    if (id == ScreenId::kFace) {
        return {};
    }
    for (const ScreenId a : ancestors_of(id, 0)) {
        impl_->push(a, 0, state);
    }
    impl_->push(id, 0, state);
    return {};
}

ActionList Ui::handle(const model::InputEvent& event, const WatchState& state) noexcept {
    const ScreenId cur = impl_->nav.current();
    // Hold MENU -> face from anywhere, except FactoryReset where MENU is the confirm gesture.
    if (event.button == model::Button::kMenu && event.kind == model::InputKind::kHold &&
        cur != ScreenId::kFactoryReset) {
        impl_->go_home();
        return {};
    }
    return impl_->apply(impl_->screen(cur).handle(event, state), state);
}

void Ui::render(const WatchState& state, gfx::Canvas& canvas) const noexcept {
    canvas.reset_clip();
    canvas.clear();
    impl_->screen(impl_->nav.current()).render(state, canvas);
}

RefreshHint Ui::refresh_hint() const noexcept {
    return impl_->hint;
}

void Ui::clear_refresh_hint() noexcept {
    impl_->hint = RefreshHint::kPartial;
}

std::int64_t Ui::idle_timeout_us() const noexcept {
    return idle_timeout_ms(impl_->nav.current()) * kUsPerMs;
}

bool Ui::idle_expired(std::int64_t now_rtc_us, std::int64_t last_input_rtc_us) const noexcept {
    return now_rtc_us - last_input_rtc_us >= idle_timeout_us();
}

} // namespace qz::ui
