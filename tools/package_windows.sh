#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${ROOT_DIR}/dist"
TARGET_ARCH="${TARGET_ARCH:-x86_64}"

case "${TARGET_ARCH}" in
    x86_64|amd64)
        TARGET_ARCH="x86_64"
        BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-win}"
        STAGE_DIR="${DIST_DIR}/melee-windows-x86_64"
        ZIP_NAME="Melee-Windows-x86_64.zip"
        TOOLCHAIN_FILE="${ROOT_DIR}/cmake/x86_64-w64-mingw32.cmake"
        SDL3_PROVIDER="package"
        DAWN_PROVIDER="package"
        NOD_PROVIDER="package"
        CXX_BIN="${CXX:-x86_64-w64-mingw32-g++}"
        VCREDIST_URL="${VCREDIST_URL:-https://aka.ms/vs/17/release/vc_redist.x64.exe}"
        VCREDIST_EXE="vc_redist.x64.exe"
        CAB_ARCH="amd64"
        OBJDUMP_BIN="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
        MINGW_DLLS=("libwinpthread-1.dll")
        ;;
    arm64|aarch64)
        TARGET_ARCH="arm64"
        BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build-win-arm64}"
        STAGE_DIR="${DIST_DIR}/melee-windows-arm64"
        ZIP_NAME="Melee-Windows-arm64.zip"
        TOOLCHAIN_FILE="${ROOT_DIR}/cmake/aarch64-w64-mingw32.cmake"
        SDL3_PROVIDER="vendor"
        DAWN_PROVIDER="package"
        NOD_PROVIDER="package"
        CXX_BIN="${CXX:-aarch64-w64-mingw32-clang++}"
        VCREDIST_URL="${VCREDIST_URL:-https://aka.ms/vs/17/release/vc_redist.arm64.exe}"
        VCREDIST_EXE="vc_redist.arm64.exe"
        CAB_ARCH="arm64"
        OBJDUMP_BIN="${OBJDUMP:-llvm-objdump}"
        if ! command -v "${OBJDUMP_BIN}" >/dev/null && command -v aarch64-w64-mingw32-objdump >/dev/null; then
            OBJDUMP_BIN="aarch64-w64-mingw32-objdump"
        fi
        MINGW_DLLS=("libwinpthread-1.dll" "libc++.dll" "libunwind.dll")
        ;;
    *)
        echo "error: unsupported TARGET_ARCH: ${TARGET_ARCH}" >&2
        exit 1
        ;;
esac

echo "=== Building Windows release (${TARGET_ARCH}) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DAURORA_SDL3_PROVIDER="${SDL3_PROVIDER}" \
    -DAURORA_DAWN_PROVIDER="${DAWN_PROVIDER}" \
    -DAURORA_NOD_PROVIDER="${NOD_PROVIDER}"
ninja -C "${BUILD_DIR}" melee

echo "=== Staging Windows package ==="
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"

cp "${BUILD_DIR}/melee.exe" "${STAGE_DIR}/"
cp -r "${ROOT_DIR}/resources" "${STAGE_DIR}/"
# A double-clicked run loses its console on exit, so ship a launcher that
# turns on verbose logging, records the environment, and stays open.
cp "${ROOT_DIR}/tools/windows/RUN-AND-LOG.bat" "${STAGE_DIR}/"

# Without a seed, every pipeline is compiled the first time it is used, which
# is what players hit as stuttering. Machines that have played for a while
# have a warm pipeline_cache.db and never see it, so the bug is invisible to
# us. The blob is aurora's own backend-independent pipeline descriptors, so
# one recorded anywhere seeds D3D12 just as well.
gzip -dc "${ROOT_DIR}/tools/initial_pipeline_cache.db.gz" \
    > "${STAGE_DIR}/initial_pipeline_cache.db"

# Dawn, SDL3, zlib/png DLLs land in the build root via AuroraCopyRuntimeDLLs.
for dll in dxcompiler.dll dxil.dll webgpu_dawn.dll SDL3.dll libpng16.dll libzlib1.dll; do
    if [[ -f "${BUILD_DIR}/${dll}" ]]; then
        cp "${BUILD_DIR}/${dll}" "${STAGE_DIR}/"
    fi
