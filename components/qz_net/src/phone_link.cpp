// hal::PhoneLink on ESP-IDF NimBLE (ARCHITECTURE.md section 13a). Phone build only (empty
// translation unit unless CONFIG_QZ_PHONE is set).
//
// One GATT service in the Nordic-UART layout (RX: phone writes request bytes; TX: watch notifies
// response bytes), carrying the console line protocol unchanged. Both characteristics demand an
// encrypted, MITM-authenticated link, and the watch asks for security as soon as a phone connects:
// IO capability DisplayOnly, LE Secure Connections, bonding. The phone OS shows a passkey prompt;
// the six digits come from esp_random() (the radio is on, so it is a true RNG) and are shown on the
// e-paper only, never logged. Bonds live in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST), so pairing
// happens once per phone.
//
// Threading: GAP/GATT callbacks run on the NimBLE host task. They assemble request lines and hand
// them to the app task through a FreeRTOS queue; state() and passkey() are atomics. send_line()
// runs on the app task and notifies in MTU-sized chunks.
//
// Power: nothing is initialized until start(); stop() stops the host task and deinitializes the
// controller (nimble_port_stop + nimble_port_deinit), so the radio is fully off between sessions.
// [ASSUMED] repeated start/stop cycles re-register the GATT service cleanly (bring-up B12).
#include "sdkconfig.h"

#ifdef CONFIG_QZ_PHONE

#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "qz/core/log.hpp"
#include "qz/net/idf_net.hpp"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

extern "C" void ble_store_config_init(void); // NimBLE NVS bond store (no public header)

namespace qz::net {
namespace {

constexpr const char* kTag = "phone";

using hal::PhoneLinkState;

/// Request lines: the console limit (console::kMaxRequestBytes, 256) plus slack; qz_net may not
/// depend on qz_console.
constexpr std::size_t kMaxLineBytes = 256;
constexpr UBaseType_t kQueueDepth = 4;
constexpr std::uint32_t kPasskeyModulo = 1'000'000;
constexpr const char* kBondNamespace = "nimble_bond";
constexpr std::uint16_t kMinNotifyPayload = 20; ///< ATT_MTU 23 - 3
constexpr int kNotifyRetries = 200;             ///< x 1 tick: mbuf pool exhaustion backoff
/// Advertising interval 100..150 ms in 0.625 ms units: discoverable within a second or two at a
/// fraction of the fast-advertising current. [TUNE]
constexpr std::uint16_t kAdvItvlMin = 160;
constexpr std::uint16_t kAdvItvlMax = 240;

struct Line {
    std::uint16_t len = 0;
    bool too_long = false;
    std::array<char, kMaxLineBytes> data{};
};

// Nordic UART Service UUIDs, little-endian byte order as NimBLE stores them.
// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (service), ...02... (RX, write), ...03... (TX, notify).
const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
const ble_uuid128_t kTxUuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

class IdfPhoneLink;
IdfPhoneLink* g_link = nullptr; // the single static instance (callbacks are C function pointers)

int gatt_access(std::uint16_t conn_handle,
                std::uint16_t attr_handle,
                ble_gatt_access_ctxt* ctxt,
                void* arg);
int gap_event(ble_gap_event* event, void* arg);

std::uint16_t g_tx_handle = 0;

// NimBLE keeps pointers into these tables: static storage, built once.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays): NimBLE's registration API takes terminated arrays
ble_gatt_chr_def g_chrs[] = {
    {
        .uuid = &kRxUuid.u,
        .access_cb = gatt_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = 16,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &kTxUuid.u,
        .access_cb = gatt_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_READ_AUTHEN,
        .min_key_size = 16,
        .val_handle = &g_tx_handle,
        .cpfd = nullptr,
    },
    {}, // terminator
};
ble_gatt_svc_def g_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .includes = nullptr,
        .characteristics = g_chrs,
    },
    {}, // terminator
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)

