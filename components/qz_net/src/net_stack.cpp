// hal::NetStack on ESP-IDF: Wi-Fi STA, SNTP (esp_netif_sntp), HTTPS GET (esp_http_client +
// certificate bundle). Radio build only; with CONFIG_QZ_RADIO=n this file is empty and
// net_stub.cpp provides the nullptr factories.
//
// Off means off: constructing the instance touches no IDF API. Every radio resource is created in
// connect() (via WifiRadio::bring_up) and released in shutdown(); connect() also unwinds itself
// on every failure path, so a failed session leaves nothing behind.
#include "sdkconfig.h"

#ifdef CONFIG_QZ_RADIO

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif_sntp.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "qz/core/log.hpp"
#include "qz/net/idf_net.hpp"
#include "wifi_radio.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>

namespace qz::net {
namespace {

constexpr const char* kTag = "net";

/// Response body cap (ARCHITECTURE section 12: weather body capped at 4 KiB).
constexpr std::size_t kMaxBodyBytes = 4096;
constexpr std::size_t kMaxUrlBytes = 383;
constexpr std::string_view kHttpsPrefix = "https://";
/// HTTP buffers: weather response headers fit in 1 KiB; the GET request line + headers in 768 B.
constexpr int kHttpRxBuffer = 1024;
constexpr int kHttpTxBuffer = 768;
/// The open phase (DNS + TCP connect + TLS handshake + headers) is made of several steps that
/// each get the per-operation timeout, so each gets a third of the budget. [TUNE]
constexpr std::uint32_t kOpenDivisor = 3;
constexpr std::uint32_t kMinOpTimeoutMs = 1000;

static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= 2, "SDKCONFIG.md section 2: two SNTP servers");

// SNTP sync notification (tcpip task): the callback has no user argument, so the result lives in
// file-local atomics. [IDF:components/esp_netif/lwip/esp_netif_sntp.c sync_time_cb passes the
// timeval from lwIP; lwIP sets the system clock first, then calls it
// (components/lwip/apps/sntp/sntp.c sntp_sync_time)].
std::atomic<std::int64_t> g_sntp_utc_us{0};
std::atomic<std::int64_t> g_sntp_rtc_us{0};
std::atomic<bool> g_sntp_done{false};

void sntp_synced(struct timeval* tv) noexcept {
    if (tv == nullptr) {
        return;
    }
    // The RTC instant is sampled in the callback, as close as possible to the server time.
    g_sntp_rtc_us.store(static_cast<std::int64_t>(esp_rtc_get_time_us()));
    g_sntp_utc_us.store(static_cast<std::int64_t>(tv->tv_sec) * 1'000'000 +
                        static_cast<std::int64_t>(tv->tv_usec));
    g_sntp_done.store(true);
}

[[nodiscard]] std::uint16_t detail_of(esp_err_t err) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(err) & 0xFFFFU);
}

[[nodiscard]] Error io_error(esp_err_t err) noexcept {
    // ESP_ERR_HTTP_EAGAIN is what esp_http_client reports when its network timeout expires.
    // [IDF:components/esp_http_client/include/esp_http_client.h esp_http_client_read]
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_EAGAIN) {
        return Error{Errc::kTimeout, detail_of(err)};
    }
    return Error{Errc::kIo, detail_of(err)};
}

/// Closes and frees the HTTP client on every exit path.
class HttpClientGuard {
public:
    explicit HttpClientGuard(esp_http_client_handle_t c) noexcept : client_(c) {}
    ~HttpClientGuard() {
        if (client_ != nullptr) {
            (void)esp_http_client_close(client_);
            (void)esp_http_client_cleanup(client_);
        }
    }
    HttpClientGuard(const HttpClientGuard&) = delete;
    HttpClientGuard& operator=(const HttpClientGuard&) = delete;

private:
    esp_http_client_handle_t client_;
};