done

if [[ -f "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" ]]; then
    cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/"
    cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/libnod.dll"
elif [[ -f "${BUILD_DIR}/libnod.dll" ]]; then
    cp "${BUILD_DIR}/libnod.dll" "${STAGE_DIR}/"
    cp "${BUILD_DIR}/libnod.dll" "${STAGE_DIR}/nod.dll"
elif [[ -f "${BUILD_DIR}/nod.dll" ]]; then
    cp "${BUILD_DIR}/nod.dll" "${STAGE_DIR}/"
    cp "${BUILD_DIR}/nod.dll" "${STAGE_DIR}/libnod.dll"
fi

echo "=== Locating MinGW runtime DLLs ==="
for dll in "${MINGW_DLLS[@]}"; do
    src=""
    if command -v "${CXX_BIN}" >/dev/null; then
        cand="$("${CXX_BIN}" -print-file-name="${dll}" 2>/dev/null || true)"
        if [[ -f "${cand}" ]]; then
            src="${cand}"
        fi
    fi
    if [[ -z "${src}" ]]; then
        cxx_path="$(command -v "${CXX_BIN}" 2>/dev/null || true)"
        if [[ -n "${cxx_path}" ]]; then
            bin_dir="$(dirname "${cxx_path}")"
            parent_dir="$(dirname "${bin_dir}")"
            for search_dir in "${parent_dir}/aarch64-w64-mingw32/bin" "${parent_dir}/x86_64-w64-mingw32/bin" "${bin_dir}"; do
                if [[ -f "${search_dir}/${dll}" ]]; then
                    src="${search_dir}/${dll}"
                    break
                fi
            done
        fi
    fi
    if [[ -z "${src}" ]]; then
        # Check system / toolchains dirs
        src="$(find /usr/x86_64-w64-mingw32/bin /usr/aarch64-w64-mingw32/bin /home/sian/toolchains -name "${dll}" -print -quit 2>/dev/null || true)"
    fi
    if [[ ! -f "${src}" ]]; then
        echo "error: ${dll} not found for ${CXX_BIN}" >&2
        exit 1
    fi
    echo "  ${dll} <- ${src}"
    cp "${src}" "${STAGE_DIR}/"
done

echo "=== Fetching Visual C++ runtime DLLs ==="
VCREDIST_DIR="${VCREDIST_DIR:-${BUILD_DIR}/vcredist}"
mkdir -p "${VCREDIST_DIR}"
if [[ ! -f "${VCREDIST_DIR}/VCRUNTIME140.dll" ]]; then
    [[ -f "${VCREDIST_DIR}/${VCREDIST_EXE}" ]] ||
        curl -sSLf -o "${VCREDIST_DIR}/${VCREDIST_EXE}" "${VCREDIST_URL}"
    python3 - "${VCREDIST_DIR}" "${VCREDIST_EXE}" "${CAB_ARCH}" <<'PY'
import pathlib, shutil, subprocess, sys

out = pathlib.Path(sys.argv[1])
exe_name = sys.argv[2]
cab_arch = sys.argv[3]
data = (out / exe_name).read_bytes()
want = {
    'vcruntime140.dll': 'VCRUNTIME140.dll',
    'vcruntime140_1.dll': 'VCRUNTIME140_1.dll',
    'msvcp140.dll': 'MSVCP140.dll',
    'msvcp140_atomic_wait.dll': 'MSVCP140_ATOMIC_WAIT.dll',
}
offsets, i = [], 0
while (i := data.find(b'MSCF', i)) >= 0:
    offsets.append(i)
    i += 4

work = out / 'extract'
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)
found = {}
filter_pattern = f'*_{cab_arch}'
for off in reversed(offsets):
    cab = work / 'container.cab'
    cab.write_bytes(data[off:])
    inner = work / 'inner'
    shutil.rmtree(inner, ignore_errors=True)
    inner.mkdir()
    quiet = {'check': False, 'capture_output': True}
    subprocess.run(['cabextract', '-q', '-d', str(inner), str(cab)], **quiet)
    for nested in sorted(p for p in inner.iterdir() if p.is_file()):
        subprocess.run(['cabextract', '-q', '-d', str(work), '-F', filter_pattern,
                        str(nested)], **quiet)
    for payload in work.glob(filter_pattern):
        dest = want.get(payload.name[:-len(f'_{cab_arch}')].lower())
        if dest:
            found[dest] = payload
    if len(found) == len(want):
        break

