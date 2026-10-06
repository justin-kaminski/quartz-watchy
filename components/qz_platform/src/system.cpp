// System services: reset reason, wake causes, slow-clock info, RNG, heap, identity.
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_private/esp_clk.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "platform_impl.hpp"
#include "soc/rtc.h"

#include <string_view>

#ifndef QZ_GIT_HASH
/// WP-28 may inject the short git hash as a compile definition; IDF's app descriptor carries only
/// the version string (no hash field) [IDF:components/esp_app_format/include/esp_app_desc.h].
#define QZ_GIT_HASH ""
#endif

namespace qz::platform {
namespace {

constexpr hal::ResetReason map_reset_reason(esp_reset_reason_t reason) noexcept {
    // [IDF:components/esp_system/include/esp_system.h esp_reset_reason_t]
    switch (reason) {
        case ESP_RST_POWERON:
            return hal::ResetReason::kPowerOn;
        case ESP_RST_BROWNOUT:
            return hal::ResetReason::kBrownout;
        case ESP_RST_DEEPSLEEP:
            return hal::ResetReason::kDeepSleep;
        case ESP_RST_SW:
            return hal::ResetReason::kSoftware;
        case ESP_RST_PANIC:
        case ESP_RST_CPU_LOCKUP:
            return hal::ResetReason::kPanic;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return hal::ResetReason::kWatchdog;
        case ESP_RST_USB:
        case ESP_RST_JTAG:
            return hal::ResetReason::kUsbJtag;
        case ESP_RST_UNKNOWN:
        case ESP_RST_EXT:
        case ESP_RST_SDIO:
        case ESP_RST_EFUSE:
        case ESP_RST_PWR_GLITCH:
        default:
            return hal::ResetReason::kOther;
    }
}

constexpr bool has_cause(std::uint32_t causes, esp_sleep_source_t cause) noexcept {
    return (causes & (std::uint32_t{1} << static_cast<std::uint32_t>(cause))) != 0;
}

} // namespace

IdfSystem::IdfSystem() noexcept {
    // Order: time first (wake-latency measurement), then the reset/wake state.
    boot_rtc_us_ = static_cast<std::int64_t>(esp_rtc_get_time_us());
    reset_reason_ = map_reset_reason(esp_reset_reason());
    // The wakeup cause is a bitmap of all sources that fired
    // [IDF:components/esp_hw_support/include/esp_sleep.h esp_sleep_get_wakeup_causes].
    const std::uint32_t causes = esp_sleep_get_wakeup_causes();
    wake_.timer = has_cause(causes, ESP_SLEEP_WAKEUP_TIMER);
    wake_.ext0 = has_cause(causes, ESP_SLEEP_WAKEUP_EXT0);
    wake_.ext1 = has_cause(causes, ESP_SLEEP_WAKEUP_EXT1);
    // GPIO bit mask of the EXT1 pins that fired; 0 for any other cause
    // [IDF:components/esp_hw_support/include/esp_sleep.h esp_sleep_get_ext1_wakeup_status].
    wake_.ext1_pins = esp_sleep_get_ext1_wakeup_status();
}

hal::SlowClockInfo IdfSystem::slow_clock() const {
    hal::SlowClockInfo info{};
    // Actual selected source; IDF falls back to the internal 150 kHz RC when the 32 kHz crystal
    // does not start [IDF:components/esp_system/port/soc/esp32s3/clk.c select_rtc_slow_clk:
    // "32 kHz XTAL not found, switching to internal 150 kHz oscillator"].
    info.external_crystal = rtc_clk_slow_src_get() == SOC_RTC_SLOW_CLK_SRC_XTAL32K;
    // Calibration value = microseconds per slow-clock cycle in Q13.19
    // [IDF:components/esp_hw_support/include/esp_private/esp_clk.h; RTC_CLK_CAL_FRACT = 19 in
    // components/esp_hw_support/port/esp32s3/include/soc/rtc.h]; 0 = not calibrated.
    const std::uint32_t cal = esp_clk_slowclk_cal_get();
    if (cal != 0) {
        constexpr std::uint64_t kQ = std::uint64_t{1'000'000} << RTC_CLK_CAL_FRACT;
        info.measured_hz = static_cast<std::uint32_t>((kQ + cal / 2U) / cal); // rounded
    }
    return info;
}

hal::FirmwareInfo IdfSystem::firmware() const {
    hal::FirmwareInfo info{};
    const esp_app_desc_t* desc = esp_app_get_description(); // static flash data, never null
    info.version = std::string_view{desc->version};
    info.git_hash = std::string_view{QZ_GIT_HASH};
    info.idf_version = std::string_view{esp_get_idf_version()};
    return info;
}

std::uint64_t IdfSystem::chip_id() const {
    std::uint8_t mac[6] = {};
    // Base MAC from eFuse; works without the Wi-Fi driver
    // [IDF:components/esp_hw_support/include/esp_mac.h esp_read_mac ESP_MAC_BASE].
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        return 0;
    }
    std::uint64_t id = 0;
    for (const std::uint8_t byte : mac) {
        id = (id << 8U) | byte;
    }
    return id;
}

std::uint32_t IdfSystem::random_u32() {
    return esp_random(); // [IDF:components/esp_hw_support/include/esp_random.h]
}

hal::HeapInfo IdfSystem::heap() const {
    hal::HeapInfo info{};
    info.free_bytes = esp_get_free_heap_size();
    info.min_free_bytes = esp_get_minimum_free_heap_size();
    return info;
}

void IdfSystem::restart() {
    esp_restart(); // noreturn [IDF:components/esp_system/include/esp_system.h]
}

} // namespace qz::platform
