#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${ROOT_DIR}/dist"
IPA_FILE="${DIST_DIR}/Melee-iOS-arm64.ipa"
DISC_PATH="${1:-/home/sian/Documents/Smash GC Port /melee.ciso}"
export PATH="/home/sian/.local/bin:${PATH}"
BUNDLE_ID="dev.melee.melee-pc.KMT3995P5V"

if [[ ! -f "${IPA_FILE}" ]]; then
    echo "IPA not found at ${IPA_FILE}. Building now..."
    "${ROOT_DIR}/tools/build_ios.sh"
fi

echo "=== Checking connected iOS devices ==="
if ! command -v idevice_id >/dev/null; then
    echo "error: libimobiledevice tools not found" >&2
    exit 1
fi

DEVICE_ID="$(idevice_id -l | head -n 1)"
if [[ -z "${DEVICE_ID}" ]]; then
    echo "error: No iOS device connected via USB. Please plug in your iPhone." >&2
    exit 1
fi
echo "Target device UDID: ${DEVICE_ID}"

echo "=== Validating USB lockdown pairing ==="
if ! idevicepair validate >/dev/null 2>&1; then
    echo "Device not paired. Requesting pair..."
    idevicepair pair || {
        echo "Please unlock your iPhone and tap 'Trust This Computer', then enter your passcode." >&2
        exit 1
    }
fi

echo "=== Installing Melee onto device ==="
uvx pymobiledevice3 apps install --developer --userspace "${IPA_FILE}"

if [[ -f "${DISC_PATH}" ]]; then
    echo "=== Pushing Melee disc (${DISC_PATH}) to app container ==="
    uvx pymobiledevice3 apps push --bundle-id "${BUNDLE_ID}" "${DISC_PATH}" "melee.ciso" || {
        echo "Note: Could not push disc directly; you can drop melee.ciso via iOS Files app under Melee folder."
    }
fi

echo "=== Launching Melee ==="
uvx pymobiledevice3 developer core-device launch-application "${BUNDLE_ID}" --userspace || true

echo "Deployment complete!"
