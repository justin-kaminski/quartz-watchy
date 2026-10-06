// Editors and pickers (WP-14): exactly one Action per save (two for the location pair), none on
// cancel; value rules, clamping, accelerated holds; every emitted value passes settings validation.
#include "qz/time/tz.hpp"
#include "ui_test_support.hpp"

#include <gtest/gtest.h>

namespace qz::ui {
namespace {

using model::Button;
using settings::Key;
using test::Harness;

// gtest macros inflate the cognitive-complexity score of table-driven checks.
// NOLINTBEGIN(readability-function-cognitive-complexity)

/// Applies a SetSetting action to `s` the way the app does (one validation path).
bool apply(settings::Settings& s, const Action& a) {
    return a.kind == ActionKind::kSetSetting && settings::set_from_string(s, a.key, a.value.view());
}

ActionList save_time_editor(Harness& h) {
    ActionList last;
    for (int i = 0; i < 5; ++i) {
        last = h.click(Button::kMenu);
    }
    return last;
}

// ---- TimeDateEditor ------------------------------------------------------------------------
TEST(TimeDateEditor, SaveWithoutEditsEmitsTheCurrentLocalTimeWithSecondsZeroed) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    const auto actions = save_time_editor(h);
    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions[0].kind, ActionKind::kSetTime);
    EXPECT_EQ(actions[0].date, (time::CivilDate{2026, 10, 6}));
    EXPECT_EQ(actions[0].time, (time::CivilTime{14, 32, 0}));
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(TimeDateEditor, InvalidClockStartsFromTheDefaultDate) {
    Harness h;
    h.state.time_valid = false;
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    const auto actions = save_time_editor(h);
    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions[0].date, (time::CivilDate{2026, 1, 1}));
    EXPECT_EQ(actions[0].time, (time::CivilTime{12, 0, 0}));
}

TEST(TimeDateEditor, UpIncrementsDownDecrementsEachField) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    (void)h.click(Button::kUp); // year 2027
    (void)h.click(Button::kMenu);
    (void)h.clicks(Button::kDown, 2); // month 8
    (void)h.click(Button::kMenu);
    (void)h.clicks(Button::kUp, 3); // day 9
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kDown); // hour 13
    (void)h.click(Button::kMenu);
    (void)h.clicks(Button::kUp, 5); // minute 37
    const auto actions = h.click(Button::kMenu);
    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions[0].date, (time::CivilDate{2027, 8, 9}));
    EXPECT_EQ(actions[0].time, (time::CivilTime{13, 37, 0}));
}

TEST(TimeDateEditor, FieldsWrapAndYearClamps) {
    Harness h;
    h.state.local.date = {2099, 12, 31};
    h.state.local.time = {0, 59, 0};
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    (void)h.click(Button::kUp); // year stays at the maximum
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp); // month 12 -> 1 (day clamped only if needed: 31 is valid)
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp); // day 31 -> 1
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kDown); // hour 0 -> 23
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp); // minute 59 -> 0
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].date, (time::CivilDate{2099, 1, 1}));
    EXPECT_EQ(a[0].time, (time::CivilTime{23, 0, 0}));

    Harness low;
    low.state.local.date = {2025, 1, 1};
    ASSERT_TRUE(low.ui.show(ScreenId::kTimeDateEditor, low.state));
    (void)low.click(Button::kDown); // year stays at the minimum
    const auto b = save_time_editor(low);
    EXPECT_EQ(b[0].date.year, 2025);
}

TEST(TimeDateEditor, DayIsClampedWhenMonthOrYearShrinksIt) {
    Harness h;
    h.state.local.date = {2026, 3, 31};
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kDown); // March -> February
    (void)h.clicks(Button::kMenu, 3);
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].date, (time::CivilDate{2026, 2, 28}));

    Harness leap;
    leap.state.local.date = {2028, 2, 29};
    ASSERT_TRUE(leap.ui.show(ScreenId::kTimeDateEditor, leap.state));
    (void)leap.click(Button::kUp); // 2029 is not a leap year
    const auto b = save_time_editor(leap);
    EXPECT_EQ(b[0].date, (time::CivilDate{2029, 2, 28}));
    EXPECT_TRUE(time::is_valid(b[0].date, b[0].time));
}

TEST(TimeDateEditor, HeldKeysKeepStepping) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    (void)h.clicks(Button::kMenu, 4); // minute field
    (void)h.send(test::hold(Button::kUp));
    (void)h.send(test::repeat(Button::kUp, 850));
    (void)h.send(test::repeat(Button::kUp, 1000));
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].time.minute, 35); // 32 + 3 steps
}

