#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/platforms/android"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/android-arm64}"

# Honour a preconfigured SDK/NDK (CI sets these); fall back to the local layout.
export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-${HOME}/Android}}"
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    ANDROID_NDK_HOME="$(find "${ANDROID_HOME}/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
fi
if [[ ! -d "${ANDROID_NDK_HOME}" ]]; then
    echo "error: no Android NDK found; set ANDROID_NDK_HOME" >&2
    exit 1
fi
export ANDROID_NDK_HOME
if [[ -d "${HOME}/Android/jdk17" && -z "${JAVA_HOME:-}" ]]; then
    export JAVA_HOME="${HOME}/Android/jdk17"
fi
[[ -n "${JAVA_HOME:-}" ]] && export PATH="${JAVA_HOME}/bin:${PATH}"

# The decomp needs GCC's scalar_storage_order, so melee_game is compiled by an
# aarch64 cross GCC (see tools/gcc_launcher.py) rather than the NDK's Clang.
# CI installs gcc-aarch64-linux-gnu; a local unpacked toolchain also works.
if [[ -z "${GCC_AARCH64_BIN:-}" ]] && ! command -v aarch64-linux-gnu-gcc >/dev/null; then
    GCC_AARCH64_BIN="${HOME}/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc"
    [[ -x "${GCC_AARCH64_BIN}" ]] || {
        echo "error: no aarch64 GCC; install gcc-aarch64-linux-gnu or set GCC_AARCH64_BIN" >&2
        exit 1
    }
    export GCC_AARCH64_BIN
fi

STRIP_TOOL="$(find "${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt" -name llvm-strip -print -quit)"
echo "=== NDK ${ANDROID_NDK_HOME} ==="

echo "=== Building native library (arm64-v8a) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DAURORA_SDL3_PROVIDER=vendor \
    -DAURORA_SDL3_LINKAGE=static \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384" \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
SDL_JAVA_SOURCE_DIR="$(sed -n 's/^AURORA_SDL3_JAVA_SOURCE_DIR:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")"
SDL_VERSION_PATTERN='private static final int SDL_(MAJOR|MINOR|MICRO)_VERSION ='
SDL_NATIVE_VERSION="$(grep -E "${SDL_VERSION_PATTERN}" "${SDL_JAVA_SOURCE_DIR}/org/libsdl/app/SDLActivity.java")"
SDL_APP_VERSION="$(grep -E "${SDL_VERSION_PATTERN}" "${ANDROID_DIR}/app/src/main/java/org/libsdl/app/SDLActivity.java")"
if [[ "${SDL_NATIVE_VERSION}" != "${SDL_APP_VERSION}" ]]; then
    echo "error: Android SDL Java version does not match the native SDL source" >&2
    exit 1
fi
ninja -C "${BUILD_DIR}" melee

echo "=== Staging assets and native libraries ==="
mkdir -p "${ANDROID_DIR}/app/src/main/assets/resources"
cp -r "${ROOT_DIR}/resources/"* "${ANDROID_DIR}/app/src/main/assets/"
cp -r "${ROOT_DIR}/resources/"* "${ANDROID_DIR}/app/src/main/assets/resources/"
# Keep the unrestricted desktop inventory out of Android. The separate starter
# is checked for the current version and structure size again by the native loader before any driver compilation.
rm -f "${ANDROID_DIR}/app/src/main/assets/initial_pipeline_cache.db" \
    "${ANDROID_DIR}/app/src/main/assets/resources/initial_pipeline_cache.db"
gzip -dc "${ROOT_DIR}/tools/android_initial_pipeline_cache.db.gz" \
    > "${ANDROID_DIR}/app/src/main/assets/android_initial_pipeline_cache.db"

mkdir -p "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a"
"${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libmelee.so" "${BUILD_DIR}/libmelee.so"
if [[ -f "${BUILD_DIR}/_deps/png-build/libpng16.so" ]]; then
    "${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libpng16.so" "${BUILD_DIR}/_deps/png-build/libpng16.so"
fi

echo "=== Preparing release signing key ==="
KEYSTORE="${MELEE_KEYSTORE_FILE:-${ANDROID_DIR}/melee-release.keystore}"
if [[ -n "${MELEE_KEYSTORE_BASE64:-}" ]]; then
    # CI path: the keystore lives in repository secrets, never in the tree.
    base64 -d <<< "${MELEE_KEYSTORE_BASE64}" > "${KEYSTORE}"
elif [[ -z "${MELEE_KEYSTORE_FILE:-}" && -f "${ANDROID_DIR}/release-signing.env" ]]; then
    # Local path: passwords sit next to the (gitignored) keystore.
    set -a; source "${ANDROID_DIR}/release-signing.env"; set +a
fi
if [[ ! -f "${KEYSTORE}" && "${MELEE_THROWAWAY_KEY:-}" == 1 ]]; then
    # Fork PRs get no repository secrets. Sign with a key made for this build
    # so the APK still builds; it lives in the build tree, never in the place
    # of the release keystore, and cannot update an installed release.
    KEYSTORE="${BUILD_DIR}/throwaway.keystore"
    rm -f "${KEYSTORE}"
    MELEE_KEYSTORE_PASSWORD=throwaway MELEE_KEY_PASSWORD=throwaway MELEE_KEY_ALIAS=throwaway
    keytool -genkeypair -noprompt -keystore "${KEYSTORE}" -storetype PKCS12 \
        -alias "${MELEE_KEY_ALIAS}" -keyalg RSA -keysize 2048 -validity 30 \
        -storepass "${MELEE_KEYSTORE_PASSWORD}" -keypass "${MELEE_KEY_PASSWORD}" \
        -dname "CN=Melee throwaway build key" > /dev/null
    export MELEE_KEYSTORE_FILE="$(realpath "${KEYSTORE}")"
    echo "warning: no release key; signing with a throwaway key" >&2
fi
if [[ ! -f "${KEYSTORE}" ]]; then
    echo "error: no signing key; set MELEE_KEYSTORE_BASE64 or create ${KEYSTORE}" >&2
    exit 1
fi
export MELEE_KEYSTORE_FILE="$(realpath "${KEYSTORE}")"
export MELEE_KEYSTORE_PASSWORD MELEE_KEY_ALIAS MELEE_KEY_PASSWORD

echo "=== Building Melee Android APK ==="
cd "${ANDROID_DIR}"
./gradlew --no-daemon :app:assembleRelease || {
    echo "Gradle assembleRelease failed, retrying once after 5s..."
    sleep 5
    ./gradlew --no-daemon :app:assembleRelease --stacktrace
}

APK="${ROOT_DIR}/dist/Melee-Android-arm64.apk"
mkdir -p "${ROOT_DIR}/dist"
cp "${ANDROID_DIR}/app/build/outputs/apk/release/app-release.apk" "${APK}"

# A release APK that silently came out unsigned would fail to install.
APKSIGNER="$(find "${ANDROID_HOME}/build-tools" -name apksigner -print -quit 2>/dev/null || true)"
if [[ -n "${APKSIGNER}" ]]; then
    "${APKSIGNER}" verify --print-certs "${APK}" | head -4
fi

echo "=== APK build complete ==="
ls -lh "${APK}"
