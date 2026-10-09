// Shared environment of the App integration tests (WP-21): every hal fake wired into a Platform,
// a scripted light sleep (button / USB events at virtual times), a scripted console and a fake
// provisioning portal, plus helpers that "sleep" and "wake" the way the harness of ARCHITECTURE.md
// section 3 does (advance the virtual RTC by the plan, set the wake cause, call run_wake).
#pragma once

#include "qz/app/app.hpp"
#include "qz/app/rtc_state.hpp"
#include "qz/board/watchy_v3.hpp"
#include "qz/conn/conn.hpp"
#include "qz/console/registry.hpp"
#include "qz/faces/registry.hpp"
#include "qz/gfx/framebuffer.hpp"
#include "qz/settings/settings.hpp"
#include "qz/testkit/fakes.hpp"
#include "qz/ui/ui.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qz::app::testenv {

constexpr std::int64_t kUs = 1'000'000;
constexpr std::int64_t kMin = 60 * kUs;
/// 2027-01-15 08:00:00Z (a Friday), minute- and 5-minute aligned.
constexpr std::int64_t kT0Utc = 1'800'000'000LL * kUs;

struct ScriptEvent {
    std::int64_t at_rtc_us = 0;
    std::uint8_t press = 0;
    std::uint8_t release = 0;
    int usb = -1; ///< -1 none, 0 unplug, 1 plug
};

/// Light sleep that applies scripted pin changes at their virtual times. Deep sleep is forwarded
/// to the FakeSleepSystem (which records the plan).
class ScriptedSleep final : public hal::SleepControl {
public:
    ScriptedSleep(testkit::FakeSleepSystem& sys,
                  testkit::VirtualClock& clock,
                  testkit::FakeBoardIo& io)
        : sys_(sys), clock_(clock), io_(io) {}

    void deep_sleep(const hal::SleepPlan& plan) override { sys_.deep_sleep(plan); }

    hal::LightSleepWake light_sleep(const hal::SleepPlan& plan) override {
        if (record_plans) {
            light_plans.push_back(plan);
        }
        if (on_light_sleep) {
            on_light_sleep(plan);
        }
        const std::int64_t start = clock_.rtc_us();
        const std::int64_t target =
            plan.timer_us >= 0 ? start + plan.timer_us : std::numeric_limits<std::int64_t>::max();
        for (;;) {
            const auto next = std::ranges::min_element(
                events_, {}, [](const ScriptEvent& e) { return e.at_rtc_us; });
            if (next == events_.end() || next->at_rtc_us > target) {
                break;
            }
            const ScriptEvent ev = *next;
            events_.erase(next);
            if (ev.at_rtc_us > clock_.rtc_us()) {
                clock_.advance_rtc_us(ev.at_rtc_us - clock_.rtc_us());
            }
            apply(ev);
            if (plan.wake_on_buttons && (ev.press != 0 || ev.release != 0)) {
                return hal::LightSleepWake::kButton;
            }
            if (plan.wake_on_usb && ev.usb >= 0) {
                return hal::LightSleepWake::kUsb;
            }
        }
        if (plan.timer_us < 0) {
            ADD_FAILURE() << "light sleep without timer and without a scripted event would hang";
            return hal::LightSleepWake::kTimer;
        }
        if (target > clock_.rtc_us()) {
            clock_.advance_rtc_us(target - clock_.rtc_us());
        }
        return hal::LightSleepWake::kTimer;
    }

    /// Press/release buttons at an absolute RTC time.
    void at(std::int64_t rtc_us, std::uint8_t press, std::uint8_t release = 0) {
        events_.push_back({rtc_us, press, release, -1});
    }
    void usb_at(std::int64_t rtc_us, bool present) {
        events_.push_back({rtc_us, 0, 0, present ? 1 : 0});
    }
    [[nodiscard]] std::size_t pending_events() const { return events_.size(); }

    std::vector<hal::SleepPlan> light_plans;
    bool record_plans = true; ///< off in the allocation test (a vector push would be charged)
    /// Called at the start of every light sleep (tests observe the app between inputs).
    std::function<void(const hal::SleepPlan&)> on_light_sleep;

private:
    void apply(const ScriptEvent& ev) {
        if (ev.press != 0) {
            io_.press(ev.press);
        }
        if (ev.release != 0) {
            io_.release(ev.release);
        }
        if (ev.usb >= 0) {
            io_.set_usb(ev.usb == 1, ev.usb == 1);
        }
    }

    testkit::FakeSleepSystem& sys_;
    testkit::VirtualClock& clock_;
    testkit::FakeBoardIo& io_;
    std::vector<ScriptEvent> events_;
};

