// Navigation graph (ARCHITECTURE.md section 15, one test per row), global rules, idle timeout,
// refresh hints, show(), screen names, rendering smoke.
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

constexpr std::array<Button, 4> kAllButtons{
    Button::kMenu, Button::kBack, Button::kUp, Button::kDown};

void expect_no_effect(Harness& h, ScreenId screen, std::initializer_list<Button> ignored) {
    for (const Button b : ignored) {
        const auto actions = h.click(b);
        EXPECT_TRUE(actions.empty()) << screen_name(screen);
        EXPECT_EQ(h.ui.current(), screen) << screen_name(screen);
    }
}

TEST(NavFace, UpOpensStepsDownOpensWeatherMenuOpensMenuBackFullRefresh) {
    Harness h;
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
    EXPECT_TRUE(h.click(Button::kUp).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kStepsHistory);
    (void)h.click(Button::kBack);
    EXPECT_TRUE(h.click(Button::kDown).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherDetail);
    (void)h.click(Button::kBack);
    EXPECT_TRUE(h.click(Button::kMenu).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    (void)h.click(Button::kBack);
    ASSERT_EQ(h.ui.current(), ScreenId::kFace);
    const auto actions = h.click(Button::kBack);
    ASSERT_EQ(actions.size(), 1U);
    EXPECT_EQ(actions[0].kind, ActionKind::kFullRefresh);
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
}

TEST(NavFace, HoldAndRepeatDoNotNavigateOnTheFace) {
    Harness h;
    EXPECT_TRUE(h.send(test::hold(Button::kUp)).empty());
    EXPECT_TRUE(h.send(test::repeat(Button::kDown, 850)).empty());
    EXPECT_TRUE(h.send(test::hold(Button::kBack)).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
}

TEST(NavGlance, StepsHistoryAndWeatherDetailBackToFace) {
    for (const ScreenId id : {ScreenId::kStepsHistory, ScreenId::kWeatherDetail}) {
        Harness h;
        ASSERT_TRUE(h.ui.show(id, h.state));
        expect_no_effect(h, id, {Button::kUp, Button::kDown, Button::kMenu});
        EXPECT_TRUE(h.click(Button::kBack).empty());
        EXPECT_EQ(h.ui.current(), ScreenId::kFace);
    }
}

struct MenuRow {
    int index;
    ScreenId target;
};
constexpr std::array<MenuRow, 13> kMenuRows{{
    {0, ScreenId::kTimeDateEditor},
    {1, ScreenId::kTimezonePicker},
    {2, ScreenId::kChoice}, // 12/24h
    {3, ScreenId::kChoice}, // units
    {4, ScreenId::kChoice}, // connectivity
    {5, ScreenId::kWeatherSettings},
    {6, ScreenId::kSyncNow},
    {7, ScreenId::kStepGoalEditor},
    {8, ScreenId::kChoice}, // vibration
    {9, ScreenId::kChoice}, // watch face
    {10, ScreenId::kDiagnostics},
    {11, ScreenId::kAbout},
    {12, ScreenId::kFactoryReset},
}};

TEST(NavMenu, EveryRowOpensItsScreenAndBackReturnsToTheMenu) {
    Harness h;
    for (const MenuRow& row : kMenuRows) {
        h.goto_menu_row(row.index);
        const auto actions = h.click(Button::kMenu);
        EXPECT_EQ(h.ui.current(), row.target) << "row " << row.index;
        if (row.target == ScreenId::kSyncNow) {
            ASSERT_EQ(actions.size(), 1U);
            EXPECT_EQ(actions[0].kind, ActionKind::kSyncNow);
        } else {
            EXPECT_TRUE(actions.empty()) << "row " << row.index;
        }
        (void)h.click(Button::kBack);
        EXPECT_EQ(h.ui.current(), ScreenId::kMenu) << "row " << row.index;
    }
}

TEST(NavMenu, ChoiceRowsEditTheRightSetting) {
    struct Case {
        int index;
        Key key;
    };
    constexpr std::array<Case, 5> cases{{{2, Key::kHourFormat},
                                         {3, Key::kTempUnit},
                                         {4, Key::kConnectivity},
                                         {8, Key::kVibration},
                                         {9, Key::kFace}}};
    for (const Case& c : cases) {
        Harness h;
        h.goto_menu_row(c.index);
        (void)h.click(Button::kMenu);
        const auto actions = h.click(Button::kMenu); // save the pre-selected value
        ASSERT_EQ(actions.size(), 1U) << c.index;
        EXPECT_EQ(actions[0].kind, ActionKind::kSetSetting);
        EXPECT_EQ(actions[0].key, c.key) << c.index;
        EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    }
}

TEST(NavMenu, UpDownMoveWithWrapAndBackLeavesToFace) {
    Harness h;
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp); // wraps from the first row to the last
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.ui.current(), ScreenId::kFactoryReset);
    (void)h.click(Button::kBack);
    (void)h.click(Button::kDown); // and back to the first row
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.ui.current(), ScreenId::kTimeDateEditor);
    h.ui.reset_to_face();
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kBack);
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
}

