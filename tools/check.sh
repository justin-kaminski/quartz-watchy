#!/usr/bin/env bash
# The local quality gate (same steps CI runs).   tools/check.sh [--fast] [--fw]
#   (default)  format check + host build/tests (ASan+UBSan) + clang-tidy
#   --fast     skip clang-tidy
#   --fw       also build the firmware with the pinned ESP-IDF
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
fast=0; fw=0
for a in "$@"; do case "$a" in --fast) fast=1;; --fw) fw=1;; *) echo "unknown flag $a" >&2; exit 2;; esac; done
cd "${QZ_ROOT}"
echo "== component contract =="; tools/check_deps.py
echo "== generated fonts are current =="; python3 tools/fontgen.py --check
echo "== generated tz table is current =="; python3 tools/tzgen.py --check
echo "== format =="; tools/format.sh --check
echo "== host build + tests =="; tools/host.sh all
if [ "${fast}" -eq 0 ]; then echo "== clang-tidy =="; tools/tidy.sh; fi
if [ "${fw}" -eq 1 ]; then
  echo "== firmware build (radio) =="; tools/fw.sh build
  echo "== firmware build (offline) =="; QZ_FW_VARIANT=offline tools/fw.sh build
  echo "== offline image has no radio symbols =="; tools/check_offline.sh
fi
echo "== ALL CHECKS PASSED =="
