// Private screen framework shared by the WP-14 screens and the WP-15 system screens.
//
// WP-15 contract: a system screen derives from `Screen`, is a member of `Ui::Impl` (ui.cpp), and
// reports navigation through `Outcome` instead of touching the stack. `SystemScreen` below is the
// WP-14 stand-in for the screens WP-15 renders (StepsHistory, WeatherDetail, SyncNow,
// Provisioning, Diagnostics, About, FactoryReset, ChargeMe, StatusOverlay): it owns their input
// behaviour (the navigation graph) and draws a bare title. WP-15 replaces its render().
#pragma once

#include "qz/ui/ui.hpp"
#include "tuning.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace qz::ui {

// ---- navigation requests -------------------------------------------------------------------
struct Nav {
    enum class Kind : std::uint8_t {
        kNone,
        kPush, ///< open `target` (with `param`) above the current screen
        kPop,  ///< back to the previous screen
        kHome  ///< straight to the face
    };
    Kind kind = Kind::kNone;
    ScreenId target = ScreenId::kFace;
    std::uint8_t param = 0;
};

/// What a screen's handle() wants: actions for the app plus at most one navigation step.
struct Outcome {
    ActionList actions;
    Nav nav;

    [[nodiscard]] static Outcome none() noexcept { return {}; }
    [[nodiscard]] static Outcome push(ScreenId id, std::uint8_t param = 0) noexcept;
    [[nodiscard]] static Outcome pop() noexcept;
    [[nodiscard]] static Outcome home() noexcept;
    /// Appends an action (the action list is sized for every screen's worst case).
    Outcome& with(const Action& action) noexcept;
};

[[nodiscard]] Action make_action(ActionKind kind) noexcept;
[[nodiscard]] Action make_set_setting(settings::Key key, std::string_view value) noexcept;

// ---- screen base ---------------------------------------------------------------------------
class Screen {
public:
    Screen() = default;
    virtual ~Screen() = default;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;
    Screen(Screen&&) = delete;
    Screen& operator=(Screen&&) = delete;

    /// Called when the screen is opened (push / show): editors copy their values from `state`.
    virtual void enter(const WatchState& state, std::uint8_t param) noexcept = 0;
    virtual void render(const WatchState& state, gfx::Canvas& canvas) const noexcept = 0;
    [[nodiscard]] virtual Outcome handle(const model::InputEvent& event,
                                         const WatchState& state) noexcept = 0;
};

// ---- input helpers -------------------------------------------------------------------------
[[nodiscard]] bool is_click(const model::InputEvent& e, model::Button b) noexcept;
/// Value editors: UP = +1, DOWN = -1 for Click, Hold and Repeat (held buttons keep stepping).
[[nodiscard]] int step_direction(const model::InputEvent& e) noexcept;
/// Lists and pages: DOWN = +1 (next row), UP = -1; same event kinds as step_direction().
[[nodiscard]] int list_direction(const model::InputEvent& e) noexcept;
/// Step multiplier for long holds (1 until tuning::kAccelAfterMs).
[[nodiscard]] std::uint32_t step_multiplier(const model::InputEvent& e) noexcept;
/// Wraps `index + delta` into [0, count); count must be > 0.
[[nodiscard]] std::size_t wrap_index(std::size_t index, int delta, std::size_t count) noexcept;

