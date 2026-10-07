// GestureRecognizer: debounce + click/hold/repeat (ARCHITECTURE.md section 15).
#include "qz/ui/ui.hpp"

#include <algorithm>

namespace qz::ui {
namespace {

using model::Button;
using model::InputKind;

constexpr std::int64_t kUsPerMs = 1000;

constexpr std::uint8_t bit_of(Button b) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(b));
}
constexpr std::uint8_t kRepeatMask = bit_of(Button::kUp) | bit_of(Button::kDown);
constexpr std::uint8_t kLongMask = bit_of(Button::kMenu);
constexpr std::uint8_t kChord = bit_of(Button::kBack) | bit_of(Button::kUp);

constexpr std::int64_t ms_to_us(std::uint32_t ms) noexcept {
    return static_cast<std::int64_t>(ms) * kUsPerMs;
}

} // namespace

GestureRecognizer::GestureRecognizer(GestureTiming timing) noexcept : timing_(timing) {}

void GestureRecognizer::seed(std::uint8_t pressed_mask, std::int64_t press_rtc_us) noexcept {
    for (std::size_t i = 0; i < model::kButtonCount; ++i) {
        const auto bit = static_cast<std::uint8_t>(1U << i);
        if ((pressed_mask & bit) == 0) {
            continue;
        }
        raw_ = static_cast<std::uint8_t>(raw_ | bit);
        state_ = static_cast<std::uint8_t>(state_ | bit);
        raw_since_[i] = press_rtc_us;
        pressed_since_[i] = press_rtc_us;
        last_emit_[i] = press_rtc_us;
        hold_sent_ = static_cast<std::uint8_t>(hold_sent_ & ~bit);
        long_sent_ = static_cast<std::uint8_t>(long_sent_ & ~bit);
    }
    if ((state_ & kChord) == kChord) {
        suppressed_ = static_cast<std::uint8_t>(suppressed_ | kChord);
    }
}

void GestureRecognizer::emit_timed(std::size_t index,
                                   std::int64_t end_us,
                                   StaticVector<model::InputEvent, 8>& out) noexcept {
    const auto bit = static_cast<std::uint8_t>(1U << index);
    if ((suppressed_ & bit) != 0) {
        return;
    }
    const auto button = static_cast<Button>(index);
    if ((hold_sent_ & bit) == 0) {
        const std::int64_t due = pressed_since_[index] + ms_to_us(timing_.hold_ms);
        if (due > end_us) {
            return;
        }
        const model::InputEvent ev{button, InputKind::kHold, timing_.hold_ms, due};
        if (!out.push_back(ev)) {
            return;
        }
        hold_sent_ = static_cast<std::uint8_t>(hold_sent_ | bit);
        last_emit_[index] = due;
    }
    if ((kRepeatMask & bit) != 0) {
        while (last_emit_[index] + ms_to_us(timing_.repeat_ms) <= end_us) {
            const std::int64_t next = last_emit_[index] + ms_to_us(timing_.repeat_ms);
            const auto held_ms =
                static_cast<std::uint32_t>((next - pressed_since_[index]) / kUsPerMs);
            if (!out.push_back({button, InputKind::kRepeat, held_ms, next})) {
                return;
            }
            last_emit_[index] = next;
        }
    } else if ((kLongMask & bit) != 0 && (long_sent_ & bit) == 0) {
        const std::int64_t due = pressed_since_[index] + ms_to_us(timing_.long_hold_ms);
        if (due <= end_us &&
            out.push_back({button, InputKind::kRepeat, timing_.long_hold_ms, due})) {
            long_sent_ = static_cast<std::uint8_t>(long_sent_ | bit);
        }
    }
}

