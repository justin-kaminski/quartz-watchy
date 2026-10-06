#!/usr/bin/env bash
# Verifies the offline firmware image really has no radio stack (SPEC: "Off means off", Kconfig compile-out).
#   tools/check_offline.sh [elf]        (default: build/fw-offline/quartz.elf)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${here}/env.sh"
elf="${1:-${QZ_ROOT}/build/fw-offline/quartz.elf}"
[ -f "${elf}" ] || { echo "no ELF at ${elf}: build with QZ_FW_VARIANT=offline tools/fw.sh build" >&2; exit 2; }
# Symbols that can only be linked if Wi-Fi, lwIP, HTTP or TLS client code is present.
forbidden='esp_wifi_init|esp_wifi_start|esp_netif_init|esp_http_client_init|esp_http_server_start|httpd_start|mbedtls_ssl_handshake|mbedtls_ssl_setup|sntp_init|esp_sntp_init|lwip_socket'
hits="$(xtensa-esp32s3-elf-nm --defined-only "${elf}" | grep -E " [TtWwDdBb] (${forbidden})\$" || true)"
if [ -n "${hits}" ]; then
  echo "offline image links radio symbols:" >&2
  echo "${hits}" >&2
  exit 1
fi
echo ">> offline image has no radio symbols ($(basename "${elf}"))"
