#!/usr/bin/env bash
# Verifies the offline firmware image really has no radio stack (SPEC: "Off means off", Kconfig compile-out).
# The positive counterpart (the radio image MUST contain the stack) is tools/check_radio.sh.
#   tools/check_offline.sh [elf]        (default: build/fw-offline/quartz.elf)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${here}/env.sh"
elf="${1:-${QZ_ROOT}/build/fw-offline/quartz.elf}"
[ -f "${elf}" ] || { echo "no ELF at ${elf}: build with QZ_FW_VARIANT=offline tools/fw.sh build" >&2; exit 2; }
# Symbols that can only be linked if Wi-Fi, lwIP, HTTP or TLS client code is present.
forbidden='nimble_port_init|ble_gap_adv_start|esp_bt_controller_init|ble_hs_init|esp_wifi_init|esp_wifi_start|esp_netif_init|esp_http_client_init|esp_http_server_start|httpd_start|mbedtls_ssl_handshake|mbedtls_ssl_setup|sntp_init|esp_sntp_init|lwip_socket|esp_wifi_deinit|esp_netif_sntp_init|esp_crt_bundle_attach|esp_http_client_open|esp_netif_create_default_wifi_sta|esp_netif_create_default_wifi_ap|tcpip_init'
hits="$(xtensa-esp32s3-elf-nm --defined-only "${elf}" | grep -E " [TtWwDdBb] (${forbidden})\$" || true)"
if [ -n "${hits}" ]; then
  echo "offline image links radio/Bluetooth symbols:" >&2
  echo "${hits}" >&2
  exit 1
fi
echo ">> offline image has no radio or Bluetooth symbols ($(basename "${elf}"))"
