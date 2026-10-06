// Virtual-time simulation harness (WP-22, ARCHITECTURE.md section 3 "the simulator drives the same
// function with a virtual clock"): runs App::run_wake in a loop against the WP-21 test environment.
//
// The harness plays the part of the chip between wakes: it takes the SleepPlan the app returned,
// advances the virtual clock to the programmed timer instant (raw RTC time, so crystal error is
// honoured) plus a wake latency, applies scripted events that happen while the watch sleeps
// (battery voltage, steps, crystal ppm, USB, button presses, power loss) at their *true* UTC
// instants and wakes the app with the right reset reason / wake source. Nothing here asserts:
// scenario tests observe through `on_wake`, the DeviceApi and the fakes.
//
// Lives next to the tests (not in qz_testkit): it needs qz_app, and the testkit sits below it in
// the layering (docs/COMPONENTS.md). Header-only, test code.
#pragma once

#include "app_test_env.hpp"
#include "qz/power/power.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace qz::app::sim {

using namespace testenv;

constexpr std::int64_t kHourUs = 3600 * kUs;
constexpr std::int64_t kDayUs = 24 * kHourUs;

/// What the harness saw of one wake (after run_wake returned).
struct WakeObs {
    hal::ResetReason reason = hal::ResetReason::kDeepSleep;
    model::WakeCause cause = model::WakeCause::kUnknown;
    std::int64_t awake_us = 0;    ///< virtual RTC time from boot_rtc_us() to the sleep plan
    std::int64_t true_utc_us = 0; ///< ground-truth UTC when run_wake returned
    bool panel_updated = false;   ///< the EPD got a full or partial update during this wake
    bool full_update = false;
    hal::SleepPlan plan{};
};

/// Awake-time totals per wake cause (WP-22 "awake-time totals").
struct AwakeTotals {
    std::array<std::uint32_t, model::kWakeCauseCount> wakes{};
    std::array<std::int64_t, model::kWakeCauseCount> awake_us{};
    std::array<std::int64_t, model::kWakeCauseCount> max_awake_us{};
    void add(const WakeObs& obs) {
        const auto i = static_cast<std::size_t>(obs.cause);
        ++wakes[i];
        awake_us[i] += obs.awake_us;
        max_awake_us[i] = std::max(max_awake_us[i], obs.awake_us);
    }
    [[nodiscard]] std::int64_t total_awake_us() const {
        std::int64_t sum = 0;
        for (const std::int64_t v : awake_us) {
            sum += v;
        }
        return sum;
    }
    [[nodiscard]] std::uint32_t total_wakes() const {
        std::uint32_t sum = 0;
        for (const std::uint32_t v : wakes) {
            sum += v;
        }
        return sum;
    }
    [[nodiscard]] std::int64_t mean_awake_us(model::WakeCause cause) const {
        const auto i = static_cast<std::size_t>(cause);
        return wakes[i] == 0 ? 0 : awake_us[i] / wakes[i];
    }
    [[nodiscard]] std::uint32_t count(model::WakeCause cause) const {
        return wakes[static_cast<std::size_t>(cause)];
    }
};

/// Battery pin millivolts for a battery voltage (inverse of the 460/360 divider, ARCH section 11).
inline std::uint16_t pin_mv_for_battery(int battery_mv) {
    return static_cast<std::uint16_t>((battery_mv * 360 + 230) / 460);
}

class Sim {
public:
    explicit Sim(bool radio = true) : env(radio) {}

    Env env;
    hal::SleepPlan plan{};
    /// Boot latency of a timer wake after the programmed instant (the app's EWMA tracks it).
    std::int64_t wake_latency_us = 30'000;
    /// Called after every wake, before the harness sleeps again.
    std::function<void(Sim&, const WakeObs&)> on_wake;
    /// Battery voltage as a function of true UTC (nullopt = leave the pin alone), sampled just
    /// before every wake.
    std::function<std::optional<int>(std::int64_t)> battery_curve;
    AwakeTotals totals;
    std::uint32_t wakes = 0;

    // ---- scripting -------------------------------------------------------------------------

