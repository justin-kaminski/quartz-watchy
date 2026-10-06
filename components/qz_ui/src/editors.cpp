#include "editors.hpp"

#include <algorithm>

namespace qz::ui {
namespace {

using model::Button;
using settings::Key;

constexpr std::int16_t kBigBaseline = 80;
constexpr std::int16_t kBigBaseline2 = 128;
constexpr std::int16_t kCaptionBaseline = 54;
constexpr std::int16_t kCaptionBaseline2 = 102;
constexpr std::int16_t kGoalBaseline = 96;
constexpr std::int16_t kGoalCaptionBaseline = 120;

std::uint32_t clamp_u32(std::int64_t v, std::uint32_t lo, std::uint32_t hi) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, lo, hi));
}

/// MENU/BACK labels shared by the multi-field editors.
std::string_view field_hint(std::uint8_t pos, std::uint8_t count) noexcept {
    if (pos == 0) {
        return pos + 1 == count ? "MENU save  BACK cancel" : "MENU next  BACK cancel";
    }
    return pos + 1 == count ? "MENU save  BACK previous" : "MENU next  BACK previous";
}

} // namespace

// ---- TimeDateEditor ------------------------------------------------------------------------
void TimeDateEditor::enter(const WatchState& state, std::uint8_t /*param*/) noexcept {
    field_ = 0;
    if (state.time_valid) {
        date_ = state.local.date;
        time_ = state.local.time;
        time_.second = 0;
    } else {
        date_ = tuning::kDefaultDate;
        time_ = tuning::kDefaultTime;
    }
    date_.year = std::clamp(date_.year, tuning::kYearMin, tuning::kYearMax);
    date_.day = std::min(date_.day, time::days_in_month(date_.year, date_.month));
}

void TimeDateEditor::step(int delta) noexcept {
    const auto wrap = [delta](std::uint8_t v, int lo, int hi) {
        const int span = hi - lo + 1;
        return static_cast<std::uint8_t>(lo + (((((v - lo) + delta) % span) + span) % span));
    };
    switch (field_) {
        case 0:
            date_.year = std::clamp(date_.year + delta, tuning::kYearMin, tuning::kYearMax);
            break;
        case 1:
            date_.month = wrap(date_.month, 1, 12);
            break;
        case 2:
            date_.day = wrap(date_.day, 1, time::days_in_month(date_.year, date_.month));
            break;
        case 3:
            time_.hour = wrap(time_.hour, 0, 23);
            break;
        default:
            time_.minute = wrap(time_.minute, 0, 59);
            break;
    }
    date_.day = std::min(date_.day, time::days_in_month(date_.year, date_.month));
}

void TimeDateEditor::render(const WatchState& /*state*/, gfx::Canvas& canvas) const noexcept {
    draw_title_bar(canvas, "Time & date");
    const gfx::Font& big = gfx::font(gfx::FontId::kLarge);
    TextBuilder<12> d;
    d.put_uint(static_cast<std::uint32_t>(date_.year), 4)
        .put_char('-')
        .put_uint(date_.month, 2)
        .put_char('-')
        .put_uint(date_.day, 2);
    TextBuilder<8> t;
    t.put_uint(time_.hour, 2).put_char(':').put_uint(time_.minute, 2);
    static constexpr std::array<std::size_t, 3> kDateBegin{0, 5, 8};
    static constexpr std::array<std::size_t, 3> kDateLen{4, 2, 2};
    static constexpr std::array<std::size_t, 2> kTimeBegin{0, 3};
    const bool in_date = field_ < 3;
    draw_caption(canvas, kCaptionBaseline, "date");
    draw_marked_text(canvas,
                     kBigBaseline,
                     big,
                     d.view(),
                     in_date ? kDateBegin[field_] : 0,
                     in_date ? kDateLen[field_] : 0);
    draw_caption(canvas, kCaptionBaseline2, "time (24h)");
    draw_marked_text(canvas,
                     kBigBaseline2,
                     big,
                     t.view(),
                     in_date ? 0 : kTimeBegin[field_ - 3U],
                     in_date ? 0 : 2);
    draw_hint(canvas, field_hint(field_, kFieldCount));
}