/// Console port decorator: counts receive_line calls and lets a test act on each one (unplug USB,
/// push the next request, ...).
class ScriptedConsole final : public hal::ConsolePort {
public:
    explicit ScriptedConsole(testkit::FakeConsolePort& port) : port_(port) {}
    Status start() override { return port_.start(); }
    void stop() override { port_.stop(); }
    Result<std::size_t> receive_line(std::span<char> out, std::uint32_t timeout_ms) override {
        ++receives;
        if (hook) {
            hook(receives);
        }
        return port_.receive_line(out, timeout_ms);
    }
    void send_line(std::string_view line) override { port_.send_line(line); }

    std::function<void(std::uint32_t)> hook;
    std::uint32_t receives = 0;

private:
    testkit::FakeConsolePort& port_;
};

/// ProvisioningPortal fake: start() stores the handler, poll() submits a queued form body.
class FakePortal final : public hal::ProvisioningPortal {
public:
    Status
    start(std::string_view ssid, const Secret<64>& password, hal::PortalHandler& handler) override {
        ++starts;
        (void)ssid_.assign(ssid); // test fake: the SSID always fits
        password_ = password;
        handler_ = &handler;
        running = true;
        return ok();
    }
    Status poll(std::uint32_t /*timeout_ms*/) override {
        ++polls;
        if (handler_ != nullptr && !pending_form.empty()) {
            const std::string body = std::move(pending_form);
            pending_form.clear();
            last_submit = handler_->submit(body);
        }
        return ok();
    }
    void stop() override {
        ++stops;
        running = false;
        handler_ = nullptr;
    }
    [[nodiscard]] std::string_view ssid() const { return ssid_.view(); }
    [[nodiscard]] std::string password() const { return std::string(password_.reveal()); }

    std::string pending_form;
    Result<std::string_view> last_submit = Error{Errc::kInternal};
    std::uint32_t starts = 0, polls = 0, stops = 0;
    bool running = false;

private:
    FixedString<32> ssid_;
    Secret<64> password_;
    hal::PortalHandler* handler_ = nullptr;
};

inline hal::WakeSources timer_wake() {
    hal::WakeSources s;
    s.timer = true;
    return s;
}

inline hal::WakeSources button_wake(std::uint8_t mask) {
    hal::WakeSources s;
    s.ext1 = true;
    for (std::size_t i = 0; i < board::kButtonPins.size(); ++i) {
        if ((mask & (1U << i)) != 0) {
            s.ext1_pins |= std::uint64_t{1} << board::kButtonPins[i];
        }
    }
    return s;
}

inline hal::WakeSources accel_wake() {
    hal::WakeSources s;
    s.ext1 = true;
    s.ext1_pins = std::uint64_t{1} << board::kAccelInt1;
    return s;
}

inline hal::WakeSources usb_wake() {
    hal::WakeSources s;
    s.ext0 = true;
    return s;
}

struct Env {
    testkit::VirtualClock clock;
    testkit::FakeEpdPanel epd{clock};
    testkit::FakeBma423 accel{clock};
    testkit::FakeBoardIo io{clock};
    testkit::FakeKvStore kv;
    testkit::FakeRtcMemory rtc;
    testkit::FakeSleepSystem sys{clock};
    ScriptedSleep sleep{sys, clock, io};
    testkit::FakeConsolePort console_port{clock};
    ScriptedConsole console{console_port};
    testkit::FakeNetStack net{clock};
    FakePortal portal;
    testkit::FakePhoneLink phone{clock};
    Platform platform;
    BuildFeatures features;
    std::unique_ptr<App> app;

    explicit Env(bool radio = true, hal::NetStack* custom_net = nullptr)
        : platform{epd,
                   accel,
                   clock,
                   io,
                   io,
                   kv,
                   rtc,
                   clock,
                   sleep,
                   sys,
                   console,
                   radio ? (custom_net != nullptr ? custom_net : static_cast<hal::NetStack*>(&net))
                         : nullptr,
                   radio ? static_cast<hal::ProvisioningPortal*>(&portal) : nullptr} {
        io.set_pin_mv(2900); // 3722 mV behind the divider: a healthy cell, Normal power level
        features.radio = radio;
        app = std::make_unique<App>(platform, features);
    }

    console::DeviceApi& api() { return app->device_api(); }

    /// Phone link compiled in (BuildFeatures.phone + a link). Off by default so menu-navigation
    /// tests keep their item positions.
    void enable_phone() {
        platform.phone = &phone;
        features.phone = true;
        app = std::make_unique<App>(platform, features);
    }

