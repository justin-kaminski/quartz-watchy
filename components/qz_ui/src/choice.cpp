#include "choice.hpp"

#include "qz/time/tz.hpp"

namespace qz::ui {
namespace {

using settings::Key;

struct Item {
    std::string_view value; ///< canonical settings string
    std::string_view label;
};

constexpr std::array<Item, 2> kHourItems{{{"24h", "24-hour"}, {"12h", "12-hour"}}};
constexpr std::array<Item, 2> kUnitItems{{{"c", "Celsius"}, {"f", "Fahrenheit"}}};
constexpr std::array<Item, 3> kConnItems{
    {{"off", "Off"}, {"time", "Time only"}, {"time+weather", "Time + weather"}}};
constexpr std::array<Item, 2> kVibItems{{{"on", "On"}, {"off", "Off"}}};
constexpr std::array<Item, 5> kSyncItems{{{"6", "6 hours"},
                                          {"12", "12 hours"},
                                          {"24", "24 hours"},
                                          {"48", "2 days"},
                                          {"168", "7 days"}}};
constexpr std::array<Item, 5> kWeatherItems{{{"30", "30 min"},
                                             {"60", "1 hour"},
                                             {"120", "2 hours"},
                                             {"180", "3 hours"},
                                             {"360", "6 hours"}}};

struct KindInfo {
    std::string_view title;
    Key key;
    std::span<const Item> items; ///< empty for kFace (items come from the FaceSource)
};

const KindInfo& kind_info(ChoiceKind kind) noexcept {
    static constexpr std::array<KindInfo, static_cast<std::size_t>(ChoiceKind::kCount)> kTable{{
        {"Clock format", Key::kHourFormat, kHourItems},
        {"Units", Key::kTempUnit, kUnitItems},
        {"Connectivity", Key::kConnectivity, kConnItems},
        {"Vibration", Key::kVibration, kVibItems},
        {"Watch face", Key::kFace, {}},
        {"Sync interval", Key::kSyncIntervalH, kSyncItems},
        {"Weather update", Key::kWeatherIntervalMin, kWeatherItems},
    }};
    const auto i = static_cast<std::size_t>(kind);
    return kTable[i < kTable.size() ? i : 0];
}

} // namespace

// ---- ChoiceScreen --------------------------------------------------------------------------
std::size_t ChoiceScreen::count() const noexcept {
    const KindInfo& info = kind_info(kind_);
    return kind_ == ChoiceKind::kFace ? faces_.count() : info.items.size();
}

FixedString<48> ChoiceScreen::value_at(std::size_t index) const noexcept {
    FixedString<48> out;
    if (kind_ == ChoiceKind::kFace) {
        TextBuilder<8> b;
        b.put_uint(faces_.id_at(index));
        (void)out.assign(b.view()); // <= 3 digits
    } else {
        (void)out.assign(kind_info(kind_).items[index].value); // short literals
    }
    return out;
}

void ChoiceScreen::enter(const WatchState& state, std::uint8_t param) noexcept {
    kind_ = param < static_cast<std::uint8_t>(ChoiceKind::kCount) ? static_cast<ChoiceKind>(param)
                                                                  : ChoiceKind::kHourFormat;
    selected_ = 0;
    current_ = 0;
    if (state.settings == nullptr) {
        return;
    }
    if (kind_ == ChoiceKind::kFace) {
        for (std::size_t i = 0; i < faces_.count(); ++i) {
            if (faces_.id_at(i) == state.settings->face_id) {
                current_ = i;
            }
        }
    } else {
        std::array<char, 24> buf{};
        const std::size_t n = settings::format_value(*state.settings, kind_info(kind_).key, buf);
        const std::string_view cur(buf.data(), n);
        const auto items = kind_info(kind_).items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].value == cur) {
                current_ = i;
            }
        }
    }
    selected_ = current_;
}

void ChoiceScreen::fill_row(std::size_t index, ListRow& row) const noexcept {
    if (kind_ == ChoiceKind::kFace) {
        row.label.put(faces_.name_of(faces_.id_at(index)));
    } else {
        row.label.put(kind_info(kind_).items[index].label);
    }
    row.mark = index == current_ ? 1 : 0;
}

void ChoiceScreen::render(const WatchState& /*state*/, gfx::Canvas& canvas) const noexcept {
    draw_list(
        canvas, kind_info(kind_).title, count(), selected_, [this](std::size_t i, ListRow& r) {
            fill_row(i, r);
        });
    draw_hint(canvas, "MENU save  BACK cancel");
}

Outcome ChoiceScreen::handle(const model::InputEvent& event, const WatchState& /*state*/) noexcept {
    const std::size_t n = count();
    if (const int dir = list_direction(event); dir != 0) {
        if (n > 0) {
            selected_ = wrap_index(selected_, dir, n);
        }
        return Outcome::none();
    }
    if (is_click(event, model::Button::kBack)) {
        return Outcome::pop();
    }
    if (is_click(event, model::Button::kMenu)) {
        Outcome o = Outcome::pop();
        if (n > 0) {
            o.with(make_set_setting(kind_info(kind_).key, value_at(selected_).view()));
        }
        return o;
    }
    return Outcome::none();
}

// ---- ZonePicker ----------------------------------------------------------------------------
void ZonePicker::enter(const WatchState& state, std::uint8_t /*param*/) noexcept {
    selected_ = 0;
    current_ = 0;
    if (state.settings == nullptr) {
        return;
    }
    const auto zones = time::builtin_zones();
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (zones[i].name == state.settings->tz_name.view()) {
            current_ = i;
            selected_ = i;
            return;
        }
    }
}

void ZonePicker::render(const WatchState& /*state*/, gfx::Canvas& canvas) const noexcept {
    const auto zones = time::builtin_zones();
    draw_list(canvas, "Time zone", zones.size(), selected_, [&](std::size_t i, ListRow& r) {
        r.label.put(zones[i].label);
        r.mark = i == current_ ? 1 : 0;
    });
    draw_hint(canvas, "MENU select  BACK cancel");
}

Outcome ZonePicker::handle(const model::InputEvent& event, const WatchState& /*state*/) noexcept {
    const auto zones = time::builtin_zones();
    if (const int dir = list_direction(event); dir != 0) {
        if (!zones.empty()) {
            selected_ =
                wrap_index(selected_, dir * static_cast<int>(step_multiplier(event)), zones.size());
        }
        return Outcome::none();
    }
    if (is_click(event, model::Button::kBack)) {
        return Outcome::pop();
    }
    if (is_click(event, model::Button::kMenu)) {
        Outcome o = Outcome::pop();
        if (!zones.empty()) {
            o.with(make_set_setting(Key::kTimeZone, zones[selected_].name));
        }
        return o;
    }
    return Outcome::none();
}

} // namespace qz::ui