void GestureRecognizer::sample(std::uint8_t pressed_mask,
                               std::int64_t now_rtc_us,
                               StaticVector<model::InputEvent, 8>& out,
                               std::uint8_t latched_mask) noexcept {
    const std::int64_t debounce_us = ms_to_us(timing_.debounce_ms);
    // Pass 0: taps that began and ended between samples (the caller was busy or asleep without a
    // button wake). A button down now, or already tracked, is left to the passes below.
    const auto unseen = static_cast<std::uint8_t>(latched_mask & ~pressed_mask & ~raw_ & ~state_);
    if ((unseen & kChord) != kChord) {
        for (std::size_t i = 0; i < model::kButtonCount; ++i) {
            if ((unseen & (1U << i)) != 0) {
                (void)out.push_back({static_cast<Button>(i), InputKind::kClick, 0, now_rtc_us});
            }
        }
    }
    // Pass 1: track raw level changes, accept settled presses.
    for (std::size_t i = 0; i < model::kButtonCount; ++i) {
        const auto bit = static_cast<std::uint8_t>(1U << i);
        const bool raw_now = (pressed_mask & bit) != 0;
        if (raw_now != ((raw_ & bit) != 0)) {
            raw_ = static_cast<std::uint8_t>(raw_ ^ bit);
            raw_since_[i] = now_rtc_us;
        }
        if (raw_now && (state_ & bit) == 0 && now_rtc_us - raw_since_[i] >= debounce_us) {
            state_ = static_cast<std::uint8_t>(state_ | bit);
            pressed_since_[i] = raw_since_[i];
            last_emit_[i] = raw_since_[i];
            hold_sent_ = static_cast<std::uint8_t>(hold_sent_ & ~bit);
            long_sent_ = static_cast<std::uint8_t>(long_sent_ & ~bit);
        }
    }
    if ((state_ & kChord) == kChord) {
        suppressed_ = static_cast<std::uint8_t>(suppressed_ | kChord);
    }
    // Pass 2: hold/repeat events, then accepted releases.
    for (std::size_t i = 0; i < model::kButtonCount; ++i) {
        const auto bit = static_cast<std::uint8_t>(1U << i);
        if ((state_ & bit) == 0) {
            continue;
        }
        const bool raw_down = (raw_ & bit) != 0;
        const bool release_accepted = !raw_down && now_rtc_us - raw_since_[i] >= debounce_us;
        emit_timed(i, raw_down ? now_rtc_us : raw_since_[i], out);
        if (!release_accepted) {
            continue;
        }
        if ((hold_sent_ & bit) == 0 && (suppressed_ & bit) == 0) {
            const std::int64_t held_us =
                std::max<std::int64_t>(0, raw_since_[i] - pressed_since_[i]);
            if (held_us >= ms_to_us(timing_.hold_ms)) {
                continue; // the Hold did not fit in `out`: retry on the next sample
            }
            const model::InputEvent ev{static_cast<Button>(i),
                                       InputKind::kClick,
                                       static_cast<std::uint32_t>(held_us / kUsPerMs),
                                       raw_since_[i]};
            if (!out.push_back(ev)) {
                continue; // output full: keep state, report on the next sample
            }
        }
        state_ = static_cast<std::uint8_t>(state_ & ~bit);
        hold_sent_ = static_cast<std::uint8_t>(hold_sent_ & ~bit);
        long_sent_ = static_cast<std::uint8_t>(long_sent_ & ~bit);
        suppressed_ = static_cast<std::uint8_t>(suppressed_ & ~bit);
    }
}

std::int64_t GestureRecognizer::next_deadline_us() const noexcept {
    std::int64_t best = -1;
    const auto consider = [&best](std::int64_t t) {
        if (best < 0 || t < best) {
            best = t;
        }
    };
    for (std::size_t i = 0; i < model::kButtonCount; ++i) {
        const auto bit = static_cast<std::uint8_t>(1U << i);
        const bool raw_down = (raw_ & bit) != 0;
        const bool accepted = (state_ & bit) != 0;
        if (raw_down != accepted) {
            consider(raw_since_[i] + ms_to_us(timing_.debounce_ms));
        }
        if (!accepted || !raw_down || (suppressed_ & bit) != 0) {
            continue;
        }
        if ((hold_sent_ & bit) == 0) {
            consider(pressed_since_[i] + ms_to_us(timing_.hold_ms));
        } else if ((kRepeatMask & bit) != 0) {
            consider(last_emit_[i] + ms_to_us(timing_.repeat_ms));
        } else if ((kLongMask & bit) != 0 && (long_sent_ & bit) == 0) {
            consider(pressed_since_[i] + ms_to_us(timing_.long_hold_ms));
        }
    }
    return best;
}

bool GestureRecognizer::any_pressed() const noexcept {
    return (state_ | raw_) != 0;
}

} // namespace qz::ui
