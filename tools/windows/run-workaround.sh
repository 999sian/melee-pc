#!/usr/bin/env bash
# Workaround launcher script for Melee PC
# Forces SDL3 to use DirectSound (or dummy) to avoid WASAPI audio startup crash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

export MELEE_DEBUG=1
export MELEE_LOG_FILE="melee-pc.log"
export MELEE_FPS=1

# Workaround for audio thread startup race: use DirectSound instead of WASAPI
# (or set SDL_AUDIO_DRIVER=dummy if DirectSound is not available)
export SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-directsound}"

DISC=""
for f in "${SCRIPT_DIR}/"*.ciso "${SCRIPT_DIR}/"*.iso; do
    if [ -f "$f" ]; then
        DISC="$f"
        break
    fi
done

echo "Running with SDL_AUDIO_DRIVER=${SDL_AUDIO_DRIVER}..."
if [ -f "${SCRIPT_DIR}/melee.exe" ]; then
    if command -v wine >/dev/null 2>&1; then
        wine "${SCRIPT_DIR}/melee.exe" ${DISC} "$@" 2>"${SCRIPT_DIR}/melee-frames.log"
    else
        echo "Error: melee.exe found but wine is not installed."
        exit 1
    fi
elif [ -f "${SCRIPT_DIR}/melee" ]; then
    "${SCRIPT_DIR}/melee" ${DISC} "$@" 2>"${SCRIPT_DIR}/melee-frames.log"
else
    echo "Error: Neither melee.exe nor melee binary found in ${SCRIPT_DIR}"
    exit 1
fi
