#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-win}"
DIST_DIR="${ROOT_DIR}/dist"
STAGE_DIR="${DIST_DIR}/melee-windows-x86_64"

echo "=== Building Windows release ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ROOT_DIR}/cmake/x86_64-w64-mingw32.cmake" \
    -DAURORA_SDL3_PROVIDER=package \
    -DAURORA_DAWN_PROVIDER=package \
    -DAURORA_NOD_PROVIDER=package
ninja -C "${BUILD_DIR}" melee

echo "=== Staging Windows package ==="
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

cp "${BUILD_DIR}/melee.exe" "${STAGE_DIR}/"
cp -r "${ROOT_DIR}/resources" "${STAGE_DIR}/"

# Dawn, SDL3, zlib/png DLLs land in the build root via AuroraCopyRuntimeDLLs.
for dll in dxcompiler.dll dxil.dll webgpu_dawn.dll SDL3.dll libpng16.dll libzlib1.dll; do
    cp "${BUILD_DIR}/${dll}" "${STAGE_DIR}/"
done
cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/libnod.dll"

# MinGW runtime DLLs: Arch keeps them under /usr/x86_64-w64-mingw32/bin,
# Debian/Ubuntu under /usr/lib/gcc/x86_64-w64-mingw32/*/. Search both.
echo "=== Locating MinGW runtime DLLs ==="
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    src="$(find /usr/x86_64-w64-mingw32 /usr/lib/gcc/x86_64-w64-mingw32 \
            -name "${dll}" -print -quit 2>/dev/null || true)"
    if [[ -z "${src}" ]]; then
        echo "error: ${dll} not found in the MinGW sysroot" >&2
        exit 1
    fi
    echo "  ${dll} <- ${src}"
    cp "${src}" "${STAGE_DIR}/"
done

echo "=== Creating Windows ZIP package ==="
cd "${DIST_DIR}"
rm -f "Melee-Windows-x86_64.zip"
if command -v 7z >/dev/null; then
    7z a -tzip "Melee-Windows-x86_64.zip" "melee-windows-x86_64"
else
    zip -qr "Melee-Windows-x86_64.zip" "melee-windows-x86_64"
fi

echo "=== Windows package successfully created at ${DIST_DIR}/Melee-Windows-x86_64.zip ==="
ls -lh "${DIST_DIR}/Melee-Windows-x86_64.zip"
