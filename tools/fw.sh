#!/usr/bin/env bash
# Firmware wrapper around idf.py using the pinned ESP-IDF and an out-of-tree build directory.
#   tools/fw.sh build
#   tools/fw.sh -p /dev/ttyACM0 flash monitor
#   tools/fw.sh size | clean | menuconfig | <any idf.py arguments>
# Build output and the generated sdkconfig live in build/fw (git-ignored);
# QZ_FW_BUILD_DIR=<dir> selects a private build directory (parallel agents must not share one).
# QZ_FW_VARIANT=offline builds the radio-less image (build/fw-offline). After changing the variant
# of an existing build directory, delete it (sdkconfig is generated once).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${here}/env.sh"
cd "${QZ_ROOT}"
defaults="sdkconfig.defaults"
default_dir="build/fw"
case "${QZ_FW_VARIANT:-}" in
  "") ;;
  offline) defaults="sdkconfig.defaults;sdkconfig.defaults.offline"; default_dir="build/fw-offline" ;;
  *) echo "unknown QZ_FW_VARIANT '${QZ_FW_VARIANT}' (expected: offline)" >&2; exit 2 ;;
esac
build_dir="${QZ_FW_BUILD_DIR:-${default_dir}}"
exec idf.py -B "${build_dir}" -DSDKCONFIG="${build_dir}/sdkconfig" -DSDKCONFIG_DEFAULTS="${defaults}" "$@"
