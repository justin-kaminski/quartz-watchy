#!/usr/bin/env bash
# Runs a command inside the pinned ESP-IDF environment without polluting the caller's shell.
#   tools/idf.sh idf.py build
#   tools/idf.sh idf.py -p /dev/ttyACM0 flash monitor
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${here}/env.sh"
exec "$@"
