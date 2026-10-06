// hal::ProvisioningPortal on ESP-IDF: WPA2 SoftAP (1 client) + esp_http_server serving the page
// and form handler supplied by qz_conn. Radio build only (empty translation unit otherwise).
//
// Threading: esp_http_server runs URI handlers on its own task, but hal::PortalHandler must be
// called on the app task (it is not thread-safe). The httpd handler therefore only copies the
// request into a one-slot mailbox and waits; poll(), called by the app task, runs the PortalHandler
// and hands the answer back. The httpd task serializes handlers, so the mailbox never sees two
// requests at once. [IDF:components/esp_http_server/src/httpd_uri.c httpd_uri processes one
// request at a time on the server task]
//
// Security (ARCHITECTURE section 13): the AP is WPA2-PSK with the caller's random password and
// max_connection = 1; bodies are capped; the page carries the one-time token (checked by
// qz_conn); the session auto-stops after kMaxSessionUs even if the app never calls stop();
// nothing from a request body is logged, and the body buffer is wiped after use.
#include "sdkconfig.h"

#ifdef CONFIG_QZ_RADIO

#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "qz/core/log.hpp"
#include "qz/net/idf_net.hpp"
#include "wifi_radio.hpp"

#include <atomic>
#include <cstring>
#include <string_view>

namespace qz::net {
namespace {

constexpr const char* kTag = "portal";

/// Largest accepted POST body; mirrors conn::kMaxFormBytes (qz_net may not depend on qz_conn).
constexpr std::size_t kMaxBodyBytes = 1024;
/// Hard stop of the whole portal session (the app's own TTL is 5 min; this is the backstop).
constexpr std::int64_t kMaxSessionUs = 6LL * 60 * 1'000'000;
/// How long an httpd handler waits for the app task to service its request before 503.
constexpr std::uint32_t kHandlerWaitMs = 2000;
/// stop(): wait for an in-flight response, then let TCP drain before the AP disappears. [TUNE]
constexpr std::uint32_t kStopDrainMaxMs = 1000;
constexpr std::uint32_t kStopFlushMs = 150;
constexpr std::uint8_t kApChannel = 1;     // [TUNE]
constexpr std::size_t kMinPasswordLen = 8; // WPA2 passphrase 8..63 characters
constexpr std::size_t kMaxPasswordLen = 63;

constexpr const char* kCsp =
    "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'";

enum class Slot : std::uint8_t { kIdle, kRequested, kServing, kDone };
enum class Kind : std::uint8_t { kGet, kPost };

class IdfPortal final : public hal::ProvisioningPortal {
public:
    Status
    start(std::string_view ssid, const Secret<64>& password, hal::PortalHandler& handler) override {
        if (active_) {
            return Errc::kBusy;
        }
        WifiRadio& radio = WifiRadio::instance();
        if (radio.mode() != RadioMode::kNone) {
            return Errc::kBusy; // a sync session owns the radio
        }
        const std::string_view pass = password.reveal();
        if (ssid.empty() || ssid.size() > sizeof(wifi_config_t::ap.ssid) ||
            pass.size() < kMinPasswordLen || pass.size() > kMaxPasswordLen) {
            return Errc::kBadArgs;
        }
        init_sync_objects();

        wifi_config_t cfg{};
        std::memcpy(cfg.ap.ssid, ssid.data(), ssid.size());
        cfg.ap.ssid_len = static_cast<std::uint8_t>(ssid.size());
        std::memcpy(cfg.ap.password, pass.data(), pass.size());
        cfg.ap.channel = kApChannel;
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.ap.max_connection = 1; // exactly one client (the owner's phone)
        cfg.ap.ssid_hidden = 0;
        cfg.ap.beacon_interval = 100;
        cfg.ap.pmf_cfg.capable = true;
        cfg.ap.pmf_cfg.required = false;
        const esp_err_t err = radio.bring_up(RadioMode::kAp, cfg);
        secure_zero(&cfg, sizeof(cfg));
        if (err != ESP_OK) {
            return Error{Errc::kIo, static_cast<std::uint16_t>(static_cast<std::uint32_t>(err))};
        }

        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.max_open_sockets = 3; // phones open a few parallel connections; lwIP needs this + 3
        hc.max_uri_handlers = 2;
        hc.lru_purge_enable = true; // a stale socket never blocks the next request
        hc.recv_wait_timeout = 5;
        hc.send_wait_timeout = 5;
        hc.stack_size = 4096;
        esp_err_t herr = httpd_start(&server_, &hc);
        if (herr == ESP_OK) {
            httpd_uri_t get{};
            get.uri = "/";
            get.method = HTTP_GET;
            get.handler = &IdfPortal::on_get;
            get.user_ctx = this;
            httpd_uri_t post{};
            post.uri = "/save";
            post.method = HTTP_POST;
            post.handler = &IdfPortal::on_post;
            post.user_ctx = this;
            herr = httpd_register_uri_handler(server_, &get);
            if (herr == ESP_OK) {
                herr = httpd_register_uri_handler(server_, &post);
            }
        }
        if (herr != ESP_OK) {
            QZ_LOGW(kTag, "httpd start: %s", esp_err_to_name(herr));
            stop();
            return Error{Errc::kIo, static_cast<std::uint16_t>(static_cast<std::uint32_t>(herr))};
        }
        handler_ = &handler;
        slot_.store(Slot::kIdle);
        responded_ok_ = false;
        start_us_ = esp_timer_get_time();
        active_ = true;
        return ok();
    }