Outcome TimeDateEditor::handle(const model::InputEvent& event,
                               const WatchState& /*state*/) noexcept {
    if (const int dir = step_direction(event); dir != 0) {
        step(dir);
        return Outcome::none();
    }
    if (is_click(event, Button::kBack)) {
        if (field_ == 0) {
            return Outcome::pop(); // cancel: nothing emitted
        }
        --field_;
        return Outcome::none();
    }
    if (is_click(event, Button::kMenu)) {
        if (field_ + 1 < kFieldCount) {
            ++field_;
            return Outcome::none();
        }
        Action a = make_action(ActionKind::kSetTime);
        a.date = date_;
        a.time = {time_.hour, time_.minute, 0};
        Outcome o = Outcome::pop();
        o.with(a);
        return o;
    }
    return Outcome::none();
}

// ---- StepGoalEditor ------------------------------------------------------------------------
void StepGoalEditor::enter(const WatchState& state, std::uint8_t /*param*/) noexcept {
    const std::uint32_t current =
        state.settings != nullptr ? state.settings->step_goal : state.steps.goal;
    goal_ = std::min(current, tuning::kStepGoalMax) / tuning::kStepGoalStep * tuning::kStepGoalStep;
}

void StepGoalEditor::render(const WatchState& /*state*/, gfx::Canvas& canvas) const noexcept {
    draw_title_bar(canvas, "Step goal");
    TextBuilder<8> v;
    if (goal_ == 0) {
        v.put("Off");
    } else {
        v.put_uint(goal_);
    }
    const gfx::Font& big = gfx::font(gfx::FontId::kHuge);
    draw_marked_text(canvas, kGoalBaseline, big, v.view(), 0, v.view().size());
    draw_caption(canvas, kGoalCaptionBaseline, "steps per day");
    draw_hint(canvas, "MENU save  BACK cancel");
}

Outcome StepGoalEditor::handle(const model::InputEvent& event,
                               const WatchState& /*state*/) noexcept {
    if (const int dir = step_direction(event); dir != 0) {
        const std::int64_t delta =
            static_cast<std::int64_t>(dir) * tuning::kStepGoalStep * step_multiplier(event);
        goal_ = clamp_u32(static_cast<std::int64_t>(goal_) + delta, 0, tuning::kStepGoalMax);
        return Outcome::none();
    }
    if (is_click(event, Button::kBack)) {
        return Outcome::pop();
    }
    if (is_click(event, Button::kMenu)) {
        TextBuilder<8> v;
        v.put_uint(goal_);
        Outcome o = Outcome::pop();
        o.with(make_set_setting(Key::kStepGoal, v.view()));
        return o;
    }
    return Outcome::none();
}

// ---- LocationEditor ------------------------------------------------------------------------
namespace {

constexpr std::array<std::int32_t, 4> kLatPlaces{1000, 100, 10, 1};
constexpr std::array<std::int32_t, 5> kLonPlaces{10000, 1000, 100, 10, 1};
constexpr std::uint8_t kLonSignPos = 5;

/// "+DD.dd" / "+DDD.dd"; `for_setting` drops the plus sign, the leading zeros and "-0".
template<std::size_t N>
void put_coordinate(
    TextBuilder<N>& b, std::int32_t units, bool neg, unsigned int_digits, bool for_setting) {
    if (neg && (!for_setting || units != 0)) {
        b.put_char('-');
    } else if (!for_setting) {
        b.put_char('+');
    }
    b.put_uint(static_cast<std::uint32_t>(units / 100), for_setting ? 1U : int_digits)
        .put_char('.')
        .put_uint(static_cast<std::uint32_t>(units % 100), 2);
}

std::int32_t to_units(std::int32_t e5, std::int32_t max_units) noexcept {
    const std::int32_t mag = e5 < 0 ? -e5 : e5;
    return std::min((mag + (tuning::kLocUnitE5 / 2)) / tuning::kLocUnitE5, max_units);
}

/// Character index inside "+DD.dd" / "+DDD.dd" of editor position `k` (0 = sign, 1.. = digits).
std::size_t char_index(std::size_t k, std::size_t int_digits) noexcept {
    if (k == 0) {
        return 0;
    }
    return k <= int_digits ? k : k + 1; // skip the '.'
}

} // namespace

