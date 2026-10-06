#!/usr/bin/env bash
# Host build + unit tests (no device, no ESP-IDF).
#   tools/host.sh [configure|build|test|all] [preset]     preset: default (ASan+UBSan) | release | coverage
# QZ_BUILD_DIR=<dir> selects a private build directory (parallel agents must not share one).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
cmd="${1:-all}"
preset="${2:-default}"
if [ "${preset}" = "default" ]; then default_dir="${QZ_ROOT}/build/host"; else default_dir="${QZ_ROOT}/build/host-${preset}"; fi
build_dir="${QZ_BUILD_DIR:-${default_dir}}"
case "${build_dir}" in /*) ;; *) build_dir="${QZ_ROOT}/${build_dir}" ;; esac   # relative -> repo root
configure() { cmake --preset "${preset}" -B "${build_dir}" --log-level=WARNING; }
build()     { cmake --build "${build_dir}"; }
run_tests() { ctest --test-dir "${build_dir}" --output-on-failure --no-tests=error -j "$(nproc)"; }
cd "${QZ_ROOT}/host"
case "${cmd}" in
  configure) configure ;;
  build)     configure && build ;;
  test)      run_tests ;;
  all)       configure && build && run_tests ;;
  *) echo "usage: $0 [configure|build|test|all] [preset]" >&2; exit 2 ;;
esac
