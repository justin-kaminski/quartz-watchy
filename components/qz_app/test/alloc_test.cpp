// Zero heap allocations on the steady-state wake path (ARCHITECTURE.md section 2): a minute tick,
// and a button session, counted through the AddressSanitizer allocator hook (same technique as
// qz_testkit's fake_allocation_test). The hook is defined here only: this is the single test of
// the qz_app binary that may define __sanitizer_malloc_hook.
//
// Fakes that legitimately allocate (EPD command log, BMA423 model) are wrapped in shims that pause
// counting while the *fake* runs, so only the application's own allocations are charged. The KV
// store is not wrapped: the minute path must not touch NVS at all.
#include "app_test_env.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>

// Test arithmetic mixes int literals with int64 microsecond constants and gtest macros inflate
// function complexity: those checks are relaxed for this file only.
// NOLINTBEGIN(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
namespace {

std::atomic<std::size_t> g_allocations{0};
std::atomic<int> g_pause{0};
std::atomic<bool> g_counting{false};

} // namespace

// Called by the ASan runtime after every allocation; overrides its weak empty definition.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" void __sanitizer_malloc_hook(const volatile void* /*ptr*/, std::size_t /*size*/) {
    if (g_counting.load(std::memory_order_relaxed) &&
        g_pause.load(std::memory_order_relaxed) == 0) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace qz::app {
namespace {

using namespace testenv;

struct Pause {
    Pause() { g_pause.fetch_add(1); }
    ~Pause() { g_pause.fetch_sub(1); }
    Pause(const Pause&) = delete;
    Pause& operator=(const Pause&) = delete;
};

class EpdShim final : public hal::EpdBus {
public:
    explicit EpdShim(testkit::FakeEpdPanel& inner) : inner_(inner) {}
    Status hardware_reset() override {
        Pause p;
        return inner_.hardware_reset();
    }
    Status command(std::uint8_t cmd) override {
        Pause p;
        return inner_.command(cmd);
    }
    Status data(std::span<const std::uint8_t> bytes) override {
        Pause p;
        return inner_.data(bytes);
    }
    Status read(std::span<std::uint8_t> out) override {
        Pause p;
        return inner_.read(out);
    }
    [[nodiscard]] bool busy() const override {
        Pause p;
        return inner_.busy();
    }
    Status wait_idle(std::uint32_t timeout_ms) override {
        Pause p;
        return inner_.wait_idle(timeout_ms);
    }

private:
    testkit::FakeEpdPanel& inner_;
};

class I2cShim final : public hal::I2cDevice {
public:
    explicit I2cShim(testkit::FakeBma423& inner) : inner_(inner) {}
    Status read_registers(std::uint8_t reg, std::span<std::uint8_t> out) override {
        Pause p;
        return inner_.read_registers(reg, out);
    }
    Status write_registers(std::uint8_t reg, std::span<const std::uint8_t> data) override {
        Pause p;
        return inner_.write_registers(reg, data);
    }

private:
    testkit::FakeBma423& inner_;
};

bool counting_works() {
    g_allocations = 0;
    g_counting = true;
    auto* probe = new std::uint8_t[64]; // NOLINT(cppcoreguidelines-owning-memory)
    *static_cast<volatile std::uint8_t*>(probe) = 1;
    const bool seen = g_allocations.load() > 0;
    delete[] probe; // NOLINT(cppcoreguidelines-owning-memory)
    g_counting = false;
    return seen;
}

struct AllocEnv {
    testkit::VirtualClock clock;
    testkit::FakeEpdPanel epd{clock};
    testkit::FakeBma423 accel{clock};
    EpdShim epd_shim{epd};
    I2cShim accel_shim{accel};
    testkit::FakeBoardIo io{clock};
    testkit::FakeKvStore kv;
    testkit::FakeRtcMemory rtc;
    testkit::FakeSleepSystem sys{clock};
    ScriptedSleep sleep{sys, clock, io};
    testkit::FakeConsolePort console{clock};
    Platform platform{
        epd_shim, accel_shim, clock, io, io, kv, rtc, clock, sleep, sys, console, nullptr, nullptr};
    BuildFeatures features{.radio = false};
    std::unique_ptr<App> app;

    AllocEnv() {
        io.set_pin_mv(2900);
        sleep.record_plans = false;
        app = std::make_unique<App>(platform, features);
    }
};

TEST(AppAllocation, MinuteTickAndButtonSessionNeverAllocate) {
    if (!counting_works()) {
        GTEST_SKIP() << "allocation counting needs the AddressSanitizer allocator hook";
    }
    AllocEnv env;
    env.sys.set_wake(hal::ResetReason::kPowerOn, {});
    (void)env.app->run_wake(); // cold boot (NVS reads allocate inside the fake: not measured)
    env.clock.set_true_utc_us(kT0Utc);
    ASSERT_TRUE(static_cast<bool>(env.app->device_api().set_time_utc(kT0Utc / kUs)));
    env.clock.advance_rtc_us(1000);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
    hal::SleepPlan plan = env.app->run_wake();

    const std::uint32_t kv_writes = env.kv.write_count();
    // Steady state: five minute ticks.
    g_allocations = 0;
    g_counting = true;
    for (int i = 0; i < 5; ++i) {
        env.clock.advance_rtc_us(plan.timer_us + 30'000);
        env.sys.set_wake(hal::ResetReason::kDeepSleep, timer_wake());
        plan = env.app->run_wake();
    }
    g_counting = false;
    EXPECT_EQ(g_allocations.load(), 0U) << "heap allocations on the minute-tick path";
    EXPECT_EQ(env.kv.write_count(), kv_writes);
    EXPECT_GE(env.epd.partial_updates(), 5U);

    // A button session (menu, idle timeout, full refresh back to the face).
    env.clock.advance_rtc_us(10 * kUs);
    env.io.press(hal::kButtonBitMenu);
    env.sleep.at(env.clock.rtc_us() + 100'000, 0, hal::kButtonBitMenu);
    env.sys.set_wake(hal::ResetReason::kDeepSleep, button_wake(hal::kButtonBitMenu));
    g_allocations = 0;
    g_counting = true;
    plan = env.app->run_wake();
    g_counting = false;
    EXPECT_EQ(g_allocations.load(), 0U) << "heap allocations in a button session";
    EXPECT_EQ(env.epd.violation_count(), 0U);
}

} // namespace
} // namespace qz::app
// NOLINTEND(readability-function-cognitive-complexity,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,misc-const-correctness,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-special-member-functions)
