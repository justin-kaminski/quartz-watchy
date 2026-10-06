// Blocking delays: busy-wait below one scheduler quantum of useful accuracy, vTaskDelay above.
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_impl.hpp"

namespace qz::platform {
namespace {

/// Waits up to this long are spun in ROM (exact, thread-safe, no tick rounding). Longer waits
/// yield the CPU. 2 ms = two ticks at CONFIG_FREERTOS_HZ=1000 (docs/SDKCONFIG.md).
constexpr std::uint32_t kBusyWaitMaxUs = 2000;

} // namespace

void IdfDelay::delay_us(std::uint32_t us) {
    if (us <= kBusyWaitMaxUs) {
        esp_rom_delay_us(us); // [IDF:components/esp_rom/include/esp_rom_sys.h]
        return;
    }
    delay_ms(us / 1000U);
    esp_rom_delay_us(us % 1000U);
}

void IdfDelay::delay_ms(std::uint32_t ms) {
    if (ms == 0) {
        return;
    }
    // vTaskDelay(n) blocks for between n-1 and n tick periods (the call lands mid-tick), so one
    // extra tick guarantees "at least ms"
    // [IDF:components/freertos/FreeRTOS-Kernel/include/freertos/ task.h vTaskDelay: "resolution of
    // one tick period"].
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

} // namespace qz::platform