TEST(NavMenu, HeldUpDownStepsTheCursor) {
    Harness h;
    (void)h.click(Button::kMenu);
    (void)h.send(test::hold(Button::kDown));
    (void)h.send(test::repeat(Button::kDown, 850));
    (void)h.click(Button::kMenu); // row 2: 12/24 hour
    EXPECT_EQ(h.ui.current(), ScreenId::kChoice);
}

TEST(NavMenu, RowsThatNeedTheRadioAreHiddenWithoutIt) {
    Harness h;
    h.state.radio_available = false;
    // Visible rows: Time & date, Time zone, 12/24, Units, Step goal, Vibration, Face, Diagnostics,
    // About, Factory reset.
    (void)h.click(Button::kMenu);
    (void)h.clicks(Button::kDown, 4);
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.ui.current(), ScreenId::kStepGoalEditor);
    (void)h.click(Button::kBack);
    (void)h.click(Button::kBack);
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp);
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.ui.current(), ScreenId::kFactoryReset);
}

TEST(NavTimeDate, MenuAdvancesFieldsAndLastFieldSavesBackStepsBackFirstCancels) {
    Harness h;
    h.goto_menu_row(0);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kTimeDateEditor);
    EXPECT_TRUE(h.clicks(Button::kMenu, 4).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kTimeDateEditor);
    (void)h.click(Button::kBack); // previous field, still editing
    EXPECT_EQ(h.ui.current(), ScreenId::kTimeDateEditor);
    EXPECT_EQ(h.click(Button::kMenu).size(), 0U); // back on the last field
    const auto saved = h.click(Button::kMenu);
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved[0].kind, ActionKind::kSetTime);
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);

    (void)h.click(Button::kMenu);                    // reopen from the Menu cursor (row 0)
    EXPECT_TRUE(h.clicks(Button::kBack, 1).empty()); // first field: cancel
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavTimezone, MenuSelectsAndSavesBackCancels) {
    Harness h;
    h.goto_menu_row(1);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kTimezonePicker);
    (void)h.click(Button::kDown);
    const auto saved = h.click(Button::kMenu);
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved[0].key, Key::kTimeZone);
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kDown);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavChoice, MenuSavesBackCancels) {
    Harness h;
    h.goto_menu_row(2);
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kDown);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.click(Button::kMenu).size(), 1U);
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavWeatherSettings, RowsToggleOrOpenAndBackReturnsToMenu) {
    Harness h;
    h.goto_menu_row(5);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kWeatherSettings);

    // Row 0: weather on/off (toggles connectivity), stays on the screen.
    auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].key, Key::kConnectivity);
    EXPECT_EQ(a[0].value.view(), "time+weather");
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);
    h.settings.connectivity = model::ConnectivityMode::kTimeWeather;
    a = h.click(Button::kMenu);
    EXPECT_EQ(a[0].value.view(), "time");

    // Row 1: high/low on/off.
    (void)h.click(Button::kDown);
    a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].key, Key::kWeatherHighLow);
    EXPECT_EQ(a[0].value.view(), "off"); // currently on
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);

    // Row 2: interval choice, returns here.
    (void)h.click(Button::kDown);
    EXPECT_TRUE(h.click(Button::kMenu).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kChoice);
    a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].key, Key::kWeatherIntervalMin);
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);

    // Row 3: location editor, cancel returns here.
    (void)h.click(Button::kDown);
    EXPECT_TRUE(h.click(Button::kMenu).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kLocationEditor);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);

    // Row 4: Setup Wi-Fi starts provisioning; BACK there stops it.
    (void)h.click(Button::kDown);
    a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kStartProvisioning);
    EXPECT_EQ(h.ui.current(), ScreenId::kProvisioning);
    a = h.click(Button::kBack);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kStopProvisioning);
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);

    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavWeatherSettings, WifiRowHiddenWithoutRadio) {
    Harness h;
    h.state.radio_available = false;
    ASSERT_TRUE(h.ui.show(ScreenId::kWeatherSettings, h.state));
    (void)h.click(Button::kUp); // wraps to the last visible row: Location
    (void)h.click(Button::kMenu);
    EXPECT_EQ(h.ui.current(), ScreenId::kLocationEditor);
}

