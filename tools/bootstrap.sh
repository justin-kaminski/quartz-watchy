#!/usr/bin/env bash
# Bootstraps the pinned ESP-IDF toolchain *inside the repository* (.toolchain/, git-ignored).
# Idempotent: re-running verifies the pinned tag and re-installs missing tools only.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_version="$(tr -d '[:space:]' < "${repo_root}/.idf-version")"
toolchain="${repo_root}/.toolchain"

export IDF_TOOLS_PATH="${toolchain}/espressif"
export GIT_TERMINAL_PROMPT=0
unset IDF_PATH   # never pick up an ESP-IDF from the surrounding environment

mkdir -p "${toolchain}"

if [ ! -d "${toolchain}/esp-idf/.git" ]; then
  echo ">> cloning ESP-IDF ${idf_version} (shallow, with submodules)"
  git clone --branch "${idf_version}" --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "${toolchain}/esp-idf"
else
  have="$(git -C "${toolchain}/esp-idf" describe --tags --exact-match 2>/dev/null || true)"
  if [ "${have}" != "${idf_version}" ]; then
    echo "!! ${toolchain}/esp-idf is at '${have:-unknown}', expected '${idf_version}'." >&2
    echo "!! Delete ${toolchain}/esp-idf and re-run to get the pinned version." >&2
    exit 1
  fi
fi

echo ">> installing ESP-IDF tools for esp32s3"
( cd "${toolchain}/esp-idf" && ./install.sh esp32s3 )
echo ">> bootstrap done"
