// Quartz firmware entry point (ARCHITECTURE.md sections 3 and 4).
//
// Every boot is a wake: build the platform, run one complete wake (the tethered loop included),
// deep sleep. Deep sleep never returns; the chip reboots into app_main on the next wake.
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "qz/app/app.hpp"
#include "qz/app/rtc_state.hpp"
#include "qz/net/idf_net.hpp"
#include "qz/platform/idf_platform.hpp"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "quartz";

// The RTC regions are fixed in the platform component (it cannot see qz_app). A grown struct must
// fail the build here, not corrupt state at run time.
static_assert(
    sizeof(qz::app::RtcState) <= qz::platform::kRtcStateRegionBytes,
    "app::RtcState outgrew qz::platform::kRtcStateRegionBytes: bump it (and kRtcStateVersion)");
static_assert(sizeof(qz::app::FrameShadow) <= qz::platform::kRtcFrameRegionBytes,
              "app::FrameShadow outgrew qz::platform::kRtcFrameRegionBytes");

// Kconfig bools are defined (as 1) when enabled and absent when disabled.
#ifdef CONFIG_QZ_RADIO
constexpr bool kRadioCompiled = true;
#else
constexpr bool kRadioCompiled = false;
#endif
#ifdef CONFIG_QZ_USB_WAKE
constexpr bool kUsbWake = true;
#else
constexpr bool kUsbWake = false;
#endif
#ifdef CONFIG_QZ_SELFTEST_INTERACTIVE
constexpr bool kSelftestInteractive = true;
#else
constexpr bool kSelftestInteractive = false;
#endif

/// Retry period after a failed platform bring-up (a broken board or a transient fault): deep sleep
/// costs microamps, a reboot loop would drain the cell in hours.
constexpr std::int64_t kInitRetryUs = 60'000'000;

/// With rollback enabled an OTA-installed image stays "pending verify" until the app confirms it;
/// the next reset would roll back. Called after the first wake completed. Images flashed over USB
/// have no pending state, so nothing happens (and nothing is logged) for them. The otadata read is
/// skipped on ordinary timer wakes (the state cannot change between deep-sleep wakes).
void confirm_image_after_cold_boot(qz::hal::ResetReason reason) noexcept {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (reason == qz::hal::ResetReason::kDeepSleep) {
        return;
    }
    // [IDF:components/app_update/esp_ota_ops.c esp_ota_get_state_partition returns NOT_SUPPORTED
    // for a non-OTA (factory) partition and NOT_FOUND without otadata; esp_ota_mark_app_valid_*
    // logs an error and fails in those cases, hence the PENDING_VERIFY gate]
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "could not confirm the running image (%s)", esp_err_to_name(err));
    }
#else
    (void)reason;
#endif
}

} // namespace

extern "C" void app_main() {
    using namespace qz;

    // 1. Platform first: captures the boot RTC time, reset reason and wake sources, releases the
    //    deep-sleep pad holds and brings up GPIO, SPI and I2C.
    const Result<platform::IdfPlatform*> init = platform::IdfPlatform::init();
    if (!init) {
        ESP_LOGE(kTag, "platform init failed (code %u)", static_cast<unsigned>(init.error().code));
        hal::SleepPlan retry{};
        retry.timer_us = kInitRetryUs;
        retry.wake_on_accel = false;
        platform::IdfPlatform::instance().sleep().deep_sleep(retry);
        esp_restart();
    }
    platform::IdfPlatform& hw = **init;

    // 2. Features. The radio is on only when it is compiled in AND qz_net actually provides it
    //    (until WP-27 the factories return nullptr, so the first image is radio-off at run time).
    hal::NetStack* net = nullptr;
    hal::ProvisioningPortal* portal = nullptr;
    if constexpr (kRadioCompiled) {
        net = net::net_stack();
        portal = net::provisioning_portal();
        if (net == nullptr || portal == nullptr) {
            net = nullptr;
            portal = nullptr;
        }
    }
    static const app::BuildFeatures features{
        .radio = net != nullptr,
        .usb_wake = kUsbWake,
        .selftest_interactive = kSelftestInteractive,
    };

    // 3. Application in static storage (no heap): hardware references, then the App.
    static app::Platform app_platform{
        .epd = hw.epd(),
        .accel = hw.accel_i2c(),
        .delay = hw.delay(),
        .io = hw.io(),
        .battery_adc = hw.battery_adc(),
        .kv = hw.kv(),
        .rtc_memory = hw.rtc_memory(),
        .clock = hw.clock(),
        .sleep = hw.sleep(),
        .system = hw.system(),
        .console = hw.console(),
        .net = net,
        .portal = portal,
    };
    static app::App application{app_platform, features};

    // 4. One complete wake. Tethered, this returns only when USB is gone or `sleep <s>` ran.
    hal::SleepPlan plan = application.run_wake();
    confirm_image_after_cold_boot(hw.system().reset_reason());

    // 5. Sleep. CONFIG_QZ_USB_WAKE gates the EXT0 USB-attach wake source (STATUS.md tech debt).
    plan.wake_on_usb = plan.wake_on_usb && features.usb_wake;
    hw.sleep().deep_sleep(plan);

    // deep_sleep() does not return unless the IDF rejected the request, in which case it has
    // already released the holds and restarted; restart again defensively.
    esp_restart();
}