TEST(TimeDateEditor, BackStepsToPreviousFieldAndCancelEmitsNothing) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    (void)h.click(Button::kUp); // an edit that must be discarded
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp);
    EXPECT_TRUE(h.click(Button::kBack).empty()); // back to the year field
    EXPECT_EQ(h.ui.current(), ScreenId::kTimeDateEditor);
    EXPECT_TRUE(h.click(Button::kBack).empty()); // cancel
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    // Reopening starts from the snapshot again.
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    EXPECT_EQ(save_time_editor(h)[0].date.year, 2026);
}

TEST(TimeDateEditor, RenderHighlightMovesWithTheField) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kTimeDateEditor, h.state));
    std::vector<std::uint32_t> crcs;
    for (int i = 0; i < 5; ++i) {
        crcs.push_back(h.draw().crc32());
        (void)h.click(Button::kMenu);
    }
    for (std::size_t i = 0; i < crcs.size(); ++i) {
        for (std::size_t j = i + 1; j < crcs.size(); ++j) {
            EXPECT_NE(crcs[i], crcs[j]) << i << " vs " << j;
        }
    }
}

// ---- StepGoalEditor ------------------------------------------------------------------------
TEST(StepGoalEditor, StartsAtTheSavedGoalAndStepsBy500) {
    Harness h;
    h.settings.step_goal = 7500;
    ASSERT_TRUE(h.ui.show(ScreenId::kStepGoalEditor, h.state));
    (void)h.click(Button::kUp);
    (void)h.clicks(Button::kDown, 3);
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kSetSetting);
    EXPECT_EQ(a[0].key, Key::kStepGoal);
    EXPECT_EQ(a[0].value.view(), "6500");
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    settings::Settings s = h.settings;
    EXPECT_TRUE(apply(s, a[0]));
    EXPECT_EQ(s.step_goal, 6500U);
}

TEST(StepGoalEditor, ClampsAtZeroAndFiftyThousand) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kStepGoalEditor, h.state));
    (void)h.clicks(Button::kDown, 3);
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "0");
    h.settings.step_goal = 49'500;
    ASSERT_TRUE(h.ui.show(ScreenId::kStepGoalEditor, h.state));
    (void)h.clicks(Button::kUp, 5);
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "50000");
}

TEST(StepGoalEditor, LongHoldsStepFaster) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kStepGoalEditor, h.state));
    (void)h.send(test::hold(Button::kUp));         // +500
    (void)h.send(test::repeat(Button::kUp, 1850)); // +500
    (void)h.send(test::repeat(Button::kUp, 2000)); // +2500
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "3500");
}

TEST(StepGoalEditor, CancelEmitsNothingAndFallsBackToTheSnapshotGoal) {
    Harness h;
    h.state.settings = nullptr;
    h.state.steps.goal = 10'000;
    ASSERT_TRUE(h.ui.show(ScreenId::kStepGoalEditor, h.state));
    (void)h.click(Button::kUp);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    ASSERT_TRUE(h.ui.show(ScreenId::kStepGoalEditor, h.state));
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "10000");
}

// ---- LocationEditor ------------------------------------------------------------------------
ActionList save_location_from(Harness& h, int menu_clicks_before_save = 10) {
    (void)h.clicks(Button::kMenu, menu_clicks_before_save);
    return h.click(Button::kMenu);
}

TEST(LocationEditor, SaveEmitsLatThenLonAndTheValuesValidate) {
    Harness h;
    h.settings.location = {4'760'000, -12'233'000};
    h.settings.location_set = true;
    ASSERT_TRUE(h.ui.show(ScreenId::kLocationEditor, h.state));
    const auto a = save_location_from(h);
    ASSERT_EQ(a.size(), 2U);
    EXPECT_EQ(a[0].key, Key::kLatitude);
    EXPECT_EQ(a[0].value.view(), "47.60");
    EXPECT_EQ(a[1].key, Key::kLongitude);
    EXPECT_EQ(a[1].value.view(), "-122.33");
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);
    settings::Settings s = settings::defaults();
    EXPECT_TRUE(apply(s, a[0]));
    EXPECT_TRUE(apply(s, a[1]));
    EXPECT_EQ(s.location, (model::Location{4'760'000, -12'233'000}));
    EXPECT_TRUE(s.location_set);
}

