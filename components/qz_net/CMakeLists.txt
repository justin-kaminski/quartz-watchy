include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

# One source set for both firmware variants: every radio file is an empty translation unit unless
# CONFIG_QZ_RADIO is set (phone_link.cpp: CONFIG_QZ_PHONE), and net_stub.cpp provides nullptr
# factories for whatever is compiled out. The IDF
# requirements below are link-level only: with CONFIG_QZ_RADIO=n nothing references them, so the
# linker drops them (verified by tools/check_offline.sh).
qz_component(
  REQUIRES qz_core qz_hal
  PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls esp_http_server
                bt nvs_flash freertos esp_timer esp_hw_support log
  PLATFORM
)
