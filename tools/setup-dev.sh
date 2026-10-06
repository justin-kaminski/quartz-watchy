#!/usr/bin/env bash
# Creates .venv with the pinned developer tooling (clang-format, clang-tidy, pytest, ...).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ ! -x "${root}/.venv/bin/python" ]; then
  python3 -m venv "${root}/.venv"
fi
"${root}/.venv/bin/pip" install -q --disable-pip-version-check -r "${root}/tools/requirements-dev.txt"
echo ">> dev tooling ready: $("${root}/.venv/bin/clang-format" --version)"
