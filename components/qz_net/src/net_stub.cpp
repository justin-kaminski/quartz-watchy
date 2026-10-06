// qz_net stubs: the first firmware image has no radio.
//
// Both factories return nullptr, which main/ and the app treat as "radio unavailable"
// (BuildFeatures::radio = false, Platform::net/portal = nullptr, connectivity mode behaves as Off).
// With CONFIG_QZ_RADIO=n this is the final implementation: nothing in this file references
// esp_wifi, esp_netif, lwIP, esp_http_* or mbedTLS, so the offline image carries no radio symbol
// (tools/check_offline.sh).
//
// WP-27 replaces the radio build: it adds the NetStack / ProvisioningPortal implementations under
// `#if CONFIG_QZ_RADIO` (returning their static instances) and keeps this nullptr path for
// CONFIG_QZ_RADIO=n. [TECH-DEBT] until WP-27 lands, the radio variant is functionally offline.
#include "qz/net/idf_net.hpp"

namespace qz::net {

hal::NetStack* net_stack() noexcept {
    return nullptr;
}

hal::ProvisioningPortal* provisioning_portal() noexcept {
    return nullptr;
}

} // namespace qz::net
