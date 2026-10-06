#!/usr/bin/env bash
# qzctl: Quartz console client. Usage: tools/qzctl.sh [--port P] <wait-ready|run|screenshot|shell|selftest> ...
# Uses the repo .venv (tools/setup-dev.sh) when present, else python3 with pyserial installed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py="$here/../.venv/bin/python"
[[ -x "$py" ]] || py="$(command -v python3)"
PYTHONPATH="$here${PYTHONPATH:+:$PYTHONPATH}" exec "$py" -m qzctl "$@"