void LocationEditor::enter(const WatchState& state, std::uint8_t /*param*/) noexcept {
    pos_ = 0;
    model::Location loc{};
    if (state.settings != nullptr) {
        loc = state.settings->location;
    }
    lat_neg_ = loc.lat_e5 < 0;
    lon_neg_ = loc.lon_e5 < 0;
    lat_units_ = to_units(loc.lat_e5, tuning::kLatMaxUnits);
    lon_units_ = to_units(loc.lon_e5, tuning::kLonMaxUnits);
}

void LocationEditor::step(int delta) noexcept {
    if (pos_ == 0) {
        lat_neg_ = !lat_neg_;
        return;
    }
    if (pos_ == kLonSignPos) {
        lon_neg_ = !lon_neg_;
        return;
    }
    const bool is_lat = pos_ < kLonSignPos;
    const std::int32_t place = is_lat ? kLatPlaces[pos_ - 1U] : kLonPlaces[pos_ - kLonSignPos - 1U];
    std::int32_t& units = is_lat ? lat_units_ : lon_units_;
    const std::int32_t digit = (units / place) % 10;
    const std::int32_t next = (digit + delta + 10) % 10;
    units = std::min(units + ((next - digit) * place),
                     is_lat ? tuning::kLatMaxUnits : tuning::kLonMaxUnits);
}

void LocationEditor::render(const WatchState& /*state*/, gfx::Canvas& canvas) const noexcept {
    draw_title_bar(canvas, "Location");
    const gfx::Font& big = gfx::font(gfx::FontId::kLarge);
    TextBuilder<12> lat;
    put_coordinate(lat, lat_units_, lat_neg_, 2, false);
    TextBuilder<12> lon;
    put_coordinate(lon, lon_units_, lon_neg_, 3, false);
    const bool on_lat = pos_ < kLonSignPos;
    const std::size_t mark = on_lat ? char_index(pos_, 2) : char_index(pos_ - kLonSignPos, 3);
    draw_caption(canvas, kCaptionBaseline, "latitude");
    draw_marked_text(canvas, kBigBaseline, big, lat.view(), mark, on_lat ? 1 : 0);
    draw_caption(canvas, kCaptionBaseline2, "longitude");
    draw_marked_text(canvas, kBigBaseline2, big, lon.view(), mark, on_lat ? 0 : 1);
    draw_hint(canvas, field_hint(pos_, kPositionCount));
}

Outcome LocationEditor::handle(const model::InputEvent& event,
                               const WatchState& /*state*/) noexcept {
    if (const int dir = step_direction(event); dir != 0) {
        step(dir);
        return Outcome::none();
    }
    if (is_click(event, Button::kBack)) {
        if (pos_ == 0) {
            return Outcome::pop();
        }
        --pos_;
        return Outcome::none();
    }
    if (is_click(event, Button::kMenu)) {
        if (pos_ + 1 < kPositionCount) {
            ++pos_;
            return Outcome::none();
        }
        TextBuilder<12> lat;
        put_coordinate(lat, lat_units_, lat_neg_, 2, true);
        TextBuilder<12> lon;
        put_coordinate(lon, lon_units_, lon_neg_, 3, true);
        Outcome o = Outcome::pop();
        o.with(make_set_setting(Key::kLatitude, lat.view()));
        o.with(make_set_setting(Key::kLongitude, lon.view()));
        return o;
    }
    return Outcome::none();
}

} // namespace qz::ui
