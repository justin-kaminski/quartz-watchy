// Suite "interactive" (manual, gated by BuildFeatures.selftest_interactive and a tethered
// session): each test prompts on the panel and waits for the owner. Never runs unattended.
#include "detail.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/hal/board_io.hpp"
#include "tests.hpp"
#include "tuning.hpp"

#include <cstdint>

namespace qz::selftest::tests {

namespace {

struct PromptFrames {
    gfx::Framebuffer previous;
    gfx::Framebuffer next;
};

PromptFrames& prompt_frames() noexcept {
    static PromptFrames f;
    return f;
}

bool prerequisites(Context& ctx) noexcept {
    return ctx.panel != nullptr && ctx.io != nullptr && ctx.delay != nullptr;
}

/// Draws two centred lines with a partial refresh; false when the panel refuses.
bool show_prompt(Context& ctx, std::string_view line1, std::string_view line2) noexcept {
    PromptFrames& f = prompt_frames();
    f.previous = f.next;
    f.next.clear();
    gfx::Canvas c(f.next);
    c.text_aligned(0,
                   70,
                   gfx::kWidth,
                   "SELF-TEST",
                   gfx::font(gfx::FontId::kMedium),
                   gfx::Align::kCenter,
                   gfx::Color::kBlack);
    c.text_aligned(0,
                   110,
                   gfx::kWidth,
                   line1,
                   gfx::font(gfx::FontId::kLarge),
                   gfx::Align::kCenter,
                   gfx::Color::kBlack);
    c.text_aligned(0,
                   140,
                   gfx::kWidth,
                   line2,
                   gfx::font(gfx::FontId::kSmall),
                   gfx::Align::kCenter,
                   gfx::Color::kBlack);
    if (!ctx.panel->init()) {
        return false;
    }
    return ctx.panel->update(f.previous, f.next, ssd1681::UpdateMode::kPartial).has_value();
}

/// Waits (bounded) for any button, returns its mask once released; kTimeout otherwise.
Result<std::uint8_t> wait_for_press(Context& ctx) noexcept {
    constexpr std::uint32_t kPolls = tuning::kPromptTimeoutMs / tuning::kPromptPollMs;
    std::uint8_t mask = 0;
    for (std::uint32_t i = 0; i < kPolls && mask == 0; ++i) {
        mask = ctx.io->pressed_buttons();
        if (mask == 0) {
            ctx.delay->delay_ms(tuning::kPromptPollMs);
        }
    }
    if (mask == 0) {
        return Errc::kTimeout;
    }
    // Wait for release so the next prompt does not see the same press.
    for (std::uint32_t i = 0; i < kPolls && ctx.io->pressed_buttons() != 0; ++i) {
        ctx.delay->delay_ms(tuning::kPromptPollMs);
    }
    return mask;
}

Outcome expect_button(Context& ctx, Detail& d, std::uint8_t bit, std::string_view label) noexcept {
    if (!prerequisites(ctx)) {
        return skip(d, "needs panel, io and delay");
    }
    if (!show_prompt(ctx, label, "within 10 s")) {
        return fail(d, "could not draw prompt");
    }
    const Result<std::uint8_t> mask = wait_for_press(ctx);
    if (!mask) {
        return fail(d, "timeout, no button pressed");
    }
    if (*mask != bit) {
        return fail(d, Text().put("wrong button, mask ").num(*mask).view());
    }
    return pass(d, "pressed");
}

} // namespace

Outcome button_menu(Context& ctx, Detail& d) noexcept {
    return expect_button(ctx, d, hal::kButtonBitMenu, "Press MENU");
}
Outcome button_back(Context& ctx, Detail& d) noexcept {
    return expect_button(ctx, d, hal::kButtonBitBack, "Press BACK");
}
Outcome button_up(Context& ctx, Detail& d) noexcept {
    return expect_button(ctx, d, hal::kButtonBitUp, "Press UP");
}
Outcome button_down(Context& ctx, Detail& d) noexcept {
    return expect_button(ctx, d, hal::kButtonBitDown, "Press DOWN");
}

Outcome vibration(Context& ctx, Detail& d) noexcept {
    if (!prerequisites(ctx)) {
        return skip(d, "needs panel, io and delay");
    }
    ctx.io->set_vibration(true);
    ctx.delay->delay_ms(tuning::kBuzzMs);
    ctx.io->set_vibration(false); // always off again before anything can fail
    if (!show_prompt(ctx, "Did it buzz?", "UP = yes, DOWN = no")) {
        return fail(d, "could not draw prompt");
    }
    const Result<std::uint8_t> mask = wait_for_press(ctx);
    if (!mask) {
        return fail(d, "timeout, no answer");
    }
    if (*mask == hal::kButtonBitUp) {
        return pass(d, "buzz confirmed");
    }
    return fail(d, *mask == hal::kButtonBitDown ? "no buzz reported" : "unexpected button");
}

} // namespace qz::selftest::tests