    Status poll(std::uint32_t timeout_ms) override {
        if (!active_) {
            return Errc::kInvalidState;
        }
        if (esp_timer_get_time() - start_us_ >= kMaxSessionUs) {
            stop();
            return Errc::kTimeout;
        }
        if (xSemaphoreTake(req_sem_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            return ok();
        }
        Slot expected = Slot::kRequested;
        if (!slot_.compare_exchange_strong(expected, Slot::kServing)) {
            return ok(); // the handler gave up (503) before we got here
        }
        resp_ptr_ = nullptr;
        resp_len_ = 0;
        resp_code_ = Code::kOk;
        if (kind_ == Kind::kGet) {
            const std::string_view page = handler_->page();
            if (page.empty()) {
                resp_code_ = Code::kUnavailable;
            } else {
                resp_ptr_ = page.data();
                resp_len_ = page.size();
            }
        } else {
            const Result<std::string_view> r = handler_->submit(std::string_view{body_, body_len_});
            if (r) {
                resp_ptr_ = r->data();
                resp_len_ = r->size();
                responded_ok_ = true;
            } else {
                resp_code_ = Code::kRejected; // generic text only; detail stays in qz_conn's log
            }
        }
        secure_zero(body_, sizeof(body_)); // the body holds the Wi-Fi password
        body_len_ = 0;
        slot_.store(Slot::kDone);
        (void)xSemaphoreGive(resp_sem_);
        return ok();
    }

    void stop() override {
        if (server_ != nullptr) {
            // A response may still be in flight (the app stops right after a successful POST):
            // let it reach lwIP, then give TCP a moment before the AP vanishes.
            for (std::uint32_t waited = 0; slot_.load() != Slot::kIdle && waited < kStopDrainMaxMs;
                 waited += 10) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (responded_ok_) {
                vTaskDelay(pdMS_TO_TICKS(kStopFlushMs));
            }
            (void)httpd_stop(server_); // joins the server task and closes every socket
            server_ = nullptr;
        }
        secure_zero(body_, sizeof(body_));
        body_len_ = 0;
        handler_ = nullptr;
        active_ = false;
        responded_ok_ = false;
        WifiRadio& radio = WifiRadio::instance();
        if (radio.mode() == RadioMode::kAp) {
            radio.teardown();
        }
    }

private:
    enum class Code : std::uint8_t { kOk, kRejected, kUnavailable };

    void init_sync_objects() noexcept {
        if (req_sem_ == nullptr) {
            // Static storage, created once; no heap. [IDF:freertos xSemaphoreCreateBinaryStatic]
            req_sem_ = xSemaphoreCreateBinaryStatic(&req_storage_);
            resp_sem_ = xSemaphoreCreateBinaryStatic(&resp_storage_);
        }
        (void)xSemaphoreTake(req_sem_, 0); // drain leftovers of a previous session
        (void)xSemaphoreTake(resp_sem_, 0);
    }

    static esp_err_t on_get(httpd_req_t* req) {
        return static_cast<IdfPortal*>(req->user_ctx)->serve(req, Kind::kGet);
    }
    static esp_err_t on_post(httpd_req_t* req) {
        return static_cast<IdfPortal*>(req->user_ctx)->serve(req, Kind::kPost);
    }

    /// Runs on the httpd task.
    esp_err_t serve(httpd_req_t* req, Kind kind) {
        if (slot_.load() != Slot::kIdle) {
            return httpd_resp_send_custom_err(req, "503 Service Unavailable", "Busy");
        }
        std::size_t len = 0;
        if (kind == Kind::kPost) {
            if (req->content_len == 0) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rejected");
            }
            if (req->content_len > kMaxBodyBytes) {
                return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Too large");
            }
            while (len < req->content_len) {
                const int n = httpd_req_recv(req, body_ + len, req->content_len - len);
                if (n <= 0) { // timeout, close or error: abandon the request
                    secure_zero(body_, sizeof(body_));
                    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                        (void)httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout");
                    }
                    return ESP_FAIL;
                }
                len += static_cast<std::size_t>(n);
            }
        }
        kind_ = kind;
        body_len_ = len;
        slot_.store(Slot::kRequested);
        (void)xSemaphoreGive(req_sem_);

