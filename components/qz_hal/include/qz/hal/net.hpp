// Radio services. Implemented by qz_net (ESP-IDF) and qz_testkit (FakeNetStack). With
// CONFIG_QZ_RADIO=n, qz_net provides stubs returning Errc::kUnsupported and the app holds a
// null NetStack pointer. Not thread-safe: the app task drives one session at a time.
#pragma once

#include "qz/core/fixed_string.hpp"
#include "qz/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qz::hal {

struct WifiCredentials {
    FixedString<32> ssid;
    Secret<64> password; ///< empty = open network
};

struct HttpResponse {
    std::uint16_t status = 0; ///< HTTP status code
    std::size_t body_len = 0; ///< bytes written to the body buffer
    bool truncated = false;   ///< body larger than the buffer
};

class NetStack {
public:
    virtual ~NetStack() = default;
    /// esp_wifi_init + STA start + associate + DHCP within timeout. Increments radio_init_count().
    virtual Status connect(const WifiCredentials& creds, std::uint32_t timeout_ms) = 0;
    /// SNTP request; returns UTC microseconds valid at the RTC instant *rtc_us_at_utc.
    virtual Result<std::int64_t> sntp_sync(std::uint32_t timeout_ms,
                                           std::int64_t* rtc_us_at_utc) = 0;
    /// HTTPS GET with certificate-bundle validation; body written to `body`.
    virtual Result<HttpResponse>
    https_get(std::string_view url, std::span<char> body, std::uint32_t timeout_ms) = 0;
    /// Stops and deinitializes Wi-Fi, netif and event loop resources. Idempotent; always called.
    virtual void shutdown() = 0;
    /// Number of radio initializations since boot (diagnostics; Off mode must keep this 0).
    [[nodiscard]] virtual std::uint32_t radio_init_count() const = 0;
};

/// Pure-side handler of the provisioning web page (implemented in qz_conn).
class PortalHandler {
public:
    virtual ~PortalHandler() = default;
    /// HTML for GET /. Valid until the next call.
    [[nodiscard]] virtual std::string_view page() = 0;
    /// Handles POST /save (application/x-www-form-urlencoded body). Returns the response HTML;
    /// an error result is reported to the browser as HTTP 400 with a generic message.
    virtual Result<std::string_view> submit(std::string_view form_body) = 0;
};

/// SoftAP + HTTP server for provisioning (ARCHITECTURE.md section 13).
class ProvisioningPortal {
public:
    virtual ~ProvisioningPortal() = default;
    /// Starts a WPA2 SoftAP (max 1 client) and the HTTP server. Increments radio_init_count().
    virtual Status
    start(std::string_view ssid, const Secret<64>& password, PortalHandler& handler) = 0;
    /// Serves pending requests for up to timeout_ms (calls into the handler on this task).
    virtual Status poll(std::uint32_t timeout_ms) = 0;
    virtual void stop() = 0;
};

} // namespace qz::hal