TEST(LocationEditor, DigitsWrapSignsToggleAndMagnitudesClamp) {
    Harness h;
    h.settings.location = {4'760'000, 1'200'000};
    ASSERT_TRUE(h.ui.show(ScreenId::kLocationEditor, h.state));
    (void)h.click(Button::kUp); // pos 0: latitude sign -> "-"
    (void)h.clicks(Button::kMenu, 4);
    (void)h.click(Button::kDown); // pos 4: hundredths 0 -> 9
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kMenu); // pos 6: longitude hundreds digit (0 of 012.00)
    (void)h.click(Button::kUp);   // 112.00
    const auto a = save_location_from(h, 4);
    ASSERT_EQ(a.size(), 2U);
    EXPECT_EQ(a[0].value.view(), "-47.69");
    EXPECT_EQ(a[1].value.view(), "112.00");
}

TEST(LocationEditor, OutOfRangeDigitsClampToTheLimits) {
    Harness h;
    h.settings.location = {8'900'000, 17'900'000};
    ASSERT_TRUE(h.ui.show(ScreenId::kLocationEditor, h.state));
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp);       // 89 -> 99 would exceed 90.00
    (void)h.clicks(Button::kMenu, 5); // pos 6
    (void)h.click(Button::kUp);       // 179 -> 279 would exceed 180.00
    const auto a = save_location_from(h, 4);
    ASSERT_EQ(a.size(), 2U);
    EXPECT_EQ(a[0].value.view(), "90.00");
    EXPECT_EQ(a[1].value.view(), "180.00");
    settings::Settings s = settings::defaults();
    EXPECT_TRUE(apply(s, a[0]));
    EXPECT_TRUE(apply(s, a[1]));
}

TEST(LocationEditor, NegativeZeroIsSavedAsZero) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kLocationEditor, h.state));
    (void)h.click(Button::kUp);       // lat "-00.00"
    (void)h.clicks(Button::kMenu, 5); // lon sign
    (void)h.click(Button::kUp);       // lon "-000.00"
    const auto a = save_location_from(h, 5);
    ASSERT_EQ(a.size(), 2U);
    EXPECT_EQ(a[0].value.view(), "0.00");
    EXPECT_EQ(a[1].value.view(), "0.00");
}

TEST(LocationEditor, BackMovesToThePreviousPositionAndCancelEmitsNothing) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kLocationEditor, h.state));
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kLocationEditor);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);
}

TEST(LocationEditor, HighlightMovesOverAllElevenPositions) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kLocationEditor, h.state));
    std::vector<std::uint32_t> crcs;
    for (int i = 0; i < 11; ++i) {
        crcs.push_back(h.draw().crc32());
        (void)h.click(Button::kMenu);
    }
    for (std::size_t i = 0; i < crcs.size(); ++i) {
        for (std::size_t j = i + 1; j < crcs.size(); ++j) {
            EXPECT_NE(crcs[i], crcs[j]) << i << " vs " << j;
        }
    }
}

// ---- Choice lists --------------------------------------------------------------------------
TEST(Choice, PreselectsTheCurrentValueAndSavesTheChosenOne) {
    Harness h;
    h.settings.hour_format = model::HourFormat::k12h;
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kHourFormat, h.state));
    auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].value.view(), "12h");
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kHourFormat, h.state));
    (void)h.click(Button::kUp); // 12h -> 24h (list cursor moves up)
    a = h.click(Button::kMenu);
    EXPECT_EQ(a[0].value.view(), "24h");
}

TEST(Choice, DefaultsPerKind) {
    struct Case {
        ChoiceKind kind;
        Key key;
        std::string_view value;
    };
    constexpr std::array<Case, 7> cases{
        {{ChoiceKind::kHourFormat, Key::kHourFormat, "24h"},
         {ChoiceKind::kUnits, Key::kTempUnit, "c"},
         {ChoiceKind::kConnectivity, Key::kConnectivity, "off"},
         {ChoiceKind::kVibration, Key::kVibration, "on"},
         {ChoiceKind::kFace, Key::kFace, "0"},
         {ChoiceKind::kSyncInterval, Key::kSyncIntervalH, "24"},
         {ChoiceKind::kWeatherInterval, Key::kWeatherIntervalMin, "60"}}};
    for (const Case& c : cases) {
        Harness h;
        ASSERT_TRUE(h.ui.show_choice(c.kind, h.state));
        const auto a = h.click(Button::kMenu);
        ASSERT_EQ(a.size(), 1U);
        EXPECT_EQ(a[0].key, c.key);
        EXPECT_EQ(a[0].value.view(), c.value);
    }
}

