#include "menu.hpp"

namespace qz::ui {
namespace {

using model::Button;
using settings::Key;

// ---- Menu items (ARCHITECTURE.md section 15, in order) -------------------------------------
enum class MenuItem : std::uint8_t {
    kTimeDate,
    kTimeZone,
    kHourFormat,
    kUnits,
    kConnectivity,
    kWeather,
    kSyncNow,
    kPhone,
    kStepGoal,
    kVibration,
    kFace,
    kDiagnostics,
    kAbout,
    kFactoryReset,
    kCount
};
constexpr std::size_t kMenuItemCount = static_cast<std::size_t>(MenuItem::kCount);

/// Items that can only fail on a build without a radio are hidden.
constexpr bool needs_radio(MenuItem item) noexcept {
    return item == MenuItem::kConnectivity || item == MenuItem::kWeather ||
           item == MenuItem::kSyncNow;
}

bool is_visible(MenuItem item, const WatchState& state) noexcept {
    if (item == MenuItem::kPhone) {
        return state.phone_available; // the phone link is compiled separately from Wi-Fi
    }
    return state.radio_available || !needs_radio(item);
}

std::size_t visible_count(const WatchState& state) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kMenuItemCount; ++i) {
        if (is_visible(static_cast<MenuItem>(i), state)) {
            ++n;
        }
    }
    return n;
}

/// The `index`-th visible item (index < visible_count).
MenuItem visible_item(const WatchState& state, std::size_t index) noexcept {
    for (std::size_t i = 0; i < kMenuItemCount; ++i) {
        const auto item = static_cast<MenuItem>(i);
        if (!is_visible(item, state)) {
            continue;
        }
        if (index == 0) {
            return item;
        }
        --index;
    }
    return MenuItem::kTimeDate;
}

std::string_view label_of(MenuItem item) noexcept {
    switch (item) {
        case MenuItem::kTimeDate:
            return "Time & date";
        case MenuItem::kTimeZone:
            return "Time zone";
        case MenuItem::kHourFormat:
            return "12/24 hour";
        case MenuItem::kUnits:
            return "Units";
        case MenuItem::kConnectivity:
            return "Connectivity";
        case MenuItem::kWeather:
            return "Weather";
        case MenuItem::kSyncNow:
            return "Sync now";
        case MenuItem::kPhone:
            return "Phone";
        case MenuItem::kStepGoal:
            return "Step goal";
        case MenuItem::kVibration:
            return "Vibration";
        case MenuItem::kFace:
            return "Watch face";
        case MenuItem::kDiagnostics:
            return "Diagnostics";
        case MenuItem::kAbout:
            return "About";
        case MenuItem::kFactoryReset:
            return "Factory reset";
        case MenuItem::kCount:
            break;
    }
    return {};
}

std::string_view on_off(bool on) noexcept {
    return on ? "on" : "off";
}

std::string_view conn_token(model::ConnectivityMode mode) noexcept {
    switch (mode) {
        case model::ConnectivityMode::kOff:
            return "off";
        case model::ConnectivityMode::kTimeOnly:
            return "time";
        case model::ConnectivityMode::kTimeWeather:
            break;
    }
    return "wx";
}

model::ConnectivityMode conn_of(const WatchState& s) noexcept {
    return s.settings != nullptr ? s.settings->connectivity : s.conn_mode;
}

