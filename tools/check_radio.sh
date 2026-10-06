#!/usr/bin/env bash
# Positive counterpart of check_offline.sh: the radio firmware image must really contain the
# Wi-Fi / SNTP / HTTPS / provisioning stack. Without it a broken CMake or Kconfig change could
# silently ship a "radio" build that is functionally offline (which is also what makes the offline
# check meaningful: both images are checked with the same symbol list).
#   tools/check_radio.sh [elf]        (default: build/fw/quartz.elf)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${here}/env.sh"
elf="${1:-${QZ_ROOT}/build/fw/quartz.elf}"
[ -f "${elf}" ] || { echo "no ELF at ${elf}: build with tools/fw.sh build" >&2; exit 2; }
required='esp_wifi_init|esp_wifi_deinit|esp_wifi_start|esp_netif_init|esp_netif_sntp_init|esp_http_client_init|esp_crt_bundle_attach|httpd_start|mbedtls_ssl_handshake'
syms="$(xtensa-esp32s3-elf-nm --defined-only "${elf}")"
missing=0
for s in ${required//|/ }; do
  if ! grep -Eq " [TtWw] ${s}\$" <<<"${syms}"; then
    echo "radio image is missing ${s}" >&2
    missing=1
  fi
done
[ "${missing}" -eq 0 ] || exit 1
echo ">> radio image contains the radio stack ($(basename "${elf}"))"
