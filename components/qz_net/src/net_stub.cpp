// qz_net stubs for CONFIG_QZ_RADIO=n (the offline image).
//
// Both factories return nullptr, which main/ and the app treat as "radio unavailable"
// (BuildFeatures::radio = false, Platform::net/portal = nullptr, connectivity forced Off).
// Nothing in this file, and nothing else in qz_net with the radio compiled out, references
// esp_wifi, esp_netif, lwIP, esp_http_* or mbedTLS, so the offline image carries no radio symbol
// (tools/check_offline.sh). The radio implementation lives in net_stack.cpp, portal.cpp and
// wifi_radio.cpp, which are empty translation units unless CONFIG_QZ_RADIO is set.
#include "sdkconfig.h"

#ifndef CONFIG_QZ_RADIO

#include "qz/net/idf_net.hpp"

namespace qz::net {

hal::NetStack* net_stack() noexcept {
    return nullptr;
}

hal::ProvisioningPortal* provisioning_portal() noexcept {
    return nullptr;
}

} // namespace qz::net

#endif // !CONFIG_QZ_RADIO