class IdfPhoneLink final : public hal::PhoneLink {
public:
    Status start(std::string_view name) override {
        if (running_) {
            return ok();
        }
        if (queue_ == nullptr) {
            queue_ =
                xQueueCreateStatic(kQueueDepth, sizeof(Line), queue_storage_.data(), &queue_buf_);
        }
        (void)xQueueReset(queue_);
        rx_ = Line{};
        conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        mtu_payload_ = kMinNotifyPayload;
        passkey_.store(0);
        state_.store(PhoneLinkState::kAdvertising);
        (void)std::snprintf(
            name_.data(), name_.size(), "%.*s", static_cast<int>(name.size()), name.data());

        const esp_err_t err = nimble_port_init();
        if (err != ESP_OK) {
            QZ_LOGW(kTag, "nimble_port_init: %s", esp_err_to_name(err));
            state_.store(PhoneLinkState::kOff);
            return Errc::kIo;
        }
        ble_hs_cfg.sync_cb = on_sync;
        ble_hs_cfg.reset_cb = on_reset;
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_mitm = 1;
        ble_hs_cfg.sm_sc = 1;
        ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

        ble_svc_gap_init();
        ble_svc_gatt_init();
        int rc = ble_gatts_count_cfg(g_svcs);
        if (rc == 0) {
            rc = ble_gatts_add_svcs(g_svcs);
        }
        if (rc == 0) {
            rc = ble_svc_gap_device_name_set(name_.data());
        }
        if (rc != 0) {
            QZ_LOGW(kTag, "gatt setup rc=%d", rc);
            (void)nimble_port_deinit();
            state_.store(PhoneLinkState::kOff);
            return Errc::kIo;
        }
        ble_store_config_init();
        running_ = true;
        ++inits_;
        nimble_port_freertos_init(host_task); // advertising starts in on_sync
        return ok();
    }

