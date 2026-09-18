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

# `exec` hands our PID to the proton wrapper, but the wine child is a separate
# process: `timeout`, Ctrl-C or a harness kill reaps only the wrapper and
# melee.exe survives. A leaked instance keeps announcing itself over mDNS and
# will contaminate anyone else's LAN discovery runs (it did exactly that during
# the M0 determinism work). So run it in the background and tear the prefix
# down on every exit path.
child=
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if [[ -n "${child}" ]]; then
        kill "${child}" 2>/dev/null || true
    fi
    local wineserver="$(dirname "${PROTON}")/files/bin/wineserver"
    if [[ -x "${wineserver}" ]]; then
        WINEPREFIX="${STEAM_COMPAT_DATA_PATH}/pfx" "${wineserver}" -k 2>/dev/null || true
    fi
    # Belt and braces: anything still holding this exact image path.
    pkill -f "${ROOT_DIR}/build-win/melee.exe" 2>/dev/null || true
    exit "${rc}"
}
trap cleanup EXIT INT TERM

"${PROTON}" run "${ROOT_DIR}/build-win/melee.exe" "$@" &
child=$!
wait "${child}"
