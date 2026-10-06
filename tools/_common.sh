# shellcheck shell=bash
# Shared helpers for tools/*.sh (sourced, not executed).
QZ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export QZ_ROOT
VENV_BIN="${QZ_ROOT}/.venv/bin"
if [ ! -x "${VENV_BIN}/clang-format" ]; then
  "${QZ_ROOT}/tools/setup-dev.sh"
fi
export PATH="${VENV_BIN}:${PATH}"

# Source files we own (tracked or new-but-not-ignored); vendored/generated code is excluded.
qz_cxx_files() {
  (cd "${QZ_ROOT}" && git ls-files --cached --others --exclude-standard -- \
      '*.cpp' '*.hpp' '*.h' '*.cc' \
    | grep -vE '(^|/)(third_party|managed_components|build[^/]*)/' \
    | grep -vE '(^|/)generated/' \
    | sort -u)
}
