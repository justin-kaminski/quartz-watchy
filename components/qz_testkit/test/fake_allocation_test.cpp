// The fakes that sit on the wake path (clock, board IO, RTC memory, sleep system) must never touch
// the heap: the virtual-time suite counts allocations on the minute path (ARCHITECTURE.md section
// 2) and must not be charged for its test doubles.
//
// Allocations are counted through the AddressSanitizer allocator hook, so the check is active in
// the default (ASan + UBSan) host build and skips itself where the hook is never called.
#include "qz/testkit/fakes.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

std::atomic<std::size_t>& allocation_counter() {
    static std::atomic<std::size_t> counter{0};
    return counter;
}

} // namespace

// Called by the ASan runtime after every allocation (malloc, operator new, ...); a strong
// definition here overrides the runtime's weak empty one.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,readability-identifier-naming)
extern "C" void __sanitizer_malloc_hook(const volatile void* /*ptr*/, std::size_t /*size*/) {
    allocation_counter().fetch_add(1, std::memory_order_relaxed);
}

namespace qz::testkit {
namespace {

std::size_t allocations_so_far() {
    return allocation_counter().load(std::memory_order_relaxed);
}

/// True if the allocator hook is live (ASan build): a probe allocation must be counted.
bool allocation_counting_works() {
    const std::size_t before = allocations_so_far();
    auto* probe = new std::uint8_t[64];              // NOLINT(cppcoreguidelines-owning-memory)
    *static_cast<volatile std::uint8_t*>(probe) = 1; // keep the allocation observable
    const std::size_t after = allocations_so_far();
    delete[] probe; // NOLINT(cppcoreguidelines-owning-memory)
    return after > before;
}

TEST(FakeAllocation, WakePathFakesNeverAllocate) {
    if (!allocation_counting_works()) {
        GTEST_SKIP() << "allocation counting needs the AddressSanitizer allocator hook";
    }
    VirtualClock clock;
    FakeBoardIo io(clock);
    FakeRtcMemory rtc;
    FakeSleepSystem sleep(clock);

    const std::size_t before = allocations_so_far();

    // One simulated minute tick plus a button session, as the app would drive the fakes.
    clock.set_crystal_error_ppb(20'000);
    clock.set_true_utc_us(1'760'000'000'000'000);
    clock.advance_us(350'000);
    clock.delay_ms(15);
    clock.delay_us(250);
    clock.set_system_utc_us(clock.true_utc_us());
    (void)clock.rtc_us();
    (void)clock.system_utc_us();
    (void)clock.elapsed_us();

    io.set_usb(true, true);
    io.press(0x05);
    io.release(0x01);
    (void)io.pressed_buttons();
    (void)io.usb_present();
    (void)io.charging();
    io.set_pin_mv(2900);
    (void)io.read_pin_mv();
    io.set_vibration(true);
    clock.delay_ms(15);
    io.set_vibration(false);
    (void)io.vibration_ms_total();

    rtc.state_region()[0] = 0xAB;
    (void)rtc.frame_region();
    rtc.scramble();

    hal::SleepPlan plan;
    plan.timer_us = 59'000'000;
    sleep.set_wake(hal::ResetReason::kDeepSleep, {});
    (void)sleep.light_sleep(plan);
    sleep.deep_sleep(plan);
    (void)sleep.last_plan();
    (void)sleep.reset_reason();
    (void)sleep.wake_sources();
    (void)sleep.boot_rtc_us();
    (void)sleep.slow_clock();
    (void)sleep.firmware();
    (void)sleep.chip_id();
    (void)sleep.random_u32();
    (void)sleep.heap();
    clock.advance_rtc_us(plan.timer_us);
    sleep.restart();

    const std::size_t after = allocations_so_far();
    EXPECT_EQ(after - before, 0U);
}

} // namespace
} // namespace qz::testkit
