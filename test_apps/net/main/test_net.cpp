// qz_net owner-run tests. None needs credentials or a reachable network: STA sessions target a
// network name that does not exist, so they exercise bring-up, the connect timeout and teardown.
//
// Heap baseline: esp_netif_deinit() is not supported by IDF (the lwIP tcpip task stays after the
// first netif init) [IDF:components/esp_netif/include/esp_netif.h], so the baseline is taken after
// one warm-up session of the same kind; every later session must return to it.
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "qz/net/idf_net.hpp"
#include "unity.h"

#include <cstddef>
#include <cstdint>

namespace {

using qz::Errc;

constexpr std::size_t kHeapTolerance = 256; // [TUNE] allocator bookkeeping jitter, bytes
constexpr int kCycles = 5;

std::size_t free_heap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void assert_wifi_down() {
    wifi_mode_t mode{};
    TEST_ASSERT_EQUAL_INT(ESP_ERR_WIFI_NOT_INIT, esp_wifi_get_mode(&mode));
}

void assert_heap_at(std::size_t baseline, const char* what) {
    const std::size_t now = free_heap();
    const std::size_t lo = baseline > kHeapTolerance ? baseline - kHeapTolerance : 0;
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(lo, now, what);
}

class StubHandler final : public qz::hal::PortalHandler {
public:
    std::string_view page() override { return "<html><body>ok</body></html>"; }
    qz::Result<std::string_view> submit(std::string_view) override { return Errc::kBadArgs; }
};

qz::hal::WifiCredentials missing_network() {
    qz::hal::WifiCredentials c;
    TEST_ASSERT_TRUE(c.ssid.assign("qz-test-no-such-network"));
    TEST_ASSERT_TRUE(c.password.assign("0123456789abcdef"));
    return c;
}

} // namespace

// Must run first (menu order = registration order): nothing may be initialized before use.
TEST_CASE("Off means off: factories initialize nothing", "[qz_net][off]") {
    qz::hal::NetStack* net = qz::net::net_stack();
    qz::hal::ProvisioningPortal* portal = qz::net::provisioning_portal();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_NOT_NULL(portal);
    TEST_ASSERT_EQUAL_UINT32(0, net->radio_init_count());
    assert_wifi_down();
}

TEST_CASE("NetStack: calls before connect are refused", "[qz_net][args]") {
    qz::hal::NetStack* net = qz::net::net_stack();
    std::int64_t rtc = 0;
    char body[64];
    TEST_ASSERT_TRUE(net->sntp_sync(1000, &rtc).error().code == Errc::kInvalidState);
    TEST_ASSERT_TRUE(net->https_get("https://example.com/", body, 1000).error().code ==
                     Errc::kInvalidState);
    qz::hal::WifiCredentials none;
    TEST_ASSERT_TRUE(net->connect(none, 1000).error().code == Errc::kNoCredentials);
    TEST_ASSERT_EQUAL_UINT32(0, net->radio_init_count()); // refused before any radio init
    net->shutdown();                                      // idempotent on an idle stack
    assert_wifi_down();
}

TEST_CASE("NetStack: failed sessions return the heap to baseline", "[qz_net][teardown]") {
    qz::hal::NetStack* net = qz::net::net_stack();
    const qz::hal::WifiCredentials creds = missing_network();
    const std::uint32_t inits0 = net->radio_init_count();

    TEST_ASSERT_FALSE(net->connect(creds, 3000)); // warm-up: one-time lwIP/PHY allocations
    net->shutdown();
    assert_wifi_down();
    const std::size_t baseline = free_heap();

    for (int i = 0; i < kCycles; ++i) {
        const qz::Status s = net->connect(creds, 3000);
        TEST_ASSERT_FALSE(s);
        TEST_ASSERT_TRUE(s.error().code == Errc::kTimeout || s.error().code == Errc::kIo);
        net->shutdown(); // SyncSession always calls it; must be harmless after a failed connect
        assert_wifi_down();
        assert_heap_at(baseline, "heap after failed STA session");
    }
    TEST_ASSERT_EQUAL_UINT32(inits0 + 1 + kCycles, net->radio_init_count());
}

TEST_CASE("Portal: sessions return the heap to baseline", "[qz_net][teardown]") {
    qz::hal::ProvisioningPortal* portal = qz::net::provisioning_portal();
    StubHandler handler;
    qz::Secret<64> pass;
    TEST_ASSERT_TRUE(pass.assign("Abcdefgh2345"));

    qz::Secret<64> weak;
    TEST_ASSERT_TRUE(weak.assign("short"));
    TEST_ASSERT_TRUE(portal->start("Quartz-TEST", weak, handler).error().code == Errc::kBadArgs);
    assert_wifi_down();

    TEST_ASSERT_TRUE(portal->start("Quartz-TEST", pass, handler)); // warm-up
    TEST_ASSERT_TRUE(portal->poll(50));
    portal->stop();
    assert_wifi_down();
    const std::size_t baseline = free_heap();

    for (int i = 0; i < kCycles; ++i) {
        TEST_ASSERT_TRUE(portal->start("Quartz-TEST", pass, handler));
        TEST_ASSERT_TRUE(portal->start("Quartz-TEST", pass, handler).error().code == Errc::kBusy);
        TEST_ASSERT_TRUE(portal->poll(100));
        portal->stop();
        portal->stop(); // idempotent
        assert_wifi_down();
        assert_heap_at(baseline, "heap after portal session");
    }
}

TEST_CASE("Radio ownership: portal and STA session exclude each other", "[qz_net][teardown]") {
    qz::hal::NetStack* net = qz::net::net_stack();
    qz::hal::ProvisioningPortal* portal = qz::net::provisioning_portal();
    StubHandler handler;
    qz::Secret<64> pass;
    TEST_ASSERT_TRUE(pass.assign("Abcdefgh2345"));
    TEST_ASSERT_TRUE(portal->start("Quartz-TEST", pass, handler));
    TEST_ASSERT_TRUE(net->connect(missing_network(), 1000).error().code == Errc::kBusy);
    net->shutdown(); // must NOT tear the portal down
    wifi_mode_t mode{};
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_wifi_get_mode(&mode));
    TEST_ASSERT_EQUAL_INT(WIFI_MODE_AP, mode);
    portal->stop();
    assert_wifi_down();
}
