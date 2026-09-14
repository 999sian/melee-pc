#!/usr/bin/env bash
# Run the Windows cross build under Proton, for testing it from Linux.
# Override PROTON to point at a different runtime.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STEAM_DIR="${STEAM_DIR:-$HOME/.local/share/Steam}"

if [[ -z "${PROTON:-}" ]]; then
    PROTON="$(find "${STEAM_DIR}/compatibilitytools.d" "${STEAM_DIR}/steamapps/common" \
        -maxdepth 2 -name proton -type f 2>/dev/null | sort -V | tail -1)"
fi
if [[ ! -x "${PROTON}" ]]; then
    echo "error: no proton found; set PROTON=/path/to/proton" >&2
    exit 1
fi

export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_DIR}"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-/tmp/proton_melee_test}"
export WINEDEBUG="-all"
mkdir -p "${STEAM_COMPAT_DATA_PATH}"

exec "${PROTON}" run "${ROOT_DIR}/build-win/melee.exe" "$@"