missing = sorted(set(want.values()) - set(found))
if missing:
    sys.exit(f'error: {", ".join(missing)} not found in {exe_name}')
for dest, payload in found.items():
    shutil.copy(payload, out / dest)
shutil.rmtree(work, ignore_errors=True)
PY
fi
for dll in VCRUNTIME140.dll VCRUNTIME140_1.dll MSVCP140.dll MSVCP140_ATOMIC_WAIT.dll; do
    echo "  ${dll}"
    cp "${VCREDIST_DIR}/${dll}" "${STAGE_DIR}/"
done

echo "=== Refusing a determinism test build ==="
# A build configured with -DMELEE_FP_PERTURB=<fn> (CMakeLists.txt) returns that
# trig function one ULP off on purpose, so it desyncs against every other
# build. The marker is the banner src/pc/net.c logs on every run of such a
# build, which only exists in the image when the option was on.
if grep -qa "FP PERTURB" "${STAGE_DIR}/melee.exe"; then
    echo "error: melee.exe was built with MELEE_FP_PERTURB; it desyncs by design" >&2
    exit 1
fi
echo "  clean (no MELEE_FP_PERTURB marker)"

echo "=== Verifying the package resolves on a clean Windows ==="
python3 - "${STAGE_DIR}" "${OBJDUMP_BIN}" <<'PY'
import pathlib, re, subprocess, sys

stage = pathlib.Path(sys.argv[1])
objdump_bin = sys.argv[2]
# Present on a clean Windows 10/11 install (x64 and ARM64).
OS_DLLS = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll',
    'ole32.dll', 'oleaut32.dll', 'ntdll.dll', 'winmm.dll', 'version.dll',
    'imm32.dll', 'setupapi.dll', 'bcrypt.dll', 'bcryptprimitives.dll',
    'dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'ucrtbase.dll', 'ws2_32.dll',
    'crypt32.dll', 'shlwapi.dll', 'msvcrt.dll', 'rpcrt4.dll', 'userenv.dll',
    'cfgmgr32.dll', 'dwmapi.dll', 'uxtheme.dll', 'powrprof.dll', 'dbghelp.dll',
    # iphlpapi: GetAdaptersAddresses (src/pc/net_lan.c). Ships with Windows
    # since 2000; it appeared in our import table when LAN discovery landed,
    # which is why the gate started failing on a package that is in fact fine.
    'winhttp.dll', 'iphlpapi.dll',
}

def is_os(name):
    n = name.lower()
    return n in OS_DLLS or n.startswith(('api-ms-win-', 'ext-ms-'))

binaries = [p for p in sorted(stage.iterdir())
            if p.suffix.lower() in ('.dll', '.exe')]
shipped = {p.name.lower() for p in binaries}
gaps = {}
for binary in binaries:
    dump = subprocess.run([objdump_bin, '-p', str(binary)],
                          capture_output=True, text=True, check=True).stdout
    for dep in sorted(set(re.findall(r'DLL Name:\s*(\S+)', dump))):
        if dep.lower() not in shipped and not is_os(dep):
            gaps.setdefault(dep, []).append(binary.name)

if gaps:
    for dep, users in sorted(gaps.items()):
        print(f'  MISSING {dep} <- {", ".join(users)}', file=sys.stderr)
    sys.exit('error: package depends on DLLs it does not ship')
print(f'  {len(binaries)} binaries, all imports resolve')
PY

echo "=== Creating Windows ZIP package ==="
cd "${DIST_DIR}"
rm -f "${ZIP_NAME}"
stage_basename="$(basename "${STAGE_DIR}")"
if command -v 7z >/dev/null; then
    7z a -tzip "${ZIP_NAME}" "${stage_basename}"
else
    zip -qr "${ZIP_NAME}" "${stage_basename}"
fi

echo "=== Windows package successfully created at ${DIST_DIR}/${ZIP_NAME} ==="
ls -lh "${DIST_DIR}/${ZIP_NAME}"