    /// Silent action at a true UTC instant while the watch sleeps (does not wake it).
    void act(std::int64_t at_utc_us, std::function<void(Sim&)> fn) {
        push({at_utc_us, Kind::kAct, std::move(fn), 0, 0});
    }
    void add_steps_at(std::int64_t at_utc_us, std::uint32_t n) {
        act(at_utc_us, [n](Sim& s) { s.env.accel.add_steps(n); });
    }
    void crystal_ppm_at(std::int64_t at_utc_us, int ppm) {
        act(at_utc_us, [ppm](Sim& s) { s.set_crystal_ppm(ppm); });
    }
    /// A button press of `hold_us` (needs the plan to have button wake enabled).
    void press_at(std::int64_t at_utc_us, std::uint8_t mask, std::int64_t hold_us = 150'000) {
        push({at_utc_us, Kind::kButton, nullptr, mask, hold_us});
    }
    /// USB attach at `at_utc_us`, console session until `at_utc_us + tethered_us`, then unplugged.
    void usb_session_at(std::int64_t at_utc_us, std::int64_t tethered_us) {
        push({at_utc_us, Kind::kUsb, nullptr, 0, tethered_us});
    }
    /// Battery removed: `off_us` of darkness, then a power-on boot (RTC memory is garbage, the RTC
    /// counter restarts, the BMA423 loses its config and counter).
    void power_loss_at(std::int64_t at_utc_us, std::int64_t off_us) {
        push({at_utc_us, Kind::kPowerLoss, nullptr, 0, off_us});
    }

    void set_crystal_ppm(int ppm) {
        ppb_ = static_cast<std::int64_t>(ppm) * 1000;
        env.clock.set_crystal_error_ppb(static_cast<std::int32_t>(ppb_));
    }
    [[nodiscard]] std::int64_t now_utc_us() const { return env.clock.true_utc_us(); }

    // ---- running ---------------------------------------------------------------------------

    /// Power-on at the current ground-truth time.
    void cold_boot() { wake(hal::ResetReason::kPowerOn, {}); }

    /// One wake right now (also the harness' own entry point; scenarios use it for setup re-plans).
    void wake(hal::ResetReason reason, const hal::WakeSources& sources) {
        if (battery_curve) {
            if (const std::optional<int> mv = battery_curve(now_utc_us())) {
                env.io.set_pin_mv(pin_mv_for_battery(*mv));
            }
        }
        const std::uint32_t updates_before = env.epd.full_updates() + env.epd.partial_updates();
        const std::uint32_t full_before = env.epd.full_updates();
        env.sys.set_wake(reason, sources);
        plan = env.app->run_wake();
        ++wakes;
        env.epd.clear_log(); // the command log grows without bound over a week otherwise

        WakeObs obs;
        obs.reason = reason;
        obs.plan = plan;
        obs.true_utc_us = now_utc_us();
        obs.awake_us = env.clock.rtc_us() - env.sys.boot_rtc_us();
        obs.panel_updated = env.epd.full_updates() + env.epd.partial_updates() != updates_before;
        obs.full_update = env.epd.full_updates() != full_before;
        console::DeviceApi& api = env.api();
        if (const std::size_t n = api.wake_record_count(); n > 0) {
            obs.cause = api.wake_record(n - 1).cause;
        }
        totals.add(obs);
        if (on_wake) {
            on_wake(*this, obs);
        }
    }

    /// Runs wakes until true UTC reaches `end_utc_us` (the final sleep ends exactly there).
    void run_until(std::int64_t end_utc_us) {
        // Events due exactly at (or just before) the end still fire.
        while (now_utc_us() < end_utc_us || has_event_until(end_utc_us)) {
            if (!sleep_step(end_utc_us)) {
                break;
            }
        }
    }

    /// Mean awake milliseconds per day over `days` -> estimated battery hours
    /// (power::estimate_hours with the [TUNE] defaults; only printed by the scenarios, never
    /// asserted).
    [[nodiscard]] std::uint32_t estimate_hours(double days) const {
        power::EstimateInputs in;
        in.awake_ms_per_day = static_cast<std::uint32_t>(
            static_cast<double>(totals.total_awake_us()) / 1000.0 / days);
        return power::estimate_hours(in);
    }

private:
    enum class Kind : std::uint8_t { kAct, kButton, kUsb, kPowerLoss };
    struct Ev {
        std::int64_t at_utc_us;
        Kind kind;
        std::function<void(Sim&)> fn;
        std::uint8_t mask;
        std::int64_t arg_us;
    };

