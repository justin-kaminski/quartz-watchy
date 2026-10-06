// FakeSleepSystem: records sleep plans and answers the hal::System queries deterministically.
// See the semantics block on the class in fakes.hpp.
#include "qz/core/assert.hpp"
#include "qz/hal/system.hpp"
#include "qz/testkit/fakes.hpp"

#include <cstdint>
#include <optional>

namespace qz::testkit {
namespace {

// Fixed identity of the fake watch: version strings from the header contract, and a made-up
// base MAC (Espressif OUI 24:0A:C4 + arbitrary bytes). No real device id is involved.
constexpr hal::FirmwareInfo kFirmware{
    .version = "0.0.0-host", .git_hash = "host", .idf_version = "host"};
constexpr std::uint64_t kChipId = 0x240AC4A1B2C3ULL;

bool has_deep_sleep_wake_source(const hal::SleepPlan& plan) noexcept {
    return plan.timer_us >= 0 || plan.wake_on_buttons || plan.wake_on_accel || plan.wake_on_usb;
}

} // namespace

FakeSleepSystem::FakeSleepSystem(VirtualClock& clock)
    : clock_(&clock), boot_rtc_us_(clock.rtc_us()) {}

void FakeSleepSystem::deep_sleep(const hal::SleepPlan& plan) {
    // A plan that nothing can end would leave the real watch dead until a hardware reset.
    QZ_ASSERT(has_deep_sleep_wake_source(plan));
    last_deep_plan_ = plan;
    ++deep_sleeps_;
}

hal::LightSleepWake FakeSleepSystem::light_sleep(const hal::SleepPlan& plan) {
    // The timer is the only event source of this fake; without one the call could never return.
    QZ_ASSERT(plan.timer_us >= 0);
    last_light_plan_ = plan;
    ++light_sleeps_;
    clock_->advance_rtc_us(plan.timer_us);
    return hal::LightSleepWake::kTimer;
}

hal::ResetReason FakeSleepSystem::reset_reason() const {
    return reset_reason_;
}

hal::WakeSources FakeSleepSystem::wake_sources() const {
    return wake_sources_;
}

std::int64_t FakeSleepSystem::boot_rtc_us() const {
    return boot_rtc_us_;
}

hal::SlowClockInfo FakeSleepSystem::slow_clock() const {
    return slow_clock_;
}

hal::FirmwareInfo FakeSleepSystem::firmware() const {
    return kFirmware;
}

std::uint64_t FakeSleepSystem::chip_id() const {
    return kChipId;
}

std::uint32_t FakeSleepSystem::random_u32() {
    // Marsaglia xorshift32 (13, 17, 5): full period 2^32 - 1 from any non-zero state.
    std::uint32_t x = random_state_;
    x ^= x << 13U;
    x ^= x >> 17U;
    x ^= x << 5U;
    random_state_ = x;
    return x;
}

hal::HeapInfo FakeSleepSystem::heap() const {
    return heap_;
}

void FakeSleepSystem::restart() {
    ++restarts_;
}

std::optional<hal::SleepPlan> FakeSleepSystem::last_plan() const {
    return last_deep_plan_;
}

void FakeSleepSystem::set_wake(hal::ResetReason reason, hal::WakeSources sources) {
    reset_reason_ = reason;
    wake_sources_ = sources;
    boot_rtc_us_ = clock_->rtc_us();
}

std::optional<hal::SleepPlan> FakeSleepSystem::last_light_sleep_plan() const {
    return last_light_plan_;
}

std::uint32_t FakeSleepSystem::deep_sleep_count() const {
    return deep_sleeps_;
}

std::uint32_t FakeSleepSystem::light_sleep_count() const {
    return light_sleeps_;
}

std::uint32_t FakeSleepSystem::restart_count() const {
    return restarts_;
}

void FakeSleepSystem::set_slow_clock(hal::SlowClockInfo info) {
    slow_clock_ = info;
}

void FakeSleepSystem::set_heap(hal::HeapInfo info) {
    heap_ = info;
}

} // namespace qz::testkit
