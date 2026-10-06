#!/usr/bin/env bash
# clang-format over all owned C++ sources.   tools/format.sh [--check]
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
cd "${QZ_ROOT}"
mapfile -t files < <(qz_cxx_files)
[ "${#files[@]}" -gt 0 ] || { echo "no C++ files"; exit 0; }
if [ "${1:-}" = "--check" ]; then
  clang-format --dry-run --Werror "${files[@]}"
  echo ">> format OK (${#files[@]} files)"
else
  clang-format -i "${files[@]}"
  echo ">> formatted ${#files[@]} files"
fi