void fill_menu_value(MenuItem item,
                     const WatchState& s,
                     const FaceSource& faces,
                     ListRow& row) noexcept {
    switch (item) {
        case MenuItem::kTimeZone:
            row.value.put(s.tz_label);
            break;
        case MenuItem::kHourFormat:
            row.value.put(s.hour_format == model::HourFormat::k12h ? "12h" : "24h");
            break;
        case MenuItem::kUnits:
            row.value.put(s.temp_unit == model::TempUnit::kFahrenheit ? "F" : "C");
            break;
        case MenuItem::kConnectivity:
            row.value.put(conn_token(conn_of(s)));
            break;
        case MenuItem::kStepGoal:
            if (s.steps.goal == 0) {
                row.value.put("off");
            } else {
                row.value.put_uint(s.steps.goal);
            }
            break;
        case MenuItem::kVibration:
            if (s.settings != nullptr) {
                row.value.put(on_off(s.settings->vibration));
            }
            break;
        case MenuItem::kPhone:
            if (s.settings != nullptr) {
                row.value.put(on_off(s.settings->phone_sync));
            }
            break;
        case MenuItem::kFace:
            if (s.settings != nullptr) {
                row.value.put(faces.name_of(s.settings->face_id));
            }
            break;
        case MenuItem::kTimeDate:
        case MenuItem::kWeather:
        case MenuItem::kSyncNow:
        case MenuItem::kDiagnostics:
        case MenuItem::kAbout:
        case MenuItem::kFactoryReset:
        case MenuItem::kCount:
            break;
    }
}

Outcome enter_item(MenuItem item) noexcept {
    switch (item) {
        case MenuItem::kTimeDate:
            return Outcome::push(ScreenId::kTimeDateEditor);
        case MenuItem::kTimeZone:
            return Outcome::push(ScreenId::kTimezonePicker);
        case MenuItem::kHourFormat:
            return Outcome::push(ScreenId::kChoice,
                                 static_cast<std::uint8_t>(ChoiceKind::kHourFormat));
        case MenuItem::kUnits:
            return Outcome::push(ScreenId::kChoice, static_cast<std::uint8_t>(ChoiceKind::kUnits));
        case MenuItem::kConnectivity:
            return Outcome::push(ScreenId::kChoice,
                                 static_cast<std::uint8_t>(ChoiceKind::kConnectivity));
        case MenuItem::kWeather:
            return Outcome::push(ScreenId::kWeatherSettings);
        case MenuItem::kSyncNow: {
            Outcome o = Outcome::push(ScreenId::kSyncNow);
            o.with(make_action(ActionKind::kSyncNow));
            return o;
        }
        case MenuItem::kPhone:
            return Outcome::push(ScreenId::kPhone);
        case MenuItem::kStepGoal:
            return Outcome::push(ScreenId::kStepGoalEditor);
        case MenuItem::kVibration:
            return Outcome::push(ScreenId::kChoice,
                                 static_cast<std::uint8_t>(ChoiceKind::kVibration));
        case MenuItem::kFace:
            return Outcome::push(ScreenId::kChoice, static_cast<std::uint8_t>(ChoiceKind::kFace));
        case MenuItem::kDiagnostics:
            return Outcome::push(ScreenId::kDiagnostics);
        case MenuItem::kAbout:
            return Outcome::push(ScreenId::kAbout);
        case MenuItem::kFactoryReset:
            return Outcome::push(ScreenId::kFactoryReset);
        case MenuItem::kCount:
            break;
    }
    return Outcome::none();
}

// ---- Phone rows ----------------------------------------------------------------------------
enum class PhoneRow : std::uint8_t { kSync, kEnabled, kForget, kCount };

bool phone_enabled(const WatchState& s) noexcept {
    return s.settings != nullptr && s.settings->phone_sync;
}

/// Off: only the switch is offered. On: sync first (the usual reason to come here).
std::size_t phone_row_count(const WatchState& s) noexcept {
    return phone_enabled(s) ? static_cast<std::size_t>(PhoneRow::kCount) : 1U;
}

PhoneRow phone_row_at(const WatchState& s, std::size_t index) noexcept {
    if (!phone_enabled(s)) {
        return PhoneRow::kEnabled;
    }
    return index < static_cast<std::size_t>(PhoneRow::kCount) ? static_cast<PhoneRow>(index)
                                                              : PhoneRow::kCount;
}

