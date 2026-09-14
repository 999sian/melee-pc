#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-win"
DIST_DIR="${ROOT_DIR}/dist"
STAGE_DIR="${DIST_DIR}/melee-windows-x86_64"

echo "=== Building Windows release ==="
ninja -C "${BUILD_DIR}" melee

echo "=== Staging Windows package ==="
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

cp "${BUILD_DIR}/melee.exe" "${STAGE_DIR}/"
cp -r "${ROOT_DIR}/resources" "${STAGE_DIR}/"

# Copy Dawn, SDL3, Nod, and zlib/png DLLs
cp "${BUILD_DIR}/dxcompiler.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/dxil.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/webgpu_dawn.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/SDL3.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/libpng16.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/libzlib1.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/libnod.dll"

# Copy MinGW runtime DLLs
cp /usr/x86_64-w64-mingw32/bin/libgcc_s_seh-1.dll "${STAGE_DIR}/"
cp /usr/x86_64-w64-mingw32/bin/libstdc++-6.dll "${STAGE_DIR}/"
cp /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll "${STAGE_DIR}/"

echo "=== Creating Windows ZIP package ==="
cd "${DIST_DIR}"
rm -f "Melee-Windows-x86_64.zip"
7z a -tzip "Melee-Windows-x86_64.zip" "melee-windows-x86_64"

echo "=== Windows package successfully created at ${DIST_DIR}/Melee-Windows-x86_64.zip ==="
ls -lh "${DIST_DIR}/Melee-Windows-x86_64.zip"
