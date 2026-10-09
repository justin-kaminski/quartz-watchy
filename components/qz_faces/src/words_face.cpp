// Words watch face (id 4): the time in words, rounded to the nearest five minutes ("quarter / past
// / four", "twenty-five / to / noon"), with the exact time, date and steps in a small footer.
// Status row on top (power tag, battery, weather, sync). Invalid time: "time / not set".
#include "face_common.hpp"

#include <array>

namespace qz::faces {

namespace {

using gfx::Align;
using gfx::Canvas;
using gfx::Color;
using gfx::FontId;

constexpr Color kInk = Color::kBlack;
constexpr std::int16_t kWordsX = 12;
constexpr std::int16_t kFirstBase = 56;
constexpr std::int16_t kLineStep = 36;
constexpr std::int16_t kRuleY = 158;
constexpr std::int16_t kFooter1Base = 174;
constexpr std::int16_t kFooter2Base = 190;

constexpr std::array<std::string_view, 12> kHours{"twelve",
                                                  "one",
                                                  "two",
                                                  "three",
                                                  "four",
                                                  "five",
                                                  "six",
                                                  "seven",
                                                  "eight",
                                                  "nine",
                                                  "ten",
                                                  "eleven"};
/// Amount words for 5..30 minutes (index = minutes / 5).
constexpr std::array<std::string_view, 7> kAmounts{
    "", "five", "ten", "quarter", "twenty", "twenty-five", "half"};

std::string_view hour_word(std::uint32_t hour24) noexcept {
    if (hour24 == 0 || hour24 == 24) {
        return "midnight";
    }
    if (hour24 == 12) {
        return "noon";
    }
    return kHours[hour24 % 12];
}

struct Phrase {
    std::array<std::string_view, 3> lines{};
    std::size_t count = 0;
    std::size_t hour_line = 0; ///< drawn bold
};

Phrase phrase_for(std::uint32_t hour, std::uint32_t minute) noexcept {
    std::uint32_t rounded = ((minute + 2) / 5) * 5; // nearest five, 58 -> 60
    if (rounded == 60) {
        rounded = 0;
        hour = (hour + 1) % 24;
    }
    Phrase p;
    if (rounded == 0) {
        const std::string_view h = hour_word(hour);
        p.lines[0] = h;
        p.count = 1;
        if (h != "noon" && h != "midnight") {
            p.lines[1] = "o'clock";
            p.count = 2;
        }
        return p;
    }
    const bool past = rounded <= 30;
    p.lines[0] = kAmounts[(past ? rounded : 60 - rounded) / 5];
    p.lines[1] = past ? "past" : "to";
    p.lines[2] = hour_word(past ? hour : (hour + 1) % 24);
    p.count = 3;
    p.hour_line = 2;
    return p;
}

} // namespace

void render_words_face(const ui::WatchState& s, Canvas& c) noexcept {
    c.reset_clip();
    c.clear(Color::kWhite);
    const gfx::Font& large = gfx::font(FontId::kLarge);
    const gfx::Font& small = gfx::font(FontId::kSmall);

    const std::int16_t free_x = draw_status_row(c, s, layout::kBarY);
    (void)draw_weather_compact(c, static_cast<std::int16_t>(free_x + 4), 0, s, small);

    const TimeText tt = format_time(s);
    Phrase p;
    if (tt.text.view() == "--:--") {
        p.lines = {"time", "not set", ""};
        p.count = 2;
        p.hour_line = 3; // none bold
    } else {
        p = phrase_for(s.local.time.hour, s.local.time.minute);
    }
    for (std::size_t i = 0; i < p.count; ++i) {
        const auto base =
            static_cast<std::int16_t>(kFirstBase + (static_cast<std::int16_t>(i) * kLineStep));
        (void)c.text(kWordsX, base, p.lines[i], large, kInk);
        if (i == p.hour_line) {
            (void)c.text(
                static_cast<std::int16_t>(kWordsX + 1), base, p.lines[i], large, kInk); // bold
        }
    }

    c.hline(layout::kMargin,
            kRuleY,
            static_cast<std::int16_t>(gfx::kWidth - (2 * layout::kMargin)),
            kInk);
    TextBuf<24> exact;
    exact.put(tt.text.view());
    if (tt.show_meridiem) {
        exact.put(tt.pm ? " PM" : " AM");
    }
    c.text(layout::kMargin, kFooter1Base, exact.view(), small, kInk);
    if (s.time_valid) {
        c.text_aligned(0,
                       kFooter1Base,
                       static_cast<std::int16_t>(gfx::kWidth - layout::kMargin),
                       format_date(s.local).view(),
                       small,
                       Align::kRight,
                       kInk);
    }
    TextBuf<32> steps;
    steps.put(format_steps(s.steps.today).view());
    if (s.steps.goal != 0) {
        steps.put_char('/');
        steps.put_uint(s.steps.goal);
    }
    steps.put(" steps");
    if (s.steps.goal != 0 && s.steps.today >= s.steps.goal) {
        steps.put("  goal!");
    }
    c.text(layout::kMargin, kFooter2Base, steps.view(), small, kInk);
}

} // namespace qz::faces