TEST(NavStepGoal, MenuSavesBackCancels) {
    Harness h;
    h.goto_menu_row(7);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kStepGoalEditor);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
    (void)h.click(Button::kMenu);
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].key, Key::kStepGoal);
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavSyncNow, MenuRetriesUnlessRunningBackLeaves) {
    Harness h;
    h.goto_menu_row(6);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kSyncNow);
    expect_no_effect(h, ScreenId::kSyncNow, {Button::kUp, Button::kDown});
    h.state.op_phase = OpPhase::kRunning;
    EXPECT_TRUE(h.click(Button::kMenu).empty());
    h.state.op_phase = OpPhase::kFailed;
    auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kSyncNow);
    EXPECT_EQ(h.ui.current(), ScreenId::kSyncNow);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavProvisioning, OnlyBackActsAndItStopsProvisioning) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kProvisioning, h.state));
    expect_no_effect(h, ScreenId::kProvisioning, {Button::kUp, Button::kDown, Button::kMenu});
    const auto a = h.click(Button::kBack);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kStopProvisioning);
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);
}

TEST(NavDiagnostics, UpDownPageMenuRunsSelfTestOnLastPageOnly) {
    Harness h;
    h.goto_menu_row(10);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kDiagnostics);
    EXPECT_TRUE(h.click(Button::kMenu).empty()); // page 0: nothing to run
    EXPECT_TRUE(h.clicks(Button::kDown, 4).empty());
    EXPECT_TRUE(h.click(Button::kMenu).empty()); // page 4
    (void)h.click(Button::kDown);                // page 5 = last (self-test)
    auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kRunSelfTest);
    EXPECT_EQ(h.ui.current(), ScreenId::kDiagnostics);
    (void)h.click(Button::kDown); // wraps to page 0
    EXPECT_TRUE(h.click(Button::kMenu).empty());
    (void)h.click(Button::kUp); // wraps back to the last page
    EXPECT_EQ(h.click(Button::kMenu).size(), 1U);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavAbout, OnlyBackActs) {
    Harness h;
    h.goto_menu_row(11);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kAbout);
    expect_no_effect(h, ScreenId::kAbout, {Button::kUp, Button::kDown, Button::kMenu});
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavFactoryReset, ConfirmsOnlyOnTheThreeSecondMenuHoldBackCancels) {
    Harness h;
    h.goto_menu_row(12);
    (void)h.click(Button::kMenu);
    ASSERT_EQ(h.ui.current(), ScreenId::kFactoryReset);
    expect_no_effect(h, ScreenId::kFactoryReset, {Button::kUp, Button::kDown, Button::kMenu});
    EXPECT_TRUE(h.send(test::hold(Button::kMenu)).empty()); // not "home" here
    EXPECT_EQ(h.ui.current(), ScreenId::kFactoryReset);
    EXPECT_TRUE(h.send(test::repeat(Button::kMenu, 2999)).empty());
    EXPECT_TRUE(h.send(test::repeat(Button::kUp, 3000)).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kFactoryReset);
    const auto a = h.send(test::repeat(Button::kMenu, 3000));
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].kind, ActionKind::kFactoryReset);
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);

    h.goto_menu_row(12);
    (void)h.click(Button::kMenu);
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kMenu);
}

TEST(NavChargeMe, IgnoresEveryButton) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kChargeMe, h.state));
    expect_no_effect(
        h, ScreenId::kChargeMe, {Button::kMenu, Button::kBack, Button::kUp, Button::kDown});
}

TEST(NavStatusOverlay, BackReturnsToFaceOthersIgnored) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kStatusOverlay, h.state));
    expect_no_effect(h, ScreenId::kStatusOverlay, {Button::kMenu, Button::kUp, Button::kDown});
    EXPECT_TRUE(h.click(Button::kBack).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
}