TEST(Choice, EveryItemOfEveryKindPassesSettingsValidation) {
    for (std::size_t k = 0; k < static_cast<std::size_t>(ChoiceKind::kCount); ++k) {
        const auto kind = static_cast<ChoiceKind>(k);
        for (int item = 0; item < 5; ++item) {
            Harness h;
            ASSERT_TRUE(h.ui.show_choice(kind, h.state));
            (void)h.clicks(Button::kDown, item);
            const auto a = h.click(Button::kMenu);
            ASSERT_EQ(a.size(), 1U);
            settings::Settings s = h.settings;
            EXPECT_TRUE(apply(s, a[0])) << k << "/" << item << " " << a[0].value.view();
        }
    }
}

TEST(Choice, FaceListComesFromTheFaceSourceAndSavesTheStableId) {
    Harness h;
    h.settings.face_id = 3;
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kFace, h.state));
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "3"); // current face pre-selected
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kFace, h.state));
    (void)h.click(Button::kUp);
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "0");
}

TEST(Choice, WrapsAndHeldKeysMoveAndCancelEmitsNothing) {
    Harness h;
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kConnectivity, h.state));
    (void)h.click(Button::kUp); // off -> wraps to the last item
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "time+weather");
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kConnectivity, h.state));
    (void)h.send(test::hold(Button::kDown));
    (void)h.send(test::repeat(Button::kDown, 850));
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "time+weather"); // 0 -> 1 -> 2
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kConnectivity, h.state));
    (void)h.click(Button::kDown);
    EXPECT_TRUE(h.click(Button::kBack).empty());
}

TEST(Choice, WithoutSettingsSnapshotStartsAtTheFirstItem) {
    Harness h;
    h.state.settings = nullptr;
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kUnits, h.state));
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), "c");
}

// ---- Time-zone picker ----------------------------------------------------------------------
TEST(ZonePicker, PreselectsTheCurrentZoneAndSavesItsName) {
    Harness h;
    h.settings.tz_name = "Europe/Berlin";
    ASSERT_TRUE(h.ui.show(ScreenId::kTimezonePicker, h.state));
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].key, Key::kTimeZone);
    EXPECT_EQ(a[0].value.view(), "Europe/Berlin");
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(ZonePicker, EveryBuiltInZoneCanBeSelectedAndPassesValidation) {
    const auto zones = time::builtin_zones();
    ASSERT_GT(zones.size(), 8U);
    Harness h;
    std::size_t start = 0;
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (zones[i].name == "UTC") {
            start = i;
        }
    }
    for (std::size_t i = 0; i < zones.size(); ++i) {
        ASSERT_TRUE(h.ui.show(ScreenId::kTimezonePicker, h.state));
        (void)h.clicks(Button::kDown, static_cast<int>((i + zones.size() - start) % zones.size()));
        const auto a = h.click(Button::kMenu);
        ASSERT_EQ(a.size(), 1U);
        EXPECT_EQ(a[0].value.view(), zones[i].name);
        settings::Settings s = h.settings;
        EXPECT_TRUE(apply(s, a[0])) << zones[i].name;
        EXPECT_EQ(s.tz_name.view(), zones[i].name);
    }
}

TEST(ZonePicker, UpWrapsLongHoldsAccelerateCancelEmitsNothing) {
    const auto zones = time::builtin_zones();
    Harness h;
    ASSERT_TRUE(h.settings.tz_name.assign(zones[0].name));
    ASSERT_TRUE(h.ui.show(ScreenId::kTimezonePicker, h.state));
    (void)h.click(Button::kUp);
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), zones[zones.size() - 1].name);
    ASSERT_TRUE(h.ui.show(ScreenId::kTimezonePicker, h.state));
    (void)h.send(test::repeat(Button::kDown, 2'500)); // 5 rows at once
    EXPECT_EQ(h.click(Button::kMenu)[0].value.view(), zones[5 % zones.size()].name);
    ASSERT_TRUE(h.ui.show(ScreenId::kTimezonePicker, h.state));
    (void)h.click(Button::kDown);
    EXPECT_TRUE(h.click(Button::kBack).empty());
}

// ---- Menu / WeatherSettings values ---------------------------------------------------------
TEST(MenuValues, CursorSurvivesReturningFromASubScreen) {
    Harness h;
    h.goto_menu_row(3);
    (void)h.click(Button::kMenu); // Units choice
    (void)h.click(Button::kBack);
    (void)h.click(Button::kMenu); // the cursor is still on Units
    EXPECT_EQ(h.ui.current(), ScreenId::kChoice);
    EXPECT_EQ(h.click(Button::kMenu)[0].key, Key::kTempUnit);
}

TEST(MenuValues, MenuOpensAtTheTopEachTimeFromTheFace) {
    Harness h;
    h.goto_menu_row(5);
    (void)h.click(Button::kBack);
    (void)h.click(Button::kBack);
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.ui.current(), ScreenId::kTimeDateEditor);
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::ui
