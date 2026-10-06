include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)

qz_component(
  REQUIRES qz_core qz_hal
  PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls esp_http_server nvs_flash
  PLATFORM
)
