#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-ios}"
DIST_DIR="${ROOT_DIR}/dist"
IOS_DIR="${ROOT_DIR}/platforms/ios"

export IOS_SDK_PATH="${IOS_SDK_PATH:-/home/sian/toolchains/sdks/sdks/iPhoneOS16.5.sdk}"
export SDKROOT="${IOS_SDK_PATH}"
export IPHONEOS_DEPLOYMENT_TARGET="${IPHONEOS_DEPLOYMENT_TARGET:-14.0}"
if [[ -d "/home/sian/.local/bin" ]]; then
    export PATH="/home/sian/.local/bin:${PATH}"
fi
export GCC_AARCH64_BIN="${GCC_AARCH64_BIN:-$(command -v aarch64-linux-gnu-gcc 2>/dev/null || echo /home/sian/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc)}"

if [[ ! -d "${IOS_SDK_PATH}" ]]; then
    echo "error: iOS SDK not found at ${IOS_SDK_PATH}" >&2
    exit 1
fi

if [[ ! -x "${GCC_AARCH64_BIN}" ]] && ! command -v aarch64-linux-gnu-gcc >/dev/null; then
    echo "error: aarch64 GCC not found; set GCC_AARCH64_BIN" >&2
    exit 1
fi

echo "=== Building Melee native binary for iOS (arm64) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/ios.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DIOS_SDK_PATH="${IOS_SDK_PATH}" \
    -DCMAKE_OSX_SYSROOT="${IOS_SDK_PATH}" \
    -DAURORA_ENABLE_GX=ON \
    -DAURORA_ENABLE_DVD=ON \
    -DAURORA_ENABLE_CARD=ON \
    -DAURORA_ENABLE_THP=ON \
    -DAURORA_ENABLE_RMLUI=ON \
    -DAURORA_CACHE_USE_ZSTD=OFF \
    -DAURORA_DAWN_PROVIDER=package \
    -DAURORA_SDL3_PROVIDER=vendor \
    -DAURORA_NOD_PROVIDER=vendor \
    -DBUILD_SHARED_LIBS=OFF \
    -DPNG_SHARED=OFF \
    -DPNG_STATIC=ON

ninja -C "${BUILD_DIR}" melee

echo "=== Assembling iOS App Bundle (Melee.app) ==="
APP_DIR="${DIST_DIR}/Payload/Melee.app"
rm -rf "${DIST_DIR}/Payload"
mkdir -p "${APP_DIR}"
BINARY_SRC="${BUILD_DIR}/melee.app/melee"
if [[ ! -f "${BINARY_SRC}" ]]; then
    BINARY_SRC="${BUILD_DIR}/melee"
fi
cp "${BINARY_SRC}" "${APP_DIR}/melee"
chmod +x "${APP_DIR}/melee"
python3 -c "
import plistlib
with open('${IOS_DIR}/Info.plist', 'rb') as f_in, open('${APP_DIR}/Info.plist', 'wb') as f_out:
    data = plistlib.load(f_in)
    plistlib.dump(data, f_out, fmt=plistlib.FMT_BINARY)
"

if [[ -d "${ROOT_DIR}/resources" ]]; then
    for item in "${ROOT_DIR}/resources/"*; do
        if [[ -f "${item}" ]]; then
            cp "${item}" "${APP_DIR}/"
        fi
    done
fi

if [[ -f "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" ]]; then
    gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
        > "${APP_DIR}/initial_pipeline_cache.db"
fi

echo "=== Codesigning Melee.app bundle ==="
if command -v plumesign >/dev/null; then
    plumesign sign --package "${APP_DIR}" --apple-id --custom-identifier "dev.melee.melee-pc.KMT3995P5V"
    echo "Signed with Apple Developer identity via plumesign"
elif command -v ldid >/dev/null; then
    ldid -S"${IOS_DIR}/Melee.entitlements" "${APP_DIR}/melee"
    echo "Signed with ldid using Melee.entitlements"
fi

echo "=== Packaging Melee-iOS-arm64.ipa ==="
IPA_FILE="${DIST_DIR}/Melee-iOS-arm64.ipa"
rm -f "${IPA_FILE}"
(cd "${DIST_DIR}" && python3 -m zipfile -c "${IPA_FILE}" Payload)
cp -f "${IPA_FILE}" "${DIST_DIR}/Melee-Signed-Final.ipa"
if ! [[ "${IPA_FILE}" -ef "/home/sian/Desktop/Melee-iOS-arm64.ipa" ]]; then
    cp -f "${IPA_FILE}" "/home/sian/Desktop/Melee-iOS-arm64.ipa" || true
fi

echo "=== iOS IPA build complete ==="
ls -lh "${IPA_FILE}"
