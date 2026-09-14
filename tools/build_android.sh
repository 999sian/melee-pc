#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/platforms/android"

export JAVA_HOME="/home/sian/Android/jdk17"
export ANDROID_HOME="/home/sian/Android"
export PATH="${JAVA_HOME}/bin:${PATH}"

STRIP_TOOL="/home/sian/Android/ndk/26.3.11579264/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

echo "=== Staging assets and native libraries ==="
mkdir -p "${ANDROID_DIR}/app/src/main/assets"
cp -r "${ROOT_DIR}/resources/"* "${ANDROID_DIR}/app/src/main/assets/"

mkdir -p "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a"
if [[ -f "${ROOT_DIR}/build/android-arm64/libmelee.so" ]]; then
    "${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libmelee.so" "${ROOT_DIR}/build/android-arm64/libmelee.so"
fi
if [[ -f "${ROOT_DIR}/build/android-arm64/_deps/png-build/libpng16.so" ]]; then
    "${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libpng16.so" "${ROOT_DIR}/build/android-arm64/_deps/png-build/libpng16.so"
fi

echo "=== Building Melee Android APK ==="
cd "${ANDROID_DIR}"
./gradlew :app:assembleDebug

mkdir -p "${ROOT_DIR}/dist"
cp "${ANDROID_DIR}/app/build/outputs/apk/debug/app-debug.apk" "${ROOT_DIR}/dist/Melee-Android-arm64-debug.apk"

echo "=== APK build complete ==="
ls -lh "${ROOT_DIR}/dist/Melee-Android-arm64-debug.apk"