        if (xSemaphoreTake(resp_sem_, pdMS_TO_TICKS(kHandlerWaitMs)) != pdTRUE) {
            Slot expected = Slot::kRequested;
            if (slot_.compare_exchange_strong(expected, Slot::kIdle)) {
                secure_zero(body_, sizeof(body_));
                return httpd_resp_send_custom_err(req, "503 Service Unavailable", "Busy");
            }
            // poll() already took the request; it finishes promptly (the handler never blocks).
            (void)xSemaphoreTake(resp_sem_, portMAX_DELAY);
        }
        esp_err_t err = ESP_OK;
        switch (resp_code_) {
            case Code::kOk:
                (void)httpd_resp_set_type(req, "text/html; charset=utf-8");
                (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
                (void)httpd_resp_set_hdr(req, "Content-Security-Policy", kCsp);
                (void)httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
                err = httpd_resp_send(req, resp_ptr_, static_cast<ssize_t>(resp_len_));
                break;
            case Code::kRejected:
                err = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Rejected");
                break;
            case Code::kUnavailable:
                err = httpd_resp_send_custom_err(req, "503 Service Unavailable", "Unavailable");
                break;
        }
        slot_.store(Slot::kIdle);
        return err;
    }

    httpd_handle_t server_ = nullptr;
    hal::PortalHandler* handler_ = nullptr;
    bool active_ = false;
    bool responded_ok_ = false;
    std::int64_t start_us_ = 0;

    // Mailbox between the httpd task and poll(); ownership alternates via slot_.
    std::atomic<Slot> slot_{Slot::kIdle};
    SemaphoreHandle_t req_sem_ = nullptr;
    SemaphoreHandle_t resp_sem_ = nullptr;
    StaticSemaphore_t req_storage_{};
    StaticSemaphore_t resp_storage_{};
    Kind kind_ = Kind::kGet;
    char body_[kMaxBodyBytes]{};
    std::size_t body_len_ = 0;
    const char* resp_ptr_ = nullptr;
    std::size_t resp_len_ = 0;
    Code resp_code_ = Code::kOk;
};

} // namespace

hal::ProvisioningPortal* provisioning_portal() noexcept {
    static IdfPortal instance; // constructing it initializes nothing
    return &instance;
}

} // namespace qz::net

#endif // CONFIG_QZ_RADIO
