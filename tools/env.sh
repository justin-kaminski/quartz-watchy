# shellcheck shell=bash
# Source this file to get the pinned ESP-IDF environment from the in-repo toolchain:
#     source tools/env.sh
# Never uses an ESP-IDF from the surrounding environment.
_qz_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export QZ_ROOT="${_qz_root}"
if [ "${QZ_USE_SYSTEM_IDF:-0}" = "1" ]; then
  # CI: the espressif/idf container ships the pinned ESP-IDF at $IDF_PATH. GitHub "container:"
  # steps bypass the image entrypoint, so activate it here.
  : "${IDF_PATH:?QZ_USE_SYSTEM_IDF=1 requires IDF_PATH (espressif/idf container)}"
  # shellcheck disable=SC1091
  . "${IDF_PATH}/export.sh" > /dev/null
  unset _qz_root
  return 0 2>/dev/null || exit 0
fi
export IDF_TOOLS_PATH="${_qz_root}/.toolchain/espressif"
unset IDF_PATH
if [ ! -f "${_qz_root}/.toolchain/esp-idf/export.sh" ]; then
  echo "ESP-IDF not bootstrapped: run tools/bootstrap.sh first" >&2
  return 1 2>/dev/null || exit 1
fi
# shellcheck disable=SC1091
. "${_qz_root}/.toolchain/esp-idf/export.sh" > /dev/null
unset _qz_root
