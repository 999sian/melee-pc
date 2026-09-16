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
    cp "${BUILD_DIR}/${dll}" "${STAGE_DIR}/"
done
cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/"
cp "${BUILD_DIR}/_deps/nod_prebuilt-src/bin/nod.dll" "${STAGE_DIR}/libnod.dll"

# MinGW runtime DLLs. libstdc++ and libgcc are linked statically (see
# CMakeLists.txt), so only libwinpthread is still imported.
#
# Shipping libstdc++-6.dll used to pick it with `find -print -quit`, which is
# not safe on Debian/Ubuntu: those carry both a win32-threads and a
# posix-threads build of the same filename, and the first hit won. CI selects
# the posix compiler because aurora's C++20 needs std::thread, so the win32
# copy landed beside an exe built against the posix one and the first
# std::ifstream died in basic_ios::init. Derive the path from the compiler
# actually used instead of searching for a name.
echo "=== Locating MinGW runtime DLLs ==="
CXX_BIN="${CXX:-x86_64-w64-mingw32-g++}"
for dll in libwinpthread-1.dll; do
    src="$("${CXX_BIN}" -print-file-name="${dll}" 2>/dev/null || true)"
    if [[ ! -f "${src}" ]]; then
        # Arch keeps them in the sysroot bin rather than beside the compiler.
        src="$(find /usr/x86_64-w64-mingw32/bin -name "${dll}" -print -quit 2>/dev/null || true)"
    fi
    if [[ ! -f "${src}" ]]; then
        echo "error: ${dll} not found for ${CXX_BIN}" >&2
        exit 1
    fi
    echo "  ${dll} <- ${src}"
    cp "${src}" "${STAGE_DIR}/"
done

# The MSVC-built prebuilts (Dawn, dxcompiler, SDL3, nod) import the Visual C++
# runtime, which is NOT part of Windows -- it comes from the VC++ 2015-2022
# redistributable. Wine and Proton provide it, so the omission only shows up on
# a real Windows box, as "VCRUNTIME140.dll was not found". Microsoft permits
# app-local deployment of these DLLs, so ship them beside melee.exe.
echo "=== Fetching Visual C++ runtime DLLs ==="
VCREDIST_DIR="${VCREDIST_DIR:-${BUILD_DIR}/vcredist}"
VCREDIST_URL="${VCREDIST_URL:-https://aka.ms/vs/17/release/vc_redist.x64.exe}"
mkdir -p "${VCREDIST_DIR}"
if [[ ! -f "${VCREDIST_DIR}/VCRUNTIME140.dll" ]]; then
    [[ -f "${VCREDIST_DIR}/vc_redist.x64.exe" ]] ||
        curl -sSLf -o "${VCREDIST_DIR}/vc_redist.x64.exe" "${VCREDIST_URL}"
    python3 - "${VCREDIST_DIR}" <<'PY'
import pathlib, shutil, subprocess, sys

# vc_redist.x64.exe is a Burn bundle: a UX cabinet, then an attached cabinet
# holding per-architecture cabinets of <name>.dll_<arch> payloads. Slice at
# each cabinet header, newest first, until the x64 runtime turns up.
out = pathlib.Path(sys.argv[1])
data = (out / 'vc_redist.x64.exe').read_bytes()
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
for off in reversed(offsets):
    cab = work / 'container.cab'
    cab.write_bytes(data[off:])
    inner = work / 'inner'
    shutil.rmtree(inner, ignore_errors=True)
    inner.mkdir()
    # Most members are not cabinets (the bundle also carries MSIs), so a
    # "no valid cabinets found" miss is expected and not worth printing.
    quiet = {'check': False, 'capture_output': True}
    subprocess.run(['cabextract', '-q', '-d', str(inner), str(cab)], **quiet)
    for nested in sorted(p for p in inner.iterdir() if p.is_file()):
        subprocess.run(['cabextract', '-q', '-d', str(work), '-F', '*_amd64',
                        str(nested)], **quiet)
    for payload in work.glob('*_amd64'):
        dest = want.get(payload.name[:-len('_amd64')].lower())
        if dest:
            found[dest] = payload
    if len(found) == len(want):
        break

missing = sorted(set(want.values()) - set(found))
if missing:
    sys.exit(f'error: {", ".join(missing)} not found in vc_redist.x64.exe')
for dest, payload in found.items():
    shutil.copy(payload, out / dest)
shutil.rmtree(work, ignore_errors=True)
PY
fi
for dll in VCRUNTIME140.dll VCRUNTIME140_1.dll MSVCP140.dll MSVCP140_ATOMIC_WAIT.dll; do
    echo "  ${dll}"
    cp "${VCREDIST_DIR}/${dll}" "${STAGE_DIR}/"
done

# Every import must resolve to something we ship or something Windows itself
# provides. Wine and Proton quietly supply extras (this is how a package
# missing the VC++ runtime passed local testing), so gate the package here
# rather than discovering it on a user's machine.
echo "=== Verifying the package resolves on a clean Windows ==="
python3 - "${STAGE_DIR}" <<'PY'
import pathlib, re, subprocess, sys

stage = pathlib.Path(sys.argv[1])
# Present on a clean Windows 10/11 x64 install.
OS_DLLS = {
    'kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'shell32.dll',
    'ole32.dll', 'oleaut32.dll', 'ntdll.dll', 'winmm.dll', 'version.dll',
    'imm32.dll', 'setupapi.dll', 'bcrypt.dll', 'bcryptprimitives.dll',
    'dxgi.dll', 'd3d11.dll', 'd3d12.dll', 'ucrtbase.dll', 'ws2_32.dll',
    'crypt32.dll', 'shlwapi.dll', 'msvcrt.dll', 'rpcrt4.dll', 'userenv.dll',
    'cfgmgr32.dll', 'dwmapi.dll', 'uxtheme.dll', 'powrprof.dll', 'dbghelp.dll',
    'winhttp.dll',
}

def is_os(name):
    n = name.lower()
    return n in OS_DLLS or n.startswith(('api-ms-win-', 'ext-ms-'))

binaries = [p for p in sorted(stage.iterdir())
            if p.suffix.lower() in ('.dll', '.exe')]
shipped = {p.name.lower() for p in binaries}
gaps = {}
for binary in binaries:
    dump = subprocess.run(['x86_64-w64-mingw32-objdump', '-p', str(binary)],
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
rm -f "Melee-Windows-x86_64.zip"
if command -v 7z >/dev/null; then
    7z a -tzip "Melee-Windows-x86_64.zip" "melee-windows-x86_64"
else
    zip -qr "Melee-Windows-x86_64.zip" "melee-windows-x86_64"
fi

echo "=== Windows package successfully created at ${DIST_DIR}/Melee-Windows-x86_64.zip ==="
ls -lh "${DIST_DIR}/Melee-Windows-x86_64.zip"
