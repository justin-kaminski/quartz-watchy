#!/usr/bin/env bash
# Host build + unit tests (no device, no ESP-IDF).
#   tools/host.sh [configure|build|test|all] [preset]     preset: default (ASan+UBSan) | release | coverage
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
cmd="${1:-all}"
preset="${2:-default}"
cd "${QZ_ROOT}/host"
case "${cmd}" in
  configure) cmake --preset "${preset}" ;;
  build)     cmake --preset "${preset}" && cmake --build --preset "${preset}" ;;
  test)      ctest --preset "${preset}" ;;
  all)       cmake --preset "${preset}" && cmake --build --preset "${preset}" && ctest --preset "${preset}" ;;
  *) echo "usage: $0 [configure|build|test|all] [preset]" >&2; exit 2 ;;
esac
