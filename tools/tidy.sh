#!/usr/bin/env bash
# clang-tidy over the host compile database (build/host).   tools/tidy.sh [files...]
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
cd "${QZ_ROOT}"
db="${QZ_ROOT}/build/host"
[ -f "${db}/compile_commands.json" ] || tools/host.sh configure >/dev/null
if [ "$#" -gt 0 ]; then
  files=("$@")
else
  # Only translation units that are part of the host build (headers are covered via HeaderFilterRegex).
  mapfile -t files < <(python3 - "${db}/compile_commands.json" "${QZ_ROOT}" <<'PY'
import json, sys, os
db, root = sys.argv[1], sys.argv[2]
seen = set()
for e in json.load(open(db)):
    f = os.path.realpath(e["file"])
    if f.startswith(root + os.sep) and "/_deps/" not in f and "/third_party/" not in f and f not in seen:
        seen.add(f)
        print(f)
PY
)
fi
[ "${#files[@]}" -gt 0 ] || { echo "no translation units"; exit 0; }
printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -n 4 clang-tidy -p "${db}" --quiet
echo ">> clang-tidy OK (${#files[@]} files)"