// ---- text building (no snprintf, no heap) --------------------------------------------------
template<std::size_t N>
class TextBuilder {
public:
    TextBuilder& put(std::string_view s) noexcept {
        for (const char c : s) {
            if (len_ >= N) {
                trim_partial_utf8();
                break;
            }
            buf_[len_++] = c;
        }
        return *this;
    }
    TextBuilder& put_char(char c) noexcept { return put(std::string_view(&c, 1)); }
    /// Decimal digits, zero-padded to `min_digits`.
    TextBuilder& put_uint(std::uint32_t v, unsigned min_digits = 1) noexcept {
        std::array<char, 10> tmp{};
        std::size_t n = 0;
        do {
            tmp[n++] = static_cast<char>('0' + (v % 10U));
            v /= 10U;
        } while (v != 0 && n < tmp.size());
        while (n < min_digits && n < tmp.size()) {
            tmp[n++] = '0';
        }
        while (n > 0) {
            put_char(tmp[--n]);
        }
        return *this;
    }
    [[nodiscard]] std::string_view view() const noexcept { return {buf_.data(), len_}; }

private:
    void trim_partial_utf8() noexcept {
        while (len_ > 0 && (static_cast<unsigned char>(buf_[len_ - 1]) & 0xC0U) == 0x80U) {
            --len_;
        }
        if (len_ > 0 && (static_cast<unsigned char>(buf_[len_ - 1]) & 0x80U) != 0) {
            --len_; // lead byte of a sequence that did not fit
        }
    }
    std::array<char, N> buf_{};
    std::size_t len_ = 0;
};

/// `s` shortened with "..." (at a code-point boundary) so it fits `max_width` pixels.
[[nodiscard]] FixedString<48>
fit_text(std::string_view s, const gfx::Font& f, std::int16_t max_width) noexcept;

// ---- drawing -------------------------------------------------------------------------------
inline constexpr std::size_t kVisibleRows = 8;

struct ListRow {
    TextBuilder<40> label;
    TextBuilder<16> value;
    std::int8_t mark = -1; ///< -1 none, 0 empty radio, 1 filled radio
};

void draw_title_bar(gfx::Canvas& canvas, std::string_view title) noexcept;
void draw_hint(gfx::Canvas& canvas, std::string_view hint) noexcept;
void draw_row(gfx::Canvas& canvas, std::int16_t y, const ListRow& row, bool selected) noexcept;
void draw_scrollbar(gfx::Canvas& canvas, std::size_t first, std::size_t count) noexcept;
/// First visible row so that `selected` stays visible (and roughly centered).
[[nodiscard]] std::size_t window_first(std::size_t selected, std::size_t count) noexcept;
/// Text centered on x = 100 with characters [begin, begin + len) drawn inverted.
void draw_marked_text(gfx::Canvas& canvas,
                      std::int16_t baseline,
                      const gfx::Font& font,
                      std::string_view text,
                      std::size_t mark_begin,
                      std::size_t mark_len) noexcept;
/// Centered caption line in the small font.
void draw_caption(gfx::Canvas& canvas, std::int16_t baseline, std::string_view text) noexcept;

/// Title bar + scrolling list; `row_fn(index, ListRow&)` fills row `index`.
template<class RowFn>
void draw_list(gfx::Canvas& canvas,
               std::string_view title,
               std::size_t count,
               std::size_t selected,
               const RowFn& row_fn) noexcept {
    draw_title_bar(canvas, title);
    const std::size_t first = window_first(selected, count);
    for (std::size_t i = 0; i < kVisibleRows && first + i < count; ++i) {
        ListRow row;
        row_fn(first + i, row);
        const auto y = static_cast<std::int16_t>(tuning::kTitleBarH + 2 +
                                                 (static_cast<std::int16_t>(i) * tuning::kRowH));
        draw_row(canvas, y, row, first + i == selected);
    }
    draw_scrollbar(canvas, first, count);
}

// ---- WP-14 stand-in for WP-15's screens ----------------------------------------------------
class SystemScreen final : public Screen {
public:
    explicit SystemScreen(ScreenId id) noexcept : id_(id) {}
    void enter(const WatchState& state, std::uint8_t param) noexcept override;
    void render(const WatchState& state, gfx::Canvas& canvas) const noexcept override;
    [[nodiscard]] Outcome handle(const model::InputEvent& event,
                                 const WatchState& state) noexcept override;

private:
    ScreenId id_;
    std::uint8_t page_ = 0; ///< Diagnostics page
};

} // namespace qz::ui
