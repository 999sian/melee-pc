#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
DIST_DIR="${ROOT_DIR}/dist"
APPDIR="${BUILD_DIR}/AppDir"
TOOLS_DIR="${BUILD_DIR}/tools"

# SDL3 is not packaged on most distros yet, so build it from source by
# default; set AURORA_SDL3_PROVIDER=system where a system SDL3 exists.
SDL3_PROVIDER="${AURORA_SDL3_PROVIDER:-vendor}"

echo "=== Building Melee PC (Linux x86-64) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DAURORA_SDL3_PROVIDER="${SDL3_PROVIDER}" \
    -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-package}" \
    -DAURORA_NOD_PROVIDER="${AURORA_NOD_PROVIDER:-package}"
ninja -C "${BUILD_DIR}" melee

echo "=== Fetching packaging tools ==="
mkdir -p "${TOOLS_DIR}" "${DIST_DIR}"
if [[ ! -x "${TOOLS_DIR}/appimagetool" ]]; then
    curl -fL "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" -o "${TOOLS_DIR}/appimagetool"
    chmod +x "${TOOLS_DIR}/appimagetool"
fi
if [[ ! -x "${TOOLS_DIR}/linuxdeploy" ]]; then
    curl -fL "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" -o "${TOOLS_DIR}/linuxdeploy"
    chmod +x "${TOOLS_DIR}/linuxdeploy"
fi
# CI runners have no FUSE, so the AppImage tools must self-extract instead.
if [[ ! -e /dev/fuse ]]; then
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

echo "=== Staging AppDir ==="
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}"

"${TOOLS_DIR}/linuxdeploy" \
    --appdir "${APPDIR}" \
    -e "${BUILD_DIR}/melee" \
    -i "${ROOT_DIR}/platforms/linux/melee.png" \
    -d "${ROOT_DIR}/platforms/linux/melee.desktop"

# Copy resources beside the binary
cp -r "${ROOT_DIR}/resources" "${APPDIR}/usr/bin/resources"
gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
    > "${APPDIR}/usr/bin/initial_pipeline_cache.db"

echo "=== Generating AppImage ==="
ARCH=x86_64 "${TOOLS_DIR}/appimagetool" "${APPDIR}" "${DIST_DIR}/Melee-x86_64.AppImage"

echo "=== Generating Portable Tarball ==="
TAR_STAGE="${BUILD_DIR}/melee-linux-x86_64"
rm -rf "${TAR_STAGE}"
mkdir -p "${TAR_STAGE}"
cp "${BUILD_DIR}/melee" "${TAR_STAGE}/"
# RelWithDebInfo leaves ~180MB of DWARF in the binary; ship it stripped.
strip --strip-debug "${TAR_STAGE}/melee"
cp -r "${ROOT_DIR}/resources" "${TAR_STAGE}/"
gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
    > "${TAR_STAGE}/initial_pipeline_cache.db"
cp "${ROOT_DIR}/platforms/linux/melee.png" "${TAR_STAGE}/"
cp "${ROOT_DIR}/platforms/linux/melee.desktop" "${TAR_STAGE}/"
cat << 'APP_RUN' > "${TAR_STAGE}/run.sh"
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
exec "${HERE}/melee" "$@"
APP_RUN
chmod +x "${TAR_STAGE}/run.sh"

tar -czf "${DIST_DIR}/melee-linux-x86_64.tar.gz" -C "${BUILD_DIR}" "melee-linux-x86_64"

echo "=== Packaging Complete ==="
ls -lh "${DIST_DIR}"
