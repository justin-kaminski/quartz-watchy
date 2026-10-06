#!/usr/bin/env bash
# Prepare a release candidate: validate, run the full gate, build both firmware variants and collect
# the artifacts into dist/quartz-<version>/ (git-ignored). Never tags, pushes or publishes: it prints
# the tag command for the lead. Checklist and rules: docs/RELEASE.md.
#   tools/release.sh            full run (several minutes: host tests, clang-tidy, two firmware builds)
#   tools/release.sh --dry-run  validate only and print the plan; builds nothing, writes nothing
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${here}/.." && pwd)"
cd "${root}"

dry=0
for a in "$@"; do
  case "$a" in
    --dry-run) dry=1 ;;
    -h|--help) sed -n '2,7p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown flag $a (usage: tools/release.sh [--dry-run])" >&2; exit 2 ;;
  esac
done

# Image size budgets (docs/ARCHITECTURE.md section 20); partition slot is 3 MiB.
radio_budget=$((1600 * 1024))
offline_budget=$((600 * 1024))

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
ok()  { echo "ok:   $*"; }

# ---- 1. validate ------------------------------------------------------------------------------
version="$(tr -d '[:space:]' < version.txt)"
if [[ "${version}" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  major="${BASH_REMATCH[1]}"; ok "version.txt = ${version}"
else
  bad "version.txt '${version}' is not MAJOR.MINOR.PATCH"; major=0
fi
idf="$(tr -d '[:space:]' < .idf-version)"; ok "ESP-IDF pin (.idf-version) = ${idf}"

if grep -Eq "^## \[${version//./\\.}\]" CHANGELOG.md; then ok "CHANGELOG.md has an entry for ${version}"
else bad "CHANGELOG.md has no '## [${version}]' entry"; fi

branch="$(git rev-parse --abbrev-ref HEAD)"
if [ "${branch}" = "main" ]; then ok "on branch main"; else bad "on branch '${branch}', releases are cut from main"; fi
if [ -z "$(git status --porcelain)" ]; then ok "working tree is clean"
else bad "working tree is not clean (commit or stash first; untracked files count)"; fi
if git rev-parse -q --verify "refs/tags/v${version}" >/dev/null; then bad "tag v${version} already exists"
else ok "tag v${version} is free"; fi

if [ "${major}" -ge 1 ] && grep -q 'NOT MEASURED' docs/POWER_BUDGET.md; then
  bad "version >= 1.0.0 needs a measured baseline: docs/POWER_BUDGET.md still has NOT MEASURED cells"
fi
if [ "${major}" -eq 0 ]; then
  echo "note: 0.x release - the hardware release gate (docs/RELEASE.md) has not been passed by definition."
fi

hash="$(git rev-parse --short=7 HEAD)"
dist="${root}/dist/quartz-${version}"
if [ "${dry}" -eq 1 ]; then
  cat <<PLAN

Plan for a real run (--dry-run builds nothing):
  1. tools/fw.sh reconfigure (radio + offline) so the embedded git hash is ${hash}, not stale
  2. tools/check.sh --fw  (contract, generated files, format, host tests ASan+UBSan, clang-tidy,
                           both firmware builds, offline/radio symbol checks)
  3. image budgets: radio <= ${radio_budget} B, offline <= ${offline_budget} B; no '-dirty' hash in either
  4. collect into ${dist}/{radio,offline}/ + SHA256SUMS, size-*.txt, MANIFEST.txt
  5. print (not run): git tag -a v${version} -m "Quartz ${version}"
PLAN
  if [ "${fail}" -ne 0 ]; then echo "dry run: the checks above must pass before a real run." >&2; exit 1; fi
  echo "dry run: all preconditions pass."
  exit 0
fi
[ "${fail}" -eq 0 ] || { echo "release aborted: fix the failures above." >&2; exit 1; }

# ---- 2. gate and builds -----------------------------------------------------------------------
echo "== reconfigure (exact git hash) =="
tools/fw.sh reconfigure >/dev/null
QZ_FW_VARIANT=offline tools/fw.sh reconfigure >/dev/null
echo "== tools/check.sh --fw =="
tools/check.sh --fw
[ -z "$(git status --porcelain)" ] || { echo "the gate modified tracked files (e.g. generated/golden): inspect git status" >&2; exit 1; }

# ---- 3. collect -------------------------------------------------------------------------------
rm -rf "${dist}"; mkdir -p "${dist}"
collect() {   # collect <variant-name> <build-dir> <budget-bytes>
  local name="$1" bdir="$2" budget="$3" out="${dist}/$1" bin size
  bin="${bdir}/quartz.bin"
  [ -f "${bin}" ] || { echo "missing ${bin}" >&2; exit 1; }
  size="$(stat -c %s "${bin}")"
  [ "${size}" -le "${budget}" ] || { echo "${name} image ${size} B exceeds budget ${budget} B" >&2; exit 1; }
  if grep -aq -- "${hash}-dirty" "${bin}"; then
    echo "${name} image carries a -dirty git hash" >&2; exit 1
  fi
  mkdir -p "${out}/bootloader" "${out}/partition_table"
  cp "${bin}" "${out}/quartz.bin"
  cp "${bdir}/bootloader/bootloader.bin" "${out}/bootloader/"
  cp "${bdir}/partition_table/partition-table.bin" "${out}/partition_table/"
  cp "${bdir}/ota_data_initial.bin" "${bdir}/flash_args" "${bdir}/sdkconfig" "${out}/"
  echo "${name}: quartz.bin ${size} B (budget ${budget} B)"
}
collect radio "${root}/build/fw" "${radio_budget}"
collect offline "${root}/build/fw-offline" "${offline_budget}"
tools/fw.sh size > "${dist}/size-radio.txt" 2>&1
QZ_FW_VARIANT=offline tools/fw.sh size > "${dist}/size-offline.txt" 2>&1
cp version.txt .idf-version dependencies.lock CHANGELOG.md "${dist}/"
{
  echo "quartz ${version}"
  echo "git ${hash}"
  echo "esp-idf ${idf}"
  echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "status: $( [ "${major}" -eq 0 ] && echo 'pre-1.0, unverified on hardware until docs/HARDWARE_BRINGUP.md passes' || echo 'release' )"
  echo "flash: cd radio && python -m esptool --chip esp32s3 -p PORT write-flash @flash_args   (offline/ likewise)"
} > "${dist}/MANIFEST.txt"
( cd "${dist}" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS )
echo "== artifacts in ${dist} =="
( cd "${dist}" && cat SHA256SUMS )

# ---- 4. hand over -----------------------------------------------------------------------------
cat <<DONE

Release candidate ${version} (git ${hash}) is ready. Nothing was tagged, pushed or published.
Remaining (lead, after review; owner approves the push):
  git tag -a v${version} -m "Quartz ${version}"
  git push origin main v${version}
Hardware gate (docs/RELEASE.md): flash dist/quartz-${version}/ and re-run docs/HARDWARE_BRINGUP.md B9
against docs/POWER_BUDGET.md (fails at > 10 % drift from the baseline).
DONE
