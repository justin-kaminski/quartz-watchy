// Interactive session (button / tap wakes) and the tethered loop (ARCHITECTURE.md sections 4, 15,
// 17). Light sleep between inputs; minute ticks stay on time while the session runs.
#include "app_core.hpp"
#include "qz/console/protocol.hpp"
#include "qz/time/civil.hpp"

#include <algorithm>

namespace qz::app {

bool Core::screen_is_passive() const noexcept {
    switch (ui_.current()) {
        case ui::ScreenId::kFace:
        case ui::ScreenId::kStepsHistory:
        case ui::ScreenId::kWeatherDetail:
        case ui::ScreenId::kStatusOverlay:
        case ui::ScreenId::kChargeMe:
            return true;
        default:
            return false;
    }
}

void Core::return_to_face() noexcept {
    // Leaving menus/editors flashes the panel clean (full refresh, section 14); leaving a passive
    // view does not.
    const bool full = !screen_is_passive() || ui_.refresh_hint() == ui::RefreshHint::kFull;
    ui_.reset_to_face();
    ui_.clear_refresh_hint();
    const time::UnixMicros target =
        keeper_.valid() ? planner_.tick_target_utc_us(p_.clock.rtc_us()) : 0;
    (void)render_and_present(full, target);
}

bool Core::handle_event(const model::InputEvent& event) noexcept {
    last_input_us_ = p_.clock.rtc_us();
    const ui::ActionList actions = ui_.handle(event, build_state(display_utc_s()));
    for (const ui::Action& action : actions) {
        execute(action);
    }
    const bool full = ui_.refresh_hint() == ui::RefreshHint::kFull;
    ui_.clear_refresh_hint();
    (void)render_and_present(full, 0); // an unchanged frame is not sent to the panel
    return true;
}

void Core::sample_buttons() noexcept {
    StaticVector<model::InputEvent, 8> events;
    const std::uint8_t latched = p_.io.take_latched_buttons(); // before the live sample
    recognizer_.sample(p_.io.pressed_buttons(), p_.clock.rtc_us(), events, latched);
    for (const model::InputEvent& event : events) {
        handle_event(event);
    }
}

void Core::interactive_session(std::uint8_t seed_mask) noexcept {
    recognizer_ = ui::GestureRecognizer{};
    const std::int64_t t0 = p_.clock.rtc_us();
    if (seed_mask != 0) {
        recognizer_.seed(seed_mask, std::min(w_.boot_rtc_us, t0));
    }
    if (ui_.current() == ui::ScreenId::kFace) {
        last_input_us_ = t0;
    } // an overlay session keeps the time it was shown at (show_wake_frame)
    const std::int64_t hard_deadline = t0 + wiring::kSessionMaxUs;
    w_.flags |= model::kWakeFlagInput;

    for (;;) {
        sample_buttons();
        poll_provisioning();
        if (ui_.current() == ui::ScreenId::kFace) {
            refresh_face_if_stale(); // a minute flip while the face is up
        }
        const std::int64_t now = p_.clock.rtc_us();
        const bool pressed = recognizer_.any_pressed();
        if (!pressed && ui_.idle_expired(now, last_input_us_)) {
            break;
        }
        if (now >= hard_deadline) {
            break;
        }
        // A finished phone session shows its result briefly, then the face returns.
        const bool phone_result = ui_.current() == ui::ScreenId::kPhoneSync && !phone_active_;
        if (phone_result && now - phone_ended_us_ >= wiring::kPhoneResultUs) {
            break;
        }

        std::int64_t wait = hard_deadline - now;
        if (pressed) {
            wait = std::min(wait, wiring::kPressedPollUs);
        } else {
            wait = std::min(wait, last_input_us_ + ui_.idle_timeout_us() - now);
        }
        const std::int64_t deadline = recognizer_.next_deadline_us();
        if (deadline >= 0) {
            wait = std::min(wait, deadline - now);
        }
        if (ui_.current() == ui::ScreenId::kFace && keeper_.valid() && shown_minute_ >= 0) {
            const std::int64_t flip =
                keeper_.rtc_at_utc((shown_minute_ + time::kSecondsPerMinute) * time::kUsPerSecond) -
                planner_.lead_us();
            wait = std::min(wait, flip - now);
        }
        if (prov_active_) {
            wait = std::min(wait, wiring::kProvPollUs);
        }
        if (phone_result) {
            wait = std::min(wait, phone_ended_us_ + wiring::kPhoneResultUs - now);
        }
        wait = std::max(wait, wiring::kMinWaitUs);
        if (phone_active_) {
            poll_phone(wait); // no light sleep while the BLE link runs
        } else {
            sleep_for(wait, !pressed);
        }
    }
    stop_phone_impl(conn::PhoneEnd::kNone);
    return_to_face();
}

// ---- tethered loop -------------------------------------------------------------------------

void Core::send_event(std::string_view json) noexcept {
    const std::size_t n = console::format_event(response_, json);
    if (n > 0) {
        p_.console.send_line(std::string_view(response_.data(), n));
    }
}

void Core::send_ready_event() noexcept {
    std::array<char, 320> buf{};
    console::JsonWriter json(buf);
    const hal::FirmwareInfo fw = p_.system.firmware();
    json.begin_object();
    json.field("evt", "ready");
    json.field("proto", static_cast<std::int64_t>(console::kProtocolVersion));
    json.field("fw", fw.version);
    json.field("git", fw.git_hash);
    json.field("reset", reset_name(w_.reason));
    json.end_object();
    send_event(json.view());
}

void Core::handle_console_line(std::size_t length) noexcept {
    const std::string_view reply =
        dispatcher_.handle_line(std::span<char>(request_.data(), length), response_);
    p_.console.send_line(reply);
}

void Core::tether_tick(const WakePlan& plan) noexcept {
    const std::int64_t now = p_.clock.rtc_us();
    const WakeCtx wake = w_; // the tick is its own wake record; the real wake resumes after it
    w_ = WakeCtx{};
    w_.cause = model::WakeCause::kTetheredTick;
    w_.reason = hal::ResetReason::kDeepSleep;
    w_.start_rtc_us = now;
    w_.boot_rtc_us = now;
    w_.usb_present = true;
    w_.safe = planner_.safe_mode(now);
    read_steps();
    sample_battery();
    sync_tap_interrupt();
    if (!keeper_.valid()) {
        w_.flags |= model::kWakeFlagTimeInvalid;
    }
    const time::UnixMicros target =
        plan.kind == WakeKind::kMinuteTick ? plan.target_utc_us : time::UnixMicros{0};
    (void)render_and_present(false, target); // a menu on screen renders unchanged: skipped
    run_connectivity();
    refresh_after_radio();

    std::array<char, 160> buf{};
    console::JsonWriter json(buf);
    json.begin_object();
    json.field("evt", "wake");
    json.field("cause", "tick");
    json.field("utc", keeper_.valid() ? now_utc_s() : 0);
    json.end_object();
    send_event(json.view());

    record_wake();
    commit_all();
    w_ = wake;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): flat event loop
std::int64_t Core::tethered_loop() noexcept {
    const Status started = p_.console.start();
    if (!started) {
        note_error(started.error());
        tether_.on_detached();
        return 0;
    }
    send_ready_event();
    recognizer_ = ui::GestureRecognizer{};
    pending_sleep_s_ = 0;
    pending_reboot_ = false;

    std::int64_t now = p_.clock.rtc_us();
    last_input_us_ = now;
    std::int64_t next_poll = now + wiring::kTetherPollUs;
    std::int64_t next_invalid_tick = now + PlannerTuning{}.invalid_time_tick_us;
    time::UnixMicros last_tick_target = -1;
    std::int64_t sleep_override_us = 0;

    for (;;) {
        now = p_.clock.rtc_us();
        const WakePlan wp = planner_.plan(make_plan_request(0));
        const bool minute_tick = wp.kind == WakeKind::kMinuteTick;
        const bool invalid_tick = wp.kind == WakeKind::kInvalidTime;

        std::int64_t timeout = wiring::kTetherReceiveMaxUs;
        timeout = std::min(timeout, next_poll - now);
        if (minute_tick) {
            timeout = std::min(timeout, wp.wake_rtc_us - now);
        } else if (invalid_tick) {
            timeout = std::min(timeout, next_invalid_tick - now);
        }
        if (recognizer_.any_pressed()) {
            timeout = std::min(timeout, wiring::kPressedPollUs);
        }
        const auto timeout_ms =
            static_cast<std::uint32_t>(std::max<std::int64_t>(timeout / 1000, 1));

        const Result<std::size_t> line = p_.console.receive_line(request_, timeout_ms);
        if (line && *line > 0) {
            handle_console_line(*line);
        } else if (!line) {
            std::array<char, 160> out{};
            const std::size_t n = console::format_err(out, "", line.error(), "line too long");
            if (n > 0) {
                p_.console.send_line(std::string_view(out.data(), n));
            }
        }
        if (pending_reboot_) {
            flush_steps();
            commit_all();
            p_.console.stop();
            tether_.on_detached();
            p_.system.restart(); // never returns on target
            return 0;
        }
        if (pending_sleep_s_ > 0) {
            sleep_override_us = static_cast<std::int64_t>(pending_sleep_s_) * wiring::kUs;
            tether_.request_sleep();
            break;
        }

        sample_buttons();
        poll_provisioning();
        poll_phone(0);
        now = p_.clock.rtc_us();
        if (now >= next_poll) {
            next_poll = now + wiring::kTetherPollUs;
            if (tether_.on_poll(p_.io.usb_present()) == TetherState::kDetaching) {
                send_event(R"({"evt":"detach"})");
                break;
            }
        }
        if (minute_tick && now >= wp.wake_rtc_us && wp.target_utc_us != last_tick_target) {
            last_tick_target = wp.target_utc_us;
            tether_tick(wp);
        } else if (invalid_tick && now >= next_invalid_tick) {
            next_invalid_tick = now + PlannerTuning{}.invalid_time_tick_us;
            tether_tick(wp);
        }
        if (!screen_is_passive() && !recognizer_.any_pressed() &&
            ui_.idle_expired(p_.clock.rtc_us(), last_input_us_)) {
            return_to_face();
        }
    }
    stop_phone_impl(conn::PhoneEnd::kNone);
    p_.console.stop();
    if (tether_.state() == TetherState::kDetaching) {
        tether_.on_detached();
    }
    return sleep_override_us;
}

} // namespace qz::app