class IdfNetStack final : public hal::NetStack {
public:
    Status connect(const hal::WifiCredentials& creds, std::uint32_t timeout_ms) override {
        WifiRadio& radio = WifiRadio::instance();
        if (radio.mode() == RadioMode::kAp) {
            return Errc::kBusy; // the provisioning portal owns the radio
        }
        shutdown(); // a session that was never shut down must not leak into this one
        if (creds.ssid.empty()) {
            return Errc::kNoCredentials;
        }
        if (timeout_ms == 0) {
            return Errc::kBadArgs;
        }
        const std::int64_t deadline_us =
            esp_timer_get_time() + static_cast<std::int64_t>(timeout_ms) * 1000;

        wifi_config_t cfg{};
        // SSID is 1..32 raw bytes, not NUL-terminated in the driver struct (ssid[32]).
        // [IDF:components/esp_wifi/include/esp_wifi_types_generic.h wifi_sta_config_t]
        std::memcpy(cfg.sta.ssid, creds.ssid.c_str(), creds.ssid.size());
        const std::string_view pass = creds.password.reveal();
        std::memcpy(cfg.sta.password, pass.data(), pass.size());
        // Minimum accepted security: WPA2 when a password is set, so a rogue open AP with the
        // same name cannot downgrade us; open networks (empty password) accept anything.
        cfg.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        cfg.sta.pmf_cfg.capable = true;
        cfg.sta.pmf_cfg.required = false;
        cfg.sta.scan_method = WIFI_FAST_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        esp_err_t err = radio.bring_up(RadioMode::kSta, cfg);
        secure_zero(&cfg, sizeof(cfg)); // the driver keeps its own RAM copy until deinit
        if (err != ESP_OK) {
            return io_error(err); // bring_up already tore down
        }
        err = radio.begin_connect();
        if (err != ESP_OK) {
            radio.teardown();
            return io_error(err);
        }
        const EventBits_t bits = radio.wait(kBitGotIp | kBitFail, deadline_us);
        if ((bits & kBitGotIp) != 0) {
            connected_ = true;
            return ok();
        }
        const bool failed = (bits & kBitFail) != 0;
        const std::uint8_t reason = radio.last_reason();
        radio.teardown();
        QZ_LOGW(kTag,
                "connect %s (reason %u)",
                failed ? "failed" : "timed out",
                static_cast<unsigned>(reason));
        return failed ? Error{Errc::kIo, reason} : Error{Errc::kTimeout};
    }

    Result<std::int64_t> sntp_sync(std::uint32_t timeout_ms, std::int64_t* rtc_us_at_utc) override {
        if (!connected_) {
            return Errc::kInvalidState;
        }
        if (rtc_us_at_utc == nullptr || timeout_ms == 0) {
            return Errc::kBadArgs;
        }
        // Server names are the Kconfig string literals, which outlive the call: lwIP keeps the
        // pointers. [IDF:components/esp_netif/lwip/esp_netif_sntp.c sntp_init_api]
        const char* const names[] = {CONFIG_QZ_SNTP_SERVER_1, CONFIG_QZ_SNTP_SERVER_2};
        esp_sntp_config_t cfg{};
        cfg.wait_for_sync = true;
        cfg.start = true;
        cfg.sync_cb = &sntp_synced;
        for (const char* name : names) {
            if (name[0] != '\0') {
                cfg.servers[cfg.num_of_servers++] = name;
            }
        }
        if (cfg.num_of_servers == 0) {
            return Errc::kBadArgs;
        }
        g_sntp_done.store(false);
        esp_err_t err = esp_netif_sntp_init(&cfg); // deinitializes itself on failure
        if (err != ESP_OK) {
            return io_error(err);
        }
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
        esp_netif_sntp_deinit(); // stops lwIP SNTP polling and frees the semaphore on every path
        if (err != ESP_OK || !g_sntp_done.load()) {
            return io_error(err == ESP_OK ? ESP_FAIL : err);
        }
        *rtc_us_at_utc = g_sntp_rtc_us.load();
        return g_sntp_utc_us.load();
    }