TEST(NavGlobal, HoldMenuGoesToFaceFromEveryScreenWithoutActions) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(ScreenId::kCount); ++i) {
        const auto id = static_cast<ScreenId>(i);
        Harness h;
        ASSERT_TRUE(h.ui.show(id, h.state)) << screen_name(id);
        const auto actions = h.send(test::hold(Button::kMenu));
        EXPECT_TRUE(actions.empty()) << screen_name(id);
        EXPECT_EQ(h.ui.current(),
                  id == ScreenId::kFactoryReset ? ScreenId::kFactoryReset : ScreenId::kFace)
            << screen_name(id);
    }
}

TEST(NavGlobal, HoldMenuFromAnEditorDiscardsEdits) {
    Harness h;
    h.goto_menu_row(7);
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kUp);
    EXPECT_TRUE(h.send(test::hold(Button::kMenu)).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
    // Reopening starts from the saved value again.
    h.goto_menu_row(7);
    (void)h.click(Button::kMenu);
    const auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].value.view(), "0");
}

TEST(NavGlobal, OtherHoldsAndRepeatsAreNotNavigation) {
    Harness h;
    h.goto_menu_row(10);
    (void)h.click(Button::kMenu);
    EXPECT_TRUE(h.send(test::hold(Button::kBack)).empty());
    EXPECT_TRUE(h.send(test::repeat(Button::kMenu, 3000)).empty());
    EXPECT_EQ(h.ui.current(), ScreenId::kDiagnostics);
}

TEST(NavRefresh, LeavingTheMenuTreeAsksForAFullRefresh) {
    Harness h;
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial);
    (void)h.click(Button::kMenu); // entering the menu: partial is fine
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial);
    (void)h.click(Button::kBack); // Menu -> face
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kFull);
    h.ui.clear_refresh_hint();
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial);

    // Deeper screens: only the final step to the face counts.
    h.goto_menu_row(11);
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kBack); // About -> Menu
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial);
    (void)h.click(Button::kBack);
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kFull);
    h.ui.clear_refresh_hint();

    // Hold MENU from deep in the tree.
    h.goto_menu_row(5);
    (void)h.click(Button::kMenu);
    (void)h.send(test::hold(Button::kMenu));
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kFull);
    h.ui.clear_refresh_hint();

    // A confirmed factory reset also leaves the tree.
    ASSERT_TRUE(h.ui.show(ScreenId::kFactoryReset, h.state));
    h.ui.clear_refresh_hint();
    (void)h.send(test::repeat(Button::kMenu, 3000));
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kFull);
}

TEST(NavRefresh, GlanceScreensReturnWithPartialRefresh) {
    for (const ScreenId id :
         {ScreenId::kStepsHistory, ScreenId::kWeatherDetail, ScreenId::kStatusOverlay}) {
        Harness h;
        ASSERT_TRUE(h.ui.show(id, h.state));
        h.ui.clear_refresh_hint();
        (void)h.click(Button::kBack);
        EXPECT_EQ(h.ui.current(), ScreenId::kFace);
        EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial) << screen_name(id);
    }
    Harness h;
    (void)h.click(Button::kUp);
    (void)h.send(test::hold(Button::kMenu));
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial);
}

TEST(NavRefresh, ResetToFaceClearsTheHint) {
    Harness h;
    (void)h.click(Button::kMenu);
    (void)h.click(Button::kBack);
    ASSERT_EQ(h.ui.refresh_hint(), RefreshHint::kFull);
    h.ui.reset_to_face();
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
    EXPECT_EQ(h.ui.refresh_hint(), RefreshHint::kPartial);
}