    /// Power-on: RTC memory is garbage, the RTC counter restarts.
    hal::SleepPlan cold_boot() {
        sys.set_wake(hal::ResetReason::kPowerOn, {});
        return app->run_wake();
    }

    /// Deep sleep per `plan` (timer only), then a timer wake `latency_us` after the programmed
    /// instant.
    hal::SleepPlan wake_timer(const hal::SleepPlan& plan, std::int64_t latency_us = 30'000) {
        EXPECT_GE(plan.timer_us, 0) << "plan has no timer";
        clock.advance_rtc_us(std::max<std::int64_t>(plan.timer_us, 0) + latency_us);
        sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
        return app->run_wake();
    }

    /// Sleeps `elapsed_us`, then wakes on `sources` (buttons are pressed by the caller).
    hal::SleepPlan wake_on(const hal::WakeSources& sources,
                           std::int64_t elapsed_us,
                           hal::ResetReason reason = hal::ResetReason::kDeepSleep) {
        clock.advance_rtc_us(elapsed_us);
        sys.set_wake(reason, sources);
        return app->run_wake();
    }

    /// Cold boot and set the wall clock (console path), leaving the watch showing `utc_us`.
    hal::SleepPlan boot_with_time(std::int64_t utc_us = kT0Utc) {
        (void)cold_boot();
        clock.set_true_utc_us(utc_us);
        EXPECT_TRUE(static_cast<bool>(api().set_time_utc(utc_us / kUs)));
        // Re-plan through a quiet timer-less wake so the caller gets a real plan.
        return wake_on(timer_wake(), 1000);
    }

    [[nodiscard]] std::int64_t utc_now_us() const { return clock.true_utc_us(); }
};

/// The face the watch must show for local time of `utc_s` (independent of the app's builder:
/// assembled from the DeviceApi getters).
inline gfx::Framebuffer expected_face(Env& env, time::UnixSeconds utc_s, bool time_valid = true) {
    console::DeviceApi& api = env.api();
    const settings::Settings& cfg = api.current_settings();
    ui::WatchState st;
    st.time_valid = time_valid;
    if (time_valid) {
        st.local = api.timezone().to_local(utc_s);
    }
    st.hour_format = cfg.hour_format;
    st.tz_label = api.timezone().abbreviation(st.local.is_dst);
    st.steps = api.steps();
    st.battery = api.battery();
    const console::WeatherInfo wx = api.weather();
    st.weather = wx.report;
    st.weather_freshness = wx.freshness;
    st.weather_age_s = wx.age_s;
    st.temp_unit = cfg.temp_unit;
    st.weather_high_low = cfg.weather_high_low;
    const console::SyncInfo sync = api.sync_info();
    st.sync = sync.indicator;
    st.conn_mode = env.features.radio ? cfg.connectivity : model::ConnectivityMode::kOff;
    st.radio_available = env.features.radio;
    st.has_credentials = api.wifi_ssid().has_value();
    st.last_sync_utc = api.time_info().last_sync_utc;
    st.power = st.battery.level;
    const console::FirmwareIdentity fw = api.firmware();
    st.fw_version = fw.version;
    st.git_hash = fw.git_hash;
    st.idf_version = fw.idf_version;
    st.drift_ppb = api.time_info().drift_ppb;
    st.settings = &cfg;
    gfx::Framebuffer fb;
    gfx::Canvas canvas(fb);
    faces::Registry registry;
    registry.render(cfg.face_id, st, canvas);
    return fb;
}

inline bool same_frame(const gfx::Framebuffer& a, const gfx::Framebuffer& b) {
    return a.bits == b.bits;
}

/// Stores credentials and the connectivity mode in NVS before the first boot.
inline void
provision_nvs(Env& env, model::ConnectivityMode mode, bool creds = true, bool location = false) {
    settings::Settings cfg = settings::defaults();
    cfg.connectivity = mode;
    if (location) {
        cfg.location_set = true;
        cfg.location = model::Location{4'000'000, -7'400'000};
    }
    ASSERT_TRUE(static_cast<bool>(settings::SettingsStore(env.kv).save(cfg, settings::defaults())));
    if (creds) {
        hal::WifiCredentials c;
        ASSERT_TRUE(c.ssid.assign("HomeNet"));
        ASSERT_TRUE(c.password.assign("correct-horse"));
        ASSERT_TRUE(static_cast<bool>(settings::CredentialStore(env.kv).save(c)));
    }
}

inline app::RtcState load_rtc(Env& env) {
    app::RtcState state;
    const app::RtcStore store(env.rtc.state_region(), env.rtc.frame_region());
    EXPECT_TRUE(static_cast<bool>(store.load(state)));
    return state;
}

inline std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

} // namespace qz::app::testenv
