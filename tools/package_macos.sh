#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/macos}"
DIST_DIR="${ROOT_DIR}/dist"
APP="${BUILD_DIR}/Melee.app"
ARCH="$(uname -m)"
MELEE_VERSION="${MELEE_VERSION:-0.0.0}"

# Apple's clang builds the C++ (aurora, Dawn glue, launcher); the decomp's C
# goes through Homebrew GCC via tools/gcc_launcher.py. See CMakeLists.txt.
echo "=== Building Melee PC (macOS ${ARCH}) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DAURORA_SDL3_PROVIDER="${AURORA_SDL3_PROVIDER:-system}" \
    -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-package}" \
    -DAURORA_NOD_PROVIDER="${AURORA_NOD_PROVIDER:-package}"
ninja -C "${BUILD_DIR}" melee

echo "=== Staging ${APP} ==="
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources" "${APP}/Contents/Frameworks"
sed "s/@MELEE_VERSION@/${MELEE_VERSION#v}/g" "${ROOT_DIR}/platforms/macos/Info.plist" \
    > "${APP}/Contents/Info.plist"
cp "${ROOT_DIR}/platforms/macos/melee.icns" "${APP}/Contents/Resources/"
cp "${BUILD_DIR}/melee" "${APP}/Contents/MacOS/melee"
# RelWithDebInfo carries the DWARF in the binary on Mach-O too; ship it stripped.
strip -S "${APP}/Contents/MacOS/melee"
# SDL_GetBasePath() is Contents/Resources/ inside a bundle; the launcher and
# aurora look for resources/ and the pipeline cache there.
cp -r "${ROOT_DIR}/resources" "${APP}/Contents/Resources/resources"
gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
    > "${APP}/Contents/Resources/initial_pipeline_cache.db"

echo "=== Bundling Homebrew dylibs ==="
# Copy every non-system dylib the binary (transitively) loads into
# Contents/Frameworks and point the load commands at @rpath.
FRAMEWORKS="${APP}/Contents/Frameworks"
bundle_deps() {
    local target="$1"
    otool -L "${target}" | awk 'NR > 1 { print $1 }' | while read -r dep; do
        case "${dep}" in
            /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) continue ;;
        esac
        local name; name="$(basename "${dep}")"
        if [[ ! -f "${FRAMEWORKS}/${name}" ]]; then
            cp "${dep}" "${FRAMEWORKS}/${name}"
            chmod u+w "${FRAMEWORKS}/${name}"
            install_name_tool -id "@rpath/${name}" "${FRAMEWORKS}/${name}"
            bundle_deps "${FRAMEWORKS}/${name}"
        fi
        install_name_tool -change "${dep}" "@rpath/${name}" "${target}"
    done
}
bundle_deps "${APP}/Contents/MacOS/melee"
install_name_tool -add_rpath "@executable_path/../Frameworks" "${APP}/Contents/MacOS/melee"
ls "${FRAMEWORKS}"

echo "=== Signing (ad hoc) ==="
# No Developer ID: an ad-hoc signature keeps the arm64 kernel happy after
# install_name_tool rewrote the load commands. First launch of a downloaded
# copy still needs right-click > Open (Gatekeeper), see README.
codesign --force --deep --sign - --timestamp=none "${APP}"
codesign --verify --deep --strict "${APP}"

echo "=== Generating zip ==="
mkdir -p "${DIST_DIR}"
rm -f "${DIST_DIR}/Melee-macOS-${ARCH}.zip"
ditto -c -k --keepParent "${APP}" "${DIST_DIR}/Melee-macOS-${ARCH}.zip"

echo "=== Packaging Complete ==="
ls -lh "${DIST_DIR}"
