#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/macos}"
DIST_DIR="${ROOT_DIR}/dist"
APP="${BUILD_DIR}/Melee.app"
ARCH="$(uname -m)"
export MELEE_VERSION="${MELEE_VERSION:-$(cat "${ROOT_DIR}/VERSION")}"

# Apple's clang builds the C++ (aurora, Dawn glue, launcher); the decomp's C
# goes through Homebrew GCC via tools/gcc_launcher.py. See CMakeLists.txt.
echo "=== Building Melee PC (macOS ${ARCH}) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DAURORA_SDL3_PROVIDER=vendor \
    -DAURORA_SDL3_LINKAGE=shared \
    -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-package}" \
    -DAURORA_NOD_PROVIDER="${AURORA_NOD_PROVIDER:-package}"
ninja -C "${BUILD_DIR}" melee

echo "=== Staging ${APP} ==="
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources" "${APP}/Contents/Frameworks"
BUNDLE_VERSION="${MELEE_VERSION#v}"
BUNDLE_VERSION="${BUNDLE_VERSION%%-*}"
sed "s/@MELEE_VERSION@/${BUNDLE_VERSION}/g" "${ROOT_DIR}/platforms/macos/Info.plist" \
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
# SDL uses an @rpath install name, so it is copied here rather than found by
# bundle_deps.
cp -L "${BUILD_DIR}/_deps/sdl-build/libSDL3.0.dylib" "${FRAMEWORKS}/"
install_name_tool -id "@rpath/libSDL3.0.dylib" "${FRAMEWORKS}/libSDL3.0.dylib"
# Print the file an @rpath/<name> dependency of <source> resolves to, using
# <source>'s LC_RPATH entries. Homebrew dylibs load their siblings this way
# (libbrotlidec -> @rpath/libbrotlicommon with @loader_path/../lib).
resolve_rpath_dep() {
    local source="$1" name="$2"
    local dir; dir="$(dirname "${source}")"
    otool -l "${source}" | awk '$1 == "cmd" { rpath = ($2 == "LC_RPATH") }
                               rpath && $1 == "path" { print $2 }' |
    while read -r rpath; do
        rpath="${rpath/#@loader_path/${dir}}"
        rpath="${rpath/#@executable_path/${dir}}"
        if [[ -f "${rpath}/${name}" ]]; then
            echo "${rpath}/${name}"
            break
        fi
    done
}
# bundle_deps <target> [<source>]: <source> is where <target> was copied from,
# so its @loader_path rpaths still point at the Homebrew prefix.
bundle_deps() {
    local target="$1" source="${2:-$1}"
    otool -L "${target}" | awk 'NR > 1 { print $1 }' | while read -r dep; do
        local name; name="$(basename "${dep}")"
        local from="${dep}"
        case "${dep}" in
            /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) continue ;;
            @rpath/*)
                [[ "${name}" == "$(basename "${target}")" ]] && continue
                [[ -f "${FRAMEWORKS}/${name}" ]] && continue
                from="$(resolve_rpath_dep "${source}" "${name}")"
                if [[ -z "${from}" ]]; then
                    echo "error: ${target} needs ${dep}, not found via ${source}'s rpaths" >&2
                    exit 1
                fi
                ;;
        esac
        if [[ ! -f "${FRAMEWORKS}/${name}" ]]; then
            cp -L "${from}" "${FRAMEWORKS}/${name}"
            chmod u+w "${FRAMEWORKS}/${name}"
            install_name_tool -id "@rpath/${name}" "${FRAMEWORKS}/${name}"
            bundle_deps "${FRAMEWORKS}/${name}" "${from}"
        fi
        if [[ "${dep}" != "@rpath/${name}" ]]; then
            install_name_tool -change "${dep}" "@rpath/${name}" "${target}"
        fi
    done
}
bundle_deps "${FRAMEWORKS}/libSDL3.0.dylib" "${BUILD_DIR}/_deps/sdl-build/libSDL3.0.dylib"
bundle_deps "${APP}/Contents/MacOS/melee" "${BUILD_DIR}/melee"
install_name_tool -add_rpath "@executable_path/../Frameworks" "${APP}/Contents/MacOS/melee"
ls "${FRAMEWORKS}"
# Every @rpath load command must now name a bundled file; a miss here is a
# launch-time "Library not loaded" on any Mac without the same Homebrew.
for f in "${APP}/Contents/MacOS/melee" "${FRAMEWORKS}"/*.dylib; do
    otool -L "${f}" | awk 'NR > 1 { print $1 }' | grep '^@rpath/' | while read -r dep; do
        [[ "${dep}" == "@rpath/$(basename "${f}")" ]] && continue
        if [[ ! -f "${FRAMEWORKS}/${dep#@rpath/}" ]]; then
            echo "error: $(basename "${f}") needs ${dep}, which is not in Frameworks" >&2
            exit 1
        fi
    done
done

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