    void stop() override {
        if (!running_) {
            return;
        }
        state_.store(PhoneLinkState::kOff); // callbacks stop reporting from here on
        if (const std::uint16_t conn = conn_handle_.load(); conn != BLE_HS_CONN_HANDLE_NONE) {
            (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        (void)ble_gap_adv_stop();
        if (nimble_port_stop() == 0) {
            (void)nimble_port_deinit();
        } else {
            QZ_LOGW(kTag, "nimble_port_stop failed");
        }
        running_ = false;
        passkey_.store(0);
        conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        rx_ = Line{}; // may hold the start of a credential
        if (queue_ != nullptr) {
            Line drop;
            while (xQueueReceive(queue_, &drop, 0) == pdTRUE) {
                std::memset(drop.data.data(), 0, drop.data.size());
            }
        }
    }

    [[nodiscard]] PhoneLinkState state() const override { return state_.load(); }
    [[nodiscard]] std::uint32_t passkey() const override { return passkey_.load(); }

    Result<std::size_t> receive_line(std::span<char> out, std::uint32_t timeout_ms) override {
        if (!running_ || queue_ == nullptr) {
            return Errc::kInvalidState;
        }
        Line line;
        if (xQueueReceive(queue_, &line, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            return std::size_t{0};
        }
        if (line.too_long || line.len > out.size()) {
            return Errc::kNoSpace;
        }
        std::memcpy(out.data(), line.data.data(), line.len);
        std::memset(line.data.data(), 0, line.data.size());
        return std::size_t{line.len};
    }

    void send_line(std::string_view line) override {
        if (state_.load() != PhoneLinkState::kSecure || !subscribed_.load()) {
            return;
        }
        send_chunks(line);
        send_chunks("\n");
    }

    Status forget_bonds() override {
        if (running_) {
            return ble_store_clear() == 0 ? ok() : Status{Errc::kIo};
        }
        // Stack down: erase the bond store's NVS namespace directly instead of powering the radio
        // [IDF:components/bt/host/nimble/nimble/nimble/host/store/config/src/ble_store_nvs.c
        // NIMBLE_NVS_NAMESPACE]. A missing namespace means there is nothing to forget.
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(kBondNamespace, NVS_READWRITE, &handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return ok();
        }
        if (err != ESP_OK) {
            return Errc::kIo;
        }
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        return err == ESP_OK ? ok() : Status{Errc::kIo};
    }

    [[nodiscard]] std::uint32_t radio_init_count() const override { return inits_; }

    // ---- NimBLE callbacks (host task) --------------------------------------------------------
    void advertise() {
        if (state_.load() == PhoneLinkState::kOff) {
            return;
        }
        ble_hs_adv_fields fields{};
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name = reinterpret_cast<const std::uint8_t*>(name_.data());
        fields.name_len = static_cast<std::uint8_t>(std::strlen(name_.data()));
        fields.name_is_complete = 1;
        int rc = ble_gap_adv_set_fields(&fields);
        // The 128-bit service UUID does not fit next to the name (31-byte limit): scan response.
        ble_hs_adv_fields rsp{};
        rsp.uuids128 = &kServiceUuid;
        rsp.num_uuids128 = 1;
        rsp.uuids128_is_complete = 1;
        if (rc == 0) {
            rc = ble_gap_adv_rsp_set_fields(&rsp);
        }
        ble_gap_adv_params params{};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        params.itvl_min = kAdvItvlMin;
        params.itvl_max = kAdvItvlMax;
        if (rc == 0) {
            rc = ble_gap_adv_start(
                own_addr_type_, nullptr, BLE_HS_FOREVER, &params, gap_event, nullptr);
        }
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            QZ_LOGW(kTag, "advertise rc=%d", rc);
        }
        state_.store(PhoneLinkState::kAdvertising);
    }

    void on_gap(ble_gap_event* event) {
        switch (event->type) {
            case BLE_GAP_EVENT_CONNECT:
                if (event->connect.status != 0) {
                    advertise();
                    break;
                }
                conn_handle_ = event->connect.conn_handle;
                subscribed_.store(false);
                mtu_payload_ = kMinNotifyPayload;
                state_.store(PhoneLinkState::kPairing);
                // Ask for security right away: the phone shows its passkey prompt (or restores a
                // bond) before the page touches the protected characteristics.
                (void)ble_gap_security_initiate(event->connect.conn_handle);
                break;
            case BLE_GAP_EVENT_DISCONNECT:
                conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
                subscribed_.store(false);
                passkey_.store(0);
                rx_ = Line{};
                advertise();
                break;
            case BLE_GAP_EVENT_ADV_COMPLETE:
                if (conn_handle_.load() == BLE_HS_CONN_HANDLE_NONE) {
                    advertise();
                }
                break;
            case BLE_GAP_EVENT_ENC_CHANGE: {
                ble_gap_conn_desc desc{};
                const bool secure = event->enc_change.status == 0 &&
                                    ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
                                    desc.sec_state.encrypted != 0 &&
                                    desc.sec_state.authenticated != 0;
                passkey_.store(0);
                if (secure) {
                    state_.store(PhoneLinkState::kSecure);
                } else {
                    // Failed or unauthenticated pairing: never serve an insecure link.
                    (void)ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
                }
                break;
            }
            case BLE_GAP_EVENT_PASSKEY_ACTION:
                if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                    ble_sm_io io{};
                    io.action = BLE_SM_IOACT_DISP;
                    io.passkey = esp_random() % kPasskeyModulo;
                    passkey_.store(io.passkey);
                    (void)ble_sm_inject_io(event->passkey.conn_handle, &io);
                }
                break;
            case BLE_GAP_EVENT_REPEAT_PAIRING: {
                // The phone lost its bond but we kept ours: drop ours and let it pair again.
                ble_gap_conn_desc desc{};
                if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                    (void)ble_store_util_delete_peer(&desc.peer_id_addr);
                }
                last_repeat_ = BLE_GAP_REPEAT_PAIRING_RETRY;
                break;
            }
            case BLE_GAP_EVENT_SUBSCRIBE:
                if (event->subscribe.attr_handle == g_tx_handle) {
                    subscribed_.store(event->subscribe.cur_notify != 0);
                }
                break;
            case BLE_GAP_EVENT_MTU:
                mtu_payload_ = static_cast<std::uint16_t>(
                    std::max<int>(kMinNotifyPayload, event->mtu.value - 3));
                break;
            default:
                break;
        }
    }

