// ESP-IDF radio services (IDF-only). With CONFIG_QZ_RADIO=n both functions return nullptr and
// no Wi-Fi/lwIP/mbedTLS symbol is referenced.
#pragma once

#include "qz/hal/net.hpp"

namespace qz::net {

/// Wi-Fi STA + SNTP + HTTPS (certificate bundle). Static instance; nothing is initialized until
/// NetStack::connect() is called (Off means off).
[[nodiscard]] hal::NetStack* net_stack() noexcept;
/// SoftAP + esp_http_server provisioning portal. Static instance; inert until start().
[[nodiscard]] hal::ProvisioningPortal* provisioning_portal() noexcept;
/// Bluetooth LE phone link (NimBLE). Static instance; the stack is powered only between
/// PhoneLink::start() and stop(). nullptr when CONFIG_QZ_PHONE is not set.
[[nodiscard]] hal::PhoneLink* phone_link() noexcept;

} // namespace qz::net