    [[nodiscard]] bool has_event_until(std::int64_t utc_us) const {
        return std::ranges::any_of(events_,
                                   [utc_us](const Ev& e) { return e.at_utc_us <= utc_us; });
    }

    void push(Ev ev) { events_.push_back(std::move(ev)); }

    /// Index of the earliest pending event (insertion order breaks ties), or -1.
    [[nodiscard]] int next_event() const {
        int best = -1;
        for (std::size_t i = 0; i < events_.size(); ++i) {
            if (best < 0 ||
                events_[i].at_utc_us < events_[static_cast<std::size_t>(best)].at_utc_us) {
                best = static_cast<int>(i);
            }
        }
        return best;
    }

    /// True time at which the timer of the current plan fires (INT64_MAX = no timer).
    [[nodiscard]] std::int64_t timer_true_utc_us() const {
        if (plan.timer_us < 0) {
            return std::numeric_limits<std::int64_t>::max();
        }
        const std::int64_t rtc_left = target_rtc_us_ - env.clock.rtc_us();
        return now_utc_us() + (rtc_left * 1'000'000'000LL) / (1'000'000'000LL + ppb_);
    }

    void advance_true_to(std::int64_t utc_us) {
        if (utc_us > now_utc_us()) {
            env.clock.advance_us(utc_us - now_utc_us());
        }
    }

    /// Sleeps until the next wake (or `end`), then runs it. False when `end` was reached asleep.
    bool sleep_step(std::int64_t end_utc_us) {
        target_rtc_us_ = plan.timer_us >= 0 ? env.clock.rtc_us() + plan.timer_us : 0;
        for (;;) {
            const int idx = next_event();
            const std::int64_t timer_at = timer_true_utc_us();
            if (idx >= 0) {
                const Ev& head = events_[static_cast<std::size_t>(idx)];
                if (head.at_utc_us <= timer_at && head.at_utc_us <= end_utc_us) {
                    const Ev ev = head;
                    events_.erase(events_.begin() + idx);
                    advance_true_to(ev.at_utc_us);
                    if (fire(ev)) {
                        return true;
                    }
                    continue;
                }
            }
            if (timer_at <= end_utc_us) {
                env.clock.advance_rtc_us(std::max<std::int64_t>(
                    target_rtc_us_ + wake_latency_us - env.clock.rtc_us(), 0));
                wake(hal::ResetReason::kDeepSleep, timer_wake());
                return true;
            }
            advance_true_to(end_utc_us);
            return false;
        }
    }

    /// Applies one event; true when it woke the app (wake already ran).
    bool fire(const Ev& ev) {
        switch (ev.kind) {
            case Kind::kAct:
                ev.fn(*this);
                return false;
            case Kind::kButton: {
                if (!plan.wake_on_buttons) {
                    return false; // the press is not seen by a watch that cannot wake on it
                }
                env.io.press(ev.mask);
                env.sleep.at(env.clock.rtc_us() + ev.arg_us, 0, ev.mask);
                wake(hal::ResetReason::kDeepSleep, button_wake(ev.mask));
                return true;
            }
            case Kind::kUsb: {
                if (!plan.wake_on_usb) {
                    ADD_FAILURE() << "USB attach with the plan's USB wake disabled";
                    return false;
                }
                const std::int64_t detach_at = ev.at_utc_us + ev.arg_us;
                env.io.set_usb(true, true);
                env.console.hook = [this, detach_at](std::uint32_t) {
                    if (now_utc_us() >= detach_at) {
                        env.io.set_usb(false, false);
                    }
                };
                wake(hal::ResetReason::kDeepSleep, usb_wake());
                env.console.hook = nullptr;
                env.io.set_usb(false, false);
                return true;
            }
            case Kind::kPowerLoss:
                env.clock.advance_us(
                    ev.arg_us); // dark time: the RTC does not count it (reset below)
                env.rtc.scramble();
                env.clock.power_loss();
                env.accel.sensor_reset();
                wake(hal::ResetReason::kPowerOn, {});
                return true;
        }
        return false;
    }

    std::vector<Ev> events_;
    std::int64_t ppb_ = 0;
    std::int64_t target_rtc_us_ = 0;
};

