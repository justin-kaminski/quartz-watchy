// App facade (ARCHITECTURE.md section 3): fixed storage for the Core, pure entry point
// run_wake() -> SleepPlan.
#include "app_core.hpp"

#include <new>

namespace qz::app {

Core::Core(Platform& platform, const BuildFeatures& features, TetherPolicy& tether) noexcept
    : p_(platform), f_(features), tether_(tether),
      store_(platform.rtc_memory.state_region(), platform.rtc_memory.frame_region()),
      keeper_(rtc_.time, platform.clock), steps_(rtc_.steps), step_store_(platform.kv),
      power_(rtc_.power, power::Thresholds{}),
      // Backoff jitter is "seeded per device" (ARCHITECTURE.md section 12): fold the chip id.
      sched_(rtc_.conn,
             static_cast<std::uint32_t>(platform.system.chip_id() ^
                                        (platform.system.chip_id() >> 32U))),
      settings_store_(platform.kv), cred_store_(platform.kv), planner_(rtc_.wake, keeper_),
      panel_(platform.epd), accel_(platform.accel, platform.delay), ui_(faces_),
      prov_(platform.system, *this),
      dispatcher_(registry_, *this, features.radio && platform.net != nullptr) {
    if (platform.net != nullptr && features.radio) {
        session_.emplace(*platform.net, weather_provider_, platform.clock);
    }
    const Status registered = console::register_builtin_commands(registry_);
    QZ_ASSERT(registered.has_value()); // a duplicate/overfull catalog is a programmer error
}

App::App(Platform& platform, const BuildFeatures& features) noexcept
    : platform_(platform), features_(features) {
    static_assert(sizeof(Core) <= kCoreStorageBytes, "grow App::kCoreStorageBytes");
    static_assert(alignof(Core) <= alignof(std::max_align_t));
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-prefer-member-initializer)
    core_ = ::new (static_cast<void*>(core_storage_.data())) Core(platform_, features_, tether_);
}

App::~App() {
    core_->~Core();
}

hal::SleepPlan App::run_wake() noexcept {
    return core_->run_wake();
}

console::DeviceApi& App::device_api() noexcept {
    return *core_;
}

const TetherPolicy& App::tether() const noexcept {
    return tether_;
}

} // namespace qz::app
