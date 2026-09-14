#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/platforms/android"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/android-arm64}"

# Honour a preconfigured SDK/NDK (CI sets these); fall back to the local layout.
export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-${HOME}/Android}}"
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    ANDROID_NDK_HOME="$(find "${ANDROID_HOME}/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
fi
if [[ ! -d "${ANDROID_NDK_HOME}" ]]; then
    echo "error: no Android NDK found; set ANDROID_NDK_HOME" >&2
    exit 1
fi
export ANDROID_NDK_HOME
if [[ -d "${HOME}/Android/jdk17" && -z "${JAVA_HOME:-}" ]]; then
    export JAVA_HOME="${HOME}/Android/jdk17"
fi
[[ -n "${JAVA_HOME:-}" ]] && export PATH="${JAVA_HOME}/bin:${PATH}"

STRIP_TOOL="$(find "${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt" -name llvm-strip -print -quit)"
echo "=== NDK ${ANDROID_NDK_HOME} ==="

echo "=== Building native library (arm64-v8a) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
ninja -C "${BUILD_DIR}" melee

echo "=== Staging assets and native libraries ==="
mkdir -p "${ANDROID_DIR}/app/src/main/assets"
cp -r "${ROOT_DIR}/resources/"* "${ANDROID_DIR}/app/src/main/assets/"

mkdir -p "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a"
"${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libmelee.so" "${BUILD_DIR}/libmelee.so"
if [[ -f "${BUILD_DIR}/_deps/png-build/libpng16.so" ]]; then
    "${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libpng16.so" "${BUILD_DIR}/_deps/png-build/libpng16.so"
fi

echo "=== Building Melee Android APK ==="
cd "${ANDROID_DIR}"
./gradlew --no-daemon :app:assembleDebug

mkdir -p "${ROOT_DIR}/dist"
cp "${ANDROID_DIR}/app/build/outputs/apk/debug/app-debug.apk" "${ROOT_DIR}/dist/Melee-Android-arm64-debug.apk"

echo "=== APK build complete ==="
ls -lh "${ROOT_DIR}/dist/Melee-Android-arm64-debug.apk"
