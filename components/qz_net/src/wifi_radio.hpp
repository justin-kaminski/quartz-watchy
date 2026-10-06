// Shared Wi-Fi bring-up and teardown for NetStack (STA) and ProvisioningPortal (AP).
//
// Exactly one Wi-Fi stack instance exists at a time. Nothing is initialized until bring_up();
// teardown() unwinds exactly what bring_up() created (wifi driver, default netif, event handlers,
// default event loop if we created it) and is idempotent. Internal to qz_net (radio build only).
#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_QZ_RADIO

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qz::net {

enum class RadioMode : std::uint8_t { kNone, kSta, kAp };

/// Event bits set by the internal event handler.
inline constexpr EventBits_t kBitGotIp = 1U << 0;   ///< STA obtained an IPv4 address
inline constexpr EventBits_t kBitFail = 1U << 1;    ///< STA association failed for good
inline constexpr EventBits_t kBitStopped = 1U << 2; ///< STA/AP stop event processed

/// Wipes memory in a way the optimizer may not elide (credential scratch buffers).
inline void secure_zero(void* p, std::size_t n) noexcept {
    auto* b = static_cast<volatile unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        b[i] = 0;
    }
}

class WifiRadio {
public:
    static WifiRadio& instance() noexcept;

    [[nodiscard]] RadioMode mode() const noexcept { return mode_; }
    /// Radio initializations since boot (NetStack sessions + portal sessions), incremented
    /// before any IDF call so a failed bring-up still counts.
    [[nodiscard]] std::uint32_t init_count() const noexcept { return init_count_.load(); }
    /// Disconnect reason of the last failed association (diagnostic detail only).
    [[nodiscard]] std::uint8_t last_reason() const noexcept { return last_reason_.load(); }

    /// netif + event loop + wifi driver + handlers + set_config + esp_wifi_start for `mode`.
    /// `cfg` is the STA or AP configuration (the caller wipes its copy afterwards). On failure
    /// everything already created is torn down before returning.
    [[nodiscard]] esp_err_t bring_up(RadioMode mode, wifi_config_t& cfg) noexcept;
    /// STA only: starts association (retries transient failures; fails fast on auth errors).
    [[nodiscard]] esp_err_t begin_connect() noexcept;
    /// Blocks until one of `bits` is set or the monotonic deadline passes. Returns the bits seen.
    [[nodiscard]] EventBits_t wait(EventBits_t bits, std::int64_t deadline_us) noexcept;
    /// Full teardown; safe to call in any state, any number of times.
    void teardown() noexcept;

private:
    WifiRadio() = default;
    static void on_event(void* arg, esp_event_base_t base, std::int32_t id, void* data) noexcept;
    void handle_event(esp_event_base_t base, std::int32_t id, void* data) noexcept;

    RadioMode mode_ = RadioMode::kNone;
    esp_netif_t* netif_ = nullptr;
    bool loop_owned_ = false;
    bool wifi_inited_ = false;
    bool wifi_started_ = false;
    esp_event_handler_instance_t wifi_inst_ = nullptr;
    esp_event_handler_instance_t ip_inst_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    StaticEventGroup_t events_storage_{};
    std::atomic<bool> connecting_{false};
    std::atomic<std::uint8_t> last_reason_{0};
    std::atomic<std::uint32_t> init_count_{0};
    std::uint8_t retries_ = 0;
};

} // namespace qz::net

#endif // CONFIG_QZ_RADIO