std::string_view phone_label(PhoneRow row) noexcept {
    switch (row) {
        case PhoneRow::kSync:
            return "Sync with phone";
        case PhoneRow::kEnabled:
            return "Phone sync";
        case PhoneRow::kForget:
            return "Forget phones";
        case PhoneRow::kCount:
            break;
    }
    return {};
}

// ---- WeatherSettings rows ------------------------------------------------------------------
enum class WxRow : std::uint8_t { kEnabled, kHighLow, kInterval, kLocation, kWifi, kCount };
constexpr std::size_t kWxRowCount = static_cast<std::size_t>(WxRow::kCount);

WxRow wx_row_at(std::size_t index) noexcept {
    static constexpr std::array<WxRow, kWxRowCount> kRows{
        WxRow::kEnabled, WxRow::kHighLow, WxRow::kInterval, WxRow::kLocation, WxRow::kWifi};
    return index < kRows.size() ? kRows[index] : WxRow::kCount;
}

std::size_t wx_visible_count(const WatchState& s) noexcept {
    return s.radio_available ? kWxRowCount : kWxRowCount - 1; // Wi-Fi setup needs the radio
}

bool wx_enabled(const WatchState& s) noexcept {
    return conn_of(s) == model::ConnectivityMode::kTimeWeather;
}
bool wx_high_low(const WatchState& s) noexcept {
    return s.settings != nullptr ? s.settings->weather_high_low : s.weather_high_low;
}

std::string_view wx_label(WxRow row) noexcept {
    switch (row) {
        case WxRow::kEnabled:
            return "Weather";
        case WxRow::kHighLow:
            return "High / low";
        case WxRow::kInterval:
            return "Update every";
        case WxRow::kLocation:
            return "Location";
        case WxRow::kWifi:
            return "Setup Wi-Fi";
        case WxRow::kCount:
            break;
    }
    return {};
}

void fill_wx_value(WxRow row, const WatchState& s, ListRow& out) noexcept {
    switch (row) {
        case WxRow::kEnabled:
            out.value.put(on_off(wx_enabled(s)));
            break;
        case WxRow::kHighLow:
            out.value.put(on_off(wx_high_low(s)));
            break;
        case WxRow::kInterval:
            if (s.settings != nullptr) {
                out.value.put_uint(s.settings->weather_interval_min).put(" min");
            }
            break;
        case WxRow::kLocation:
            if (s.settings != nullptr) {
                out.value.put(s.settings->location_set ? "set" : "unset");
            }
            break;
        case WxRow::kWifi:
            out.value.put(s.has_credentials ? "saved" : "");
            break;
        case WxRow::kCount:
            break;
    }
}

} // namespace

// ---- MenuScreen ----------------------------------------------------------------------------
void MenuScreen::enter(const WatchState& /*state*/, std::uint8_t /*param*/) noexcept {
    cursor_ = 0;
}

void MenuScreen::render(const WatchState& state, gfx::Canvas& canvas) const noexcept {
    draw_list(canvas, "Menu", visible_count(state), cursor_, [&](std::size_t i, ListRow& row) {
        const MenuItem item = visible_item(state, i);
        row.label.put(label_of(item));
        fill_menu_value(item, state, faces_, row);
    });
    draw_hint(canvas, "MENU enter  BACK exit");
}

Outcome MenuScreen::handle(const model::InputEvent& event, const WatchState& state) noexcept {
    const std::size_t n = visible_count(state);
    cursor_ = cursor_ < n ? cursor_ : n - 1; // the item set can shrink (radio flag)
    if (const int dir = list_direction(event); dir != 0) {
        cursor_ = wrap_index(cursor_, dir, n);
        return Outcome::none();
    }
    if (is_click(event, Button::kBack)) {
        return Outcome::pop();
    }
    if (is_click(event, Button::kMenu)) {
        return enter_item(visible_item(state, cursor_));
    }
    return Outcome::none();
}