    int on_write(ble_gatt_access_ctxt* ctxt) {
        if (state_.load() != PhoneLinkState::kSecure) {
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN; // belt and braces next to the flags
        }
        std::array<char, kMaxLineBytes> chunk{};
        std::uint16_t len = 0;
        const int rc = ble_hs_mbuf_to_flat(
            ctxt->om, chunk.data(), static_cast<std::uint16_t>(chunk.size()), &len);
        if (rc == BLE_HS_EMSGSIZE) {
            rx_.too_long = true; // a single write longer than any request: the line is void
        } else if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        for (std::uint16_t i = 0; i < len; ++i) {
            const char c = chunk[i];
            if (c == '\n') {
                if (xQueueSend(queue_, &rx_, 0) != pdTRUE) {
                    QZ_LOGW(kTag, "request queue full: line dropped");
                }
                std::memset(rx_.data.data(), 0, rx_.data.size());
                rx_.len = 0;
                rx_.too_long = false;
            } else if (c == '\r') {
                continue;
            } else if (rx_.len < rx_.data.size()) {
                rx_.data[rx_.len++] = c;
            } else {
                rx_.too_long = true;
            }
        }
        std::memset(chunk.data(), 0, chunk.size());
        return 0;
    }

    int repeat_pairing_answer() const { return last_repeat_; }

private:
    static void host_task(void* /*param*/) {
        nimble_port_run(); // returns when nimble_port_stop() runs
        nimble_port_freertos_deinit();
    }

    static void on_sync() {
        if (g_link == nullptr) {
            return;
        }
        if (ble_hs_util_ensure_addr(0) != 0 ||
            ble_hs_id_infer_auto(0, &g_link->own_addr_type_) != 0) {
            QZ_LOGW(kTag, "no usable BLE address");
            return;
        }
        g_link->advertise();
    }

    static void on_reset(int reason) { QZ_LOGW(kTag, "host reset, reason=%d", reason); }

    void send_chunks(std::string_view data) {
        while (!data.empty()) {
            const std::size_t n = std::min<std::size_t>(data.size(), mtu_payload_.load());
            int rc = BLE_HS_ENOMEM;
            for (int attempt = 0; attempt < kNotifyRetries && rc == BLE_HS_ENOMEM; ++attempt) {
                os_mbuf* om = ble_hs_mbuf_from_flat(data.data(), static_cast<std::uint16_t>(n));
                if (om == nullptr) {
                    vTaskDelay(1);
                    continue;
                }
                rc = ble_gatts_notify_custom(conn_handle_.load(), g_tx_handle, om); // consumes om
                if (rc == BLE_HS_ENOMEM) {
                    vTaskDelay(1);
                }
            }
            if (rc != 0) {
                QZ_LOGW(kTag, "notify rc=%d: response truncated", rc);
                return;
            }
            data.remove_prefix(n);
        }
    }

    std::atomic<PhoneLinkState> state_{PhoneLinkState::kOff};
    std::atomic<std::uint32_t> passkey_{0};
    std::atomic<bool> subscribed_{false};
    std::array<char, 32> name_{};
    std::array<std::uint8_t, kQueueDepth * sizeof(Line)> queue_storage_{};
    StaticQueue_t queue_buf_{};
    QueueHandle_t queue_ = nullptr;
    Line rx_{}; ///< line being assembled (host task only)
    std::atomic<std::uint16_t> conn_handle_{BLE_HS_CONN_HANDLE_NONE}; ///< host task writes
    std::atomic<std::uint16_t> mtu_payload_{kMinNotifyPayload};
    std::uint8_t own_addr_type_ = 0;
    int last_repeat_ = BLE_GAP_REPEAT_PAIRING_RETRY;
    std::uint32_t inits_ = 0;
    bool running_ = false;
};

int gatt_access(std::uint16_t /*conn_handle*/,
                std::uint16_t attr_handle,
                ble_gatt_access_ctxt* ctxt,
                void* /*arg*/) {
    if (g_link == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            return g_link->on_write(ctxt);
        case BLE_GATT_ACCESS_OP_READ_CHR:
            return attr_handle == g_tx_handle ? 0 : BLE_ATT_ERR_UNLIKELY; // empty value
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

int gap_event(ble_gap_event* event, void* /*arg*/) {
    if (g_link == nullptr) {
        return 0;
    }
    g_link->on_gap(event);
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        return g_link->repeat_pairing_answer();
    }
    return 0;
}

} // namespace

hal::PhoneLink* phone_link() noexcept {
    static IdfPhoneLink link;
    g_link = &link;
    return &link;
}

} // namespace qz::net

#endif // CONFIG_QZ_PHONE