TEST(NavIdle, TimeoutsPerScreen) {
    struct Case {
        ScreenId id;
        std::int64_t timeout_ms;
    };
    constexpr std::array<Case, 17> cases{{
        {ScreenId::kFace, 2'000},
        {ScreenId::kChargeMe, 2'000},
        {ScreenId::kStatusOverlay, 5'000},
        {ScreenId::kStepsHistory, 30'000},
        {ScreenId::kWeatherDetail, 30'000},
        {ScreenId::kMenu, 30'000},
        {ScreenId::kTimeDateEditor, 30'000},
        {ScreenId::kTimezonePicker, 30'000},
        {ScreenId::kChoice, 30'000},
        {ScreenId::kWeatherSettings, 30'000},
        {ScreenId::kLocationEditor, 30'000},
        {ScreenId::kStepGoalEditor, 30'000},
        {ScreenId::kSyncNow, 60'000},
        {ScreenId::kProvisioning, 300'000},
        {ScreenId::kDiagnostics, 30'000},
        {ScreenId::kAbout, 30'000},
        {ScreenId::kFactoryReset, 30'000},
    }};
    for (const Case& c : cases) {
        Harness h;
        ASSERT_TRUE(h.ui.show(c.id, h.state));
        const std::int64_t last = 5'000'000;
        const std::int64_t t = c.timeout_ms * 1000;
        EXPECT_EQ(h.ui.idle_timeout_us(), t) << screen_name(c.id);
        EXPECT_FALSE(h.ui.idle_expired(last, last)) << screen_name(c.id);
        EXPECT_FALSE(h.ui.idle_expired(last + t - 1, last)) << screen_name(c.id);
        EXPECT_TRUE(h.ui.idle_expired(last + t, last)) << screen_name(c.id);
        EXPECT_TRUE(h.ui.idle_expired(last + t + 1'000'000, last)) << screen_name(c.id);
    }
}

TEST(NavIdle, MenuIdleIsThirtySecondsFromTheLastInput) {
    Harness h;
    (void)h.click(Button::kMenu);
    EXPECT_FALSE(h.ui.idle_expired(29'999'000, 0));
    EXPECT_TRUE(h.ui.idle_expired(30'000'000, 0));
}

TEST(NavShow, RebuildsTheStackSoBackWalksTheMenuPath) {
    struct Case {
        ScreenId id;
        std::vector<ScreenId> back_path; ///< screens visited by repeated BACK (before the face)
    };
    const std::vector<Case> cases{
        {ScreenId::kMenu, {}},
        {ScreenId::kTimeDateEditor, {ScreenId::kMenu}},
        {ScreenId::kTimezonePicker, {ScreenId::kMenu}},
        {ScreenId::kChoice, {ScreenId::kMenu}},
        {ScreenId::kWeatherSettings, {ScreenId::kMenu}},
        {ScreenId::kLocationEditor, {ScreenId::kWeatherSettings, ScreenId::kMenu}},
        {ScreenId::kStepGoalEditor, {ScreenId::kMenu}},
        {ScreenId::kSyncNow, {ScreenId::kMenu}},
        {ScreenId::kProvisioning, {ScreenId::kWeatherSettings, ScreenId::kMenu}},
        {ScreenId::kDiagnostics, {ScreenId::kMenu}},
        {ScreenId::kAbout, {ScreenId::kMenu}},
        {ScreenId::kFactoryReset, {ScreenId::kMenu}},
        {ScreenId::kStepsHistory, {}},
        {ScreenId::kWeatherDetail, {}},
        {ScreenId::kStatusOverlay, {}},
    };
    for (const Case& c : cases) {
        Harness h;
        ASSERT_TRUE(h.ui.show(c.id, h.state)) << screen_name(c.id);
        EXPECT_EQ(h.ui.current(), c.id);
        for (const ScreenId expected : c.back_path) {
            (void)h.click(Button::kBack);
            EXPECT_EQ(h.ui.current(), expected) << screen_name(c.id);
        }
        (void)h.click(Button::kBack);
        EXPECT_EQ(h.ui.current(), ScreenId::kFace) << screen_name(c.id);
    }
}

TEST(NavShow, FaceAndErrors) {
    Harness h;
    ASSERT_TRUE(h.ui.show(ScreenId::kMenu, h.state));
    ASSERT_TRUE(h.ui.show(ScreenId::kFace, h.state));
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
    EXPECT_FALSE(h.ui.show(ScreenId::kCount, h.state));
    EXPECT_FALSE(h.ui.show_choice(ChoiceKind::kCount, h.state));
    EXPECT_EQ(h.ui.current(), ScreenId::kFace);
}

TEST(NavShow, ShowChoiceSelectsTheKindAndWeatherIntervalReturnsToWeatherSettings) {
    Harness h;
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kSyncInterval, h.state));
    auto a = h.click(Button::kMenu);
    ASSERT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0].key, Key::kSyncIntervalH);
    EXPECT_EQ(a[0].value.view(), "24");
    ASSERT_TRUE(h.ui.show_choice(ChoiceKind::kWeatherInterval, h.state));
    a = h.click(Button::kMenu);
    EXPECT_EQ(a[0].key, Key::kWeatherIntervalMin);
    EXPECT_EQ(a[0].value.view(), "60");
    EXPECT_EQ(h.ui.current(), ScreenId::kWeatherSettings);
}

TEST(NavNames, RoundTripAndUnknown) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(ScreenId::kCount); ++i) {
        const auto id = static_cast<ScreenId>(i);
        const auto name = screen_name(id);
        EXPECT_FALSE(name.empty());
        const auto back = screen_from_name(name);
        ASSERT_TRUE(back.has_value()) << name;
        EXPECT_EQ(*back, id);
    }
    EXPECT_EQ(screen_name(ScreenId::kFace), "face");
    EXPECT_EQ(screen_name(ScreenId::kMenu), "menu");
    EXPECT_TRUE(screen_name(ScreenId::kCount).empty());
    const auto bad = screen_from_name("nope");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code, Errc::kNotFound);
}

TEST(NavRender, EveryScreenRendersDeterministicallyAndNonBlank) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(ScreenId::kCount); ++i) {
        const auto id = static_cast<ScreenId>(i);
        Harness h;
        ASSERT_TRUE(h.ui.show(id, h.state));
        const auto a = h.draw();
        const auto b = h.draw();
        EXPECT_EQ(a.crc32(), b.crc32()) << screen_name(id);
        EXPECT_GT(test::black_pixels(a), 0) << screen_name(id);
    }
}

TEST(NavRender, FaceUsesTheSettingsFaceIdAndClearsFirst) {
    Harness h;
    (void)h.draw();
    EXPECT_EQ(h.faces.last_rendered, 0);
    h.settings.face_id = 3;
    (void)h.draw();
    EXPECT_EQ(h.faces.last_rendered, 3);
    // Rendering clears the canvas: stale ink never survives.
    gfx::Framebuffer fb;
    fb.clear(gfx::Color::kBlack);
    gfx::Canvas canvas(fb);
    canvas.set_clip({0, 0, 1, 1});
    h.ui.render(h.state, canvas);
    EXPECT_EQ(fb.get(150, 150), gfx::Color::kWhite);
}

TEST(NavRender, ListsScrollWithoutTouchingTheScrollbarColumnsOutsideTheScreen) {
    Harness h;
    h.settings.tz_name = "UTC";
    ASSERT_TRUE(h.ui.show(ScreenId::kTimezonePicker, h.state));
    ASSERT_GT(time::builtin_zones().size(), 8U);
    for (int i = 0; i < 30; ++i) {
        (void)h.click(Button::kDown);
        EXPECT_GT(test::black_pixels(h.draw()), 0);
    }
    // Held keys on long lists move faster once the hold is long (accelerated step).
    (void)h.send(test::repeat(Button::kDown, 2'500));
    EXPECT_GT(test::black_pixels(h.draw()), 0);
}

TEST(NavRender, MenuWithLongLabelsAndNoSettingsDoesNotCrash) {
    Harness h;
    h.state.settings = nullptr;
    h.state.tz_label = "A very long time zone label that can never fit the row";
    ASSERT_TRUE(h.ui.show(ScreenId::kMenu, h.state));
    EXPECT_GT(test::black_pixels(h.draw()), 0);
    for (const ScreenId id : {ScreenId::kTimeDateEditor,
                              ScreenId::kStepGoalEditor,
                              ScreenId::kLocationEditor,
                              ScreenId::kChoice,
                              ScreenId::kTimezonePicker,
                              ScreenId::kWeatherSettings}) {
        ASSERT_TRUE(h.ui.show(id, h.state));
        EXPECT_GT(test::black_pixels(h.draw()), 0) << screen_name(id);
    }
}

TEST(NavGlobal, EveryButtonKindOnEveryScreenIsSafe) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(ScreenId::kCount); ++i) {
        Harness h;
        ASSERT_TRUE(h.ui.show(static_cast<ScreenId>(i), h.state));
        for (int round = 0; round < 40; ++round) {
            const Button b = kAllButtons[static_cast<std::size_t>(round) % kAllButtons.size()];
            (void)h.send(test::click(b));
            (void)h.send(test::repeat(b, 150U * static_cast<std::uint32_t>(round)));
            (void)h.draw();
        }
    }
}

// NOLINTEND(readability-function-cognitive-complexity)

} // namespace
} // namespace qz::ui