    Result<hal::HttpResponse>
    https_get(std::string_view url, std::span<char> body, std::uint32_t timeout_ms) override {
        if (!connected_) {
            return Errc::kInvalidState;
        }
        // TLS validation is never optional: plain http:// is refused outright.
        if (url.size() <= kHttpsPrefix.size() || url.size() > kMaxUrlBytes ||
            url.substr(0, kHttpsPrefix.size()) != kHttpsPrefix || body.empty() || timeout_ms == 0) {
            return Errc::kBadArgs;
        }
        char url_z[kMaxUrlBytes + 1];
        std::memcpy(url_z, url.data(), url.size());
        url_z[url.size()] = '\0';

        const std::int64_t deadline_us =
            esp_timer_get_time() + static_cast<std::int64_t>(timeout_ms) * 1000;
        const std::uint32_t open_timeout_ms = std::max(timeout_ms / kOpenDivisor, kMinOpTimeoutMs);

        esp_http_client_config_t cfg{};
        cfg.url = url_z;
        cfg.timeout_ms = static_cast<int>(std::min(open_timeout_ms, timeout_ms));
        cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = &esp_crt_bundle_attach; // validates the chain AND the hostname
        cfg.disable_auto_redirect = true;               // no silent hops to other hosts
        cfg.keep_alive_enable = false;
        cfg.buffer_size = kHttpRxBuffer;
        cfg.buffer_size_tx = kHttpTxBuffer;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client == nullptr) {
            return Errc::kNoSpace;
        }
        const HttpClientGuard guard{client};

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            QZ_LOGW(kTag, "https open: %s", esp_err_to_name(err)); // URL carries the location
            return io_error(err);
        }
        const std::int64_t content_len = esp_http_client_fetch_headers(client);
        if (content_len < 0) {
            return io_error(content_len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL);
        }
        hal::HttpResponse res;
        res.status = static_cast<std::uint16_t>(esp_http_client_get_status_code(client));
        if (res.status != 200) {
            return res; // caller treats non-200 as failure; no body needed
        }
        const std::size_t cap = std::min(body.size(), kMaxBodyBytes);
        std::size_t len = 0;
        while (len < cap) {
            const std::int64_t left_us = deadline_us - esp_timer_get_time();
            if (left_us <= 0) {
                return Errc::kTimeout;
            }
            const std::uint32_t op_ms =
                static_cast<std::uint32_t>(std::min<std::int64_t>(left_us / 1000 + 1, timeout_ms));
            (void)esp_http_client_set_timeout_ms(client, static_cast<int>(op_ms));
            const int n = esp_http_client_read(
                client, body.data() + len, static_cast<int>(std::min<std::size_t>(cap - len, 512)));
            if (n < 0) {
                return io_error(n == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL);
            }
            if (n == 0) {
                break; // end of body
            }
            len += static_cast<std::size_t>(n);
        }
        res.body_len = len;
        // Truncated = the server has more than we were allowed to take. With Content-Length the
        // comparison is exact; for chunked bodies the client tells us whether it saw the end.
        res.truncated = content_len > 0 ? static_cast<std::size_t>(content_len) > len
                                        : !esp_http_client_is_complete_data_received(client);
        return res;
    }

    void shutdown() override {
        WifiRadio& radio = WifiRadio::instance();
        connected_ = false;
        if (radio.mode() == RadioMode::kSta) { // never tear down a portal session
            radio.teardown();
        }
    }

    [[nodiscard]] std::uint32_t radio_init_count() const override {
        return WifiRadio::instance().init_count();
    }

private:
    bool connected_ = false;
};

} // namespace

hal::NetStack* net_stack() noexcept {
    static IdfNetStack instance; // constructing it initializes nothing
    return &instance;
}

} // namespace qz::net

#endif // CONFIG_QZ_RADIO
