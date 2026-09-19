#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
DIST_DIR="${ROOT_DIR}/dist"
APPDIR="${BUILD_DIR}/AppDir"
TOOLS_DIR="${BUILD_DIR}/tools"

ARCH="$(uname -m)"
case "${ARCH}" in
    x86_64|aarch64) ;;
    *) echo "Unsupported architecture: ${ARCH}" >&2; exit 1 ;;
esac

# SDL3 is not packaged on most distros yet, so build it from source by
# default; set AURORA_SDL3_PROVIDER=system where a system SDL3 exists.
SDL3_PROVIDER="${AURORA_SDL3_PROVIDER:-vendor}"

echo "=== Building Melee PC (Linux ${ARCH}) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DAURORA_SDL3_PROVIDER="${SDL3_PROVIDER}" \
    -DAURORA_DAWN_PROVIDER="${AURORA_DAWN_PROVIDER:-package}" \
    -DAURORA_NOD_PROVIDER="${AURORA_NOD_PROVIDER:-package}"
ninja -C "${BUILD_DIR}" melee

echo "=== Fetching packaging tools ==="
mkdir -p "${TOOLS_DIR}" "${DIST_DIR}"
if [[ ! -x "${TOOLS_DIR}/appimagetool" ]]; then
    curl -fL "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage" -o "${TOOLS_DIR}/appimagetool"
    chmod +x "${TOOLS_DIR}/appimagetool"
fi
if [[ ! -x "${TOOLS_DIR}/linuxdeploy" ]]; then
    curl -fL "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage" -o "${TOOLS_DIR}/linuxdeploy"
    chmod +x "${TOOLS_DIR}/linuxdeploy"
fi
# CI runners have no FUSE, so the AppImage tools must self-extract instead.
if [[ ! -e /dev/fuse ]]; then
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

echo "=== Staging AppDir ==="
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}"

# Locate libusb-1.0.so.0 for bundling (dlopened at runtime by SDL3's hidapi)
LIBUSB_PATH="$(ldconfig -p 2>/dev/null | grep -E 'libusb-1\.0\.so\.0' | head -n1 | awk '{print $NF}' || true)"
if [[ -z "${LIBUSB_PATH}" || ! -f "${LIBUSB_PATH}" ]]; then
    LIBUSB_PATH="$(find /usr/lib* /lib* -name "libusb-1.0.so.0" 2>/dev/null | head -n1 || true)"
fi

EXTRA_LINUXDEPLOY_ARGS=()
if [[ -n "${LIBUSB_PATH}" && -f "${LIBUSB_PATH}" ]]; then
    echo "Found libusb for bundling: ${LIBUSB_PATH}"
    EXTRA_LINUXDEPLOY_ARGS+=("-l" "${LIBUSB_PATH}")
else
    echo "Warning: libusb-1.0.so.0 not found on host; AppImage will rely on host library"
fi

"${TOOLS_DIR}/linuxdeploy" \
    --appdir "${APPDIR}" \
    -e "${BUILD_DIR}/melee" \
    -i "${ROOT_DIR}/platforms/linux/melee.png" \
    -d "${ROOT_DIR}/platforms/linux/melee.desktop" \
    "${EXTRA_LINUXDEPLOY_ARGS[@]}"

# Copy resources beside the binary
cp -r "${ROOT_DIR}/resources" "${APPDIR}/usr/bin/resources"
gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
    > "${APPDIR}/usr/bin/initial_pipeline_cache.db"

echo "=== Generating AppImage ==="
ARCH="${ARCH}" "${TOOLS_DIR}/appimagetool" "${APPDIR}" "${DIST_DIR}/Melee-${ARCH}.AppImage"

echo "=== Generating Portable Tarball ==="
TAR_STAGE="${BUILD_DIR}/melee-linux-${ARCH}"
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
if [[ -n "${LIBUSB_PATH}" && -f "${LIBUSB_PATH}" ]]; then
    mkdir -p "${TAR_STAGE}/lib"
    cp -L "${LIBUSB_PATH}" "${TAR_STAGE}/lib/libusb-1.0.so.0"
fi
cat << 'APP_RUN' > "${TAR_STAGE}/run.sh"
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
if [[ -d "${HERE}/lib" ]]; then
    export LD_LIBRARY_PATH="${HERE}/lib:${LD_LIBRARY_PATH:-}"
fi
exec "${HERE}/melee" "$@"
APP_RUN
chmod +x "${TAR_STAGE}/run.sh"

tar -czf "${DIST_DIR}/melee-linux-${ARCH}.tar.gz" -C "${BUILD_DIR}" "melee-linux-${ARCH}"

echo "=== Packaging Complete ==="
ls -lh "${DIST_DIR}"
