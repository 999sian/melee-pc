#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
DIST_DIR="${ROOT_DIR}/dist"
APPDIR="${BUILD_DIR}/AppDir"
TOOLS_DIR="${BUILD_DIR}/tools"

echo "=== Building Melee PC (Linux x86-64) ==="
cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
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

echo "=== Generating AppImage ==="
ARCH=x86_64 "${TOOLS_DIR}/appimagetool" "${APPDIR}" "${DIST_DIR}/Melee-x86_64.AppImage"

echo "=== Generating Portable Tarball ==="
TAR_STAGE="${BUILD_DIR}/melee-linux-x86_64"
rm -rf "${TAR_STAGE}"
mkdir -p "${TAR_STAGE}"
cp "${BUILD_DIR}/melee" "${TAR_STAGE}/"
cp -r "${ROOT_DIR}/resources" "${TAR_STAGE}/"
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
