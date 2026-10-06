#include "sdkconfig.h"

#ifdef CONFIG_QZ_RADIO

#include "esp_timer.h"
#include "esp_wifi_default.h"
#include "qz/core/log.hpp"
#include "wifi_radio.hpp"

#include <algorithm>

namespace qz::net {
namespace {

constexpr const char* kTag = "wifi";
/// Transient failures (AP not found yet, beacon timeout) are retried this many times inside the
/// caller's deadline; authentication failures are final immediately. [TUNE]
constexpr std::uint8_t kMaxConnectRetries = 3;
/// Time allowed for the STA/AP stop event after esp_wifi_stop(). [TUNE]
constexpr std::int64_t kStopWaitUs = 500'000;

bool is_auth_failure(std::uint8_t reason) noexcept {
    // [IDF:components/esp_wifi/include/esp_wifi_types_generic.h wifi_err_reason_t]
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

void log_err(const char* what, esp_err_t err) noexcept {
    QZ_LOGW(kTag, "%s: %s", what, esp_err_to_name(err)); // never any SSID/password
}

} // namespace

WifiRadio& WifiRadio::instance() noexcept {
    static WifiRadio radio;
    return radio;
}

void WifiRadio::on_event(void* arg, esp_event_base_t base, std::int32_t id, void* data) noexcept {
    static_cast<WifiRadio*>(arg)->handle_event(base, id, data);
}

void WifiRadio::handle_event(esp_event_base_t base, std::int32_t id, void* data) noexcept {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_DISCONNECTED: {
                if (!connecting_.load()) {
                    break; // teardown or a later drop: not part of the connect attempt
                }
                const auto* ev = static_cast<const wifi_event_sta_disconnected_t*>(data);
                const std::uint8_t reason = ev != nullptr ? ev->reason : std::uint8_t{0};
                last_reason_.store(reason);
                if (is_auth_failure(reason) || retries_ >= kMaxConnectRetries) {
                    xEventGroupSetBits(events_, kBitFail);
                } else {
                    ++retries_;
                    if (esp_wifi_connect() != ESP_OK) {
                        xEventGroupSetBits(events_, kBitFail);
                    }
                }
                break;
            }
            case WIFI_EVENT_STA_STOP:
            case WIFI_EVENT_AP_STOP:
                xEventGroupSetBits(events_, kBitStopped);
                break;
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events_, kBitGotIp);
    }
}

esp_err_t WifiRadio::bring_up(RadioMode mode, wifi_config_t& cfg) noexcept {
    if (mode_ != RadioMode::kNone || mode == RadioMode::kNone) {
        return ESP_ERR_INVALID_STATE;
    }
    mode_ = mode;
    init_count_.fetch_add(1);
    if (events_ == nullptr) {
        // Static storage: no heap, created once, reused by every session. [IDF:freertos]
        events_ = xEventGroupCreateStatic(&events_storage_);
        if (events_ == nullptr) {
            teardown();
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(events_, kBitGotIp | kBitFail | kBitStopped);
    connecting_.store(false);
    last_reason_.store(0);
    retries_ = 0;

    // One-time lwIP/tcpip init; esp_netif_deinit() is not supported, so the tcpip task stays
    // [IDF:components/esp_netif/include/esp_netif.h esp_netif_deinit]. Repeat calls are harmless.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        log_err("netif_init", err);
        teardown();
        return err;
    }
    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        loop_owned_ = true;
    } else if (err != ESP_ERR_INVALID_STATE) { // INVALID_STATE: someone else owns the loop
        log_err("event_loop", err);
        teardown();
        return err;
    }
    // Aborts internally on allocation failure (documented); the null check is belt and braces.
    // [IDF:components/esp_wifi/include/esp_wifi_default.h]
    netif_ = mode == RadioMode::kSta ? esp_netif_create_default_wifi_sta()
                                     : esp_netif_create_default_wifi_ap();
    if (netif_ == nullptr) {
        teardown();
        return ESP_ERR_NO_MEM;
    }
    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        log_err("wifi_init", err);
        teardown();
        return err;
    }
    wifi_inited_ = true;
    // RAM-only config: no NVS writes by the driver (SDKCONFIG.md section 2).
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, this, &wifi_inst_);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, this, &ip_inst_);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(mode == RadioMode::kSta ? WIFI_MODE_STA : WIFI_MODE_AP);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(mode == RadioMode::kSta ? WIFI_IF_STA : WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
        wifi_started_ = err == ESP_OK;
    }
    if (err != ESP_OK) {
        log_err("wifi_setup", err);
        teardown();
    }
    return err;
}

esp_err_t WifiRadio::begin_connect() noexcept {
    if (mode_ != RadioMode::kSta || !wifi_started_) {
        return ESP_ERR_INVALID_STATE;
    }
    retries_ = 0;
    connecting_.store(true);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        connecting_.store(false);
    }
    return err;
}

EventBits_t WifiRadio::wait(EventBits_t bits, std::int64_t deadline_us) noexcept {
    if (events_ == nullptr) {
        return 0;
    }
    for (;;) {
        const std::int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0) {
            return xEventGroupGetBits(events_) & bits;
        }
        const TickType_t ticks =
            std::max<TickType_t>(1, pdMS_TO_TICKS(static_cast<std::uint32_t>(left_us / 1000)));
        const EventBits_t got = xEventGroupWaitBits(events_, bits, pdFALSE, pdFALSE, ticks) & bits;
        if (got != 0) {
            return got;
        }
    }
}

void WifiRadio::teardown() noexcept {
    connecting_.store(false);
    if (wifi_started_) {
        if (mode_ == RadioMode::kSta) {
            (void)esp_wifi_disconnect();
        }
        xEventGroupClearBits(events_, kBitStopped);
        if (esp_wifi_stop() == ESP_OK) {
            // Let the default netif handlers see STA/AP_STOP before their loop goes away. Our
            // handler is registered after theirs, so its bit means they ran. [ASSUMED] dispatch
            // order = registration order on the default loop; bounded wait either way.
            (void)wait(kBitStopped, esp_timer_get_time() + kStopWaitUs);
        }
        wifi_started_ = false;
    }
    if (wifi_inst_ != nullptr) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_inst_);
        wifi_inst_ = nullptr;
    }
    if (ip_inst_ != nullptr) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_inst_);
        ip_inst_ = nullptr;
    }
    if (wifi_inited_) {
        (void)esp_wifi_deinit();
        wifi_inited_ = false;
    }
    if (netif_ != nullptr) {
        // Detaches the netif from the driver, unregisters its default handlers and destroys it.
        // [IDF:components/esp_wifi/include/esp_wifi_default.h esp_netif_destroy_default_wifi]
        esp_netif_destroy_default_wifi(netif_);
        netif_ = nullptr;
    }
    if (loop_owned_) {
        (void)esp_event_loop_delete_default();
        loop_owned_ = false;
    }
    mode_ = RadioMode::kNone;
}

} // namespace qz::net

#endif // CONFIG_QZ_RADIO
