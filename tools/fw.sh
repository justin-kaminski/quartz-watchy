#!/usr/bin/env bash
# Firmware wrapper around idf.py using the pinned ESP-IDF and an out-of-tree build directory.
#   tools/fw.sh build
#   tools/fw.sh -p /dev/ttyACM0 flash monitor
#   tools/fw.sh size | clean | menuconfig | <any idf.py arguments>
# Build output and the generated sdkconfig live in build/fw (git-ignored);
# QZ_FW_BUILD_DIR=<dir> selects a private build directory (parallel agents must not share one).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${here}/env.sh"
cd "${QZ_ROOT}"
build_dir="${QZ_FW_BUILD_DIR:-build/fw}"
exec idf.py -B "${build_dir}" -DSDKCONFIG="${build_dir}/sdkconfig" "$@"