/// Off-mode style start: power-on with the ground-truth clock at `utc_us`, the time set manually
/// (console path) and one quiet re-plan so `plan` is a real minute-tick plan.
inline void boot_manual(Sim& sim, std::int64_t utc_us, std::string_view zone = {}) {
    sim.cold_boot(); // the boot (full refresh) takes virtual time: set the ground truth after it
    if (!zone.empty()) {
        // Zone first: the step day is derived from the local date when the time is first set.
        EXPECT_TRUE(static_cast<bool>(sim.env.api().apply_setting(settings::Key::kTimeZone, zone)));
    }
    sim.env.clock.set_true_utc_us(utc_us);
    EXPECT_TRUE(static_cast<bool>(sim.env.api().set_time_utc(utc_us / kUs)));
    sim.env.clock.advance_rtc_us(1000);
    sim.wake(hal::ResetReason::kDeepSleep, timer_wake());
}

/// UTC microseconds of a civil UTC date and time.
inline std::int64_t utc_us_of(int year, int month, int day, int hour = 0, int minute = 0) {
    const time::CivilDate date{
        year, static_cast<std::uint8_t>(month), static_cast<std::uint8_t>(day)};
    return (static_cast<std::int64_t>(time::days_from_civil(date)) * 86'400 + hour * 3600 +
            minute * 60) *
           kUs;
}

/// Selects the IANA zone through the same path as the settings menu (works after the first wake).
inline void set_timezone(Sim& sim, std::string_view name) {
    EXPECT_TRUE(static_cast<bool>(sim.env.api().apply_setting(settings::Key::kTimeZone, name)));
}

/// Hand-computed local civil minute-of-day offset check helper: minutes since local midnight of
/// the instant `utc_us` in a zone with the given UTC offset in seconds.
inline int local_minute_of_day(std::int64_t utc_us, std::int32_t offset_s) {
    const std::int64_t local_s = utc_us / kUs + offset_s;
    return static_cast<int>(((local_s % 86400) + 86400) % 86400 / 60);
}

/// Minute-flip bookkeeping for timer wakes with a valid clock: how late (after the true minute
/// boundary) each panel update completed and whether the face on the panel is the face of the
/// true minute (checked every `stride`-th update: rendering a reference face is the expensive
/// part).
struct FlipStats {
    std::uint32_t updates = 0;
    std::uint32_t frames_checked = 0;
    std::uint32_t frame_mismatches = 0;
    std::int64_t min_late_us = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_late_us = std::numeric_limits<std::int64_t>::min();
    /// Partial updates after the wake-ahead EWMA has converged / full refreshes (flash waveform).
    std::int64_t max_late_partial_us = std::numeric_limits<std::int64_t>::min();
    std::int64_t max_late_full_us = std::numeric_limits<std::int64_t>::min();
};

inline void observe_flip(Sim& sim, const WakeObs& obs, FlipStats& st, std::uint32_t stride) {
    if (obs.cause != model::WakeCause::kTimer || !obs.panel_updated ||
        !sim.env.api().time_info().valid) {
        return;
    }
    // A wake that ran a radio session returns long after its panel update (display comes first,
    // ARCH section 4): only the face is checked, not the completion time.
    const console::DeviceApi& api = sim.env.api();
    const bool radio =
        (api.wake_record(api.wake_record_count() - 1).flags & model::kWakeFlagRadio) != 0;
    const std::int64_t boundary = obs.true_utc_us / kMin * kMin;
    const std::int64_t late = obs.true_utc_us - boundary;
    ++st.updates;
    if (!radio) {
        st.min_late_us = std::min(st.min_late_us, late);
        st.max_late_us = std::max(st.max_late_us, late);
        if (obs.full_update) {
            st.max_late_full_us = std::max(st.max_late_full_us, late);
        } else if (st.updates > 8) { // wake-ahead EWMA has converged (ARCH 8.4)
            st.max_late_partial_us = std::max(st.max_late_partial_us, late);
        }
    }
    if (stride != 0 && st.updates % stride == 0) {
        ++st.frames_checked;
        if (!same_frame(sim.env.epd.displayed(), expected_face(sim.env, boundary / kUs))) {
            ++st.frame_mismatches;
        }
    }
}

} // namespace qz::app::sim