// ---- PhoneScreen ---------------------------------------------------------------------------
void PhoneScreen::enter(const WatchState& /*state*/, std::uint8_t /*param*/) noexcept {
    cursor_ = 0;
}

void PhoneScreen::render(const WatchState& state, gfx::Canvas& canvas) const noexcept {
    draw_list(canvas, "Phone", phone_row_count(state), cursor_, [&](std::size_t i, ListRow& row) {
        const PhoneRow r = phone_row_at(state, i);
        row.label.put(phone_label(r));
        if (r == PhoneRow::kEnabled) {
            row.value.put(on_off(phone_enabled(state)));
        }
    });
    draw_hint(canvas, "MENU select  BACK back");
}

Outcome PhoneScreen::handle(const model::InputEvent& event, const WatchState& state) noexcept {
    const std::size_t n = phone_row_count(state);
    cursor_ = cursor_ < n ? cursor_ : n - 1; // turning the feature off shrinks the list
    if (const int dir = list_direction(event); dir != 0) {
        cursor_ = wrap_index(cursor_, dir, n);
        return Outcome::none();
    }
    if (is_click(event, Button::kBack)) {
        return Outcome::pop();
    }
    if (!is_click(event, Button::kMenu)) {
        return Outcome::none();
    }
    Outcome o;
    switch (phone_row_at(state, cursor_)) {
        case PhoneRow::kSync:
            o = Outcome::push(ScreenId::kPhoneSync);
            o.with(make_action(ActionKind::kStartPhoneSync));
            break;
        case PhoneRow::kEnabled:
            o.with(make_set_setting(Key::kPhoneSync, on_off(!phone_enabled(state))));
            break;
        case PhoneRow::kForget:
            o.with(make_action(ActionKind::kForgetPhones));
            break;
        case PhoneRow::kCount:
            break;
    }
    return o;
}

// ---- WeatherSettingsScreen -----------------------------------------------------------------
void WeatherSettingsScreen::enter(const WatchState& /*state*/, std::uint8_t /*param*/) noexcept {
    cursor_ = 0;
}

void WeatherSettingsScreen::render(const WatchState& state, gfx::Canvas& canvas) const noexcept {
    draw_list(
        canvas, "Weather", wx_visible_count(state), cursor_, [&](std::size_t i, ListRow& row) {
            const WxRow wx = wx_row_at(i);
            row.label.put(wx_label(wx));
            fill_wx_value(wx, state, row);
        });
    draw_hint(canvas, "MENU change  BACK back");
}

Outcome WeatherSettingsScreen::handle(const model::InputEvent& event,
                                      const WatchState& state) noexcept {
    const std::size_t n = wx_visible_count(state);
    cursor_ = cursor_ < n ? cursor_ : n - 1;
    if (const int dir = list_direction(event); dir != 0) {
        cursor_ = wrap_index(cursor_, dir, n);
        return Outcome::none();
    }
    if (is_click(event, Button::kBack)) {
        return Outcome::pop();
    }
    if (!is_click(event, Button::kMenu)) {
        return Outcome::none();
    }
    Outcome o;
    switch (wx_row_at(cursor_)) {
        case WxRow::kEnabled:
            o.with(
                make_set_setting(Key::kConnectivity, wx_enabled(state) ? "time" : "time+weather"));
            break;
        case WxRow::kHighLow:
            o.with(make_set_setting(Key::kWeatherHighLow, on_off(!wx_high_low(state))));
            break;
        case WxRow::kInterval:
            o = Outcome::push(ScreenId::kChoice,
                              static_cast<std::uint8_t>(ChoiceKind::kWeatherInterval));
            break;
        case WxRow::kLocation:
            o = Outcome::push(ScreenId::kLocationEditor);
            break;
        case WxRow::kWifi:
            o = Outcome::push(ScreenId::kProvisioning);
            o.with(make_action(ActionKind::kStartProvisioning));
            break;
        case WxRow::kCount:
            break;
    }
    return o;
}

} // namespace qz::ui
