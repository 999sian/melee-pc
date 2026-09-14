# Android Build Guide for Melee PC

This directory contains the Android application wrapper for Super Smash Bros. Melee PC.

## Architecture & Requirements

1. **Big-Endian Data Structures**:
   - The decompiled Melee codebase relies on GCC's `__attribute__((scalar_storage_order("big-endian")))` to transparently load and read GameCube big-endian disc structures.
   - Modern Android NDK ships only with Clang (which does not support `scalar_storage_order`).
   - Compiling `libmelee.so` for Android requires a GCC toolchain targeting Android/aarch64 (such as `aarch64-linux-gnu-gcc` cross-compiler configured with Android sysroot) or building under an environment providing GNU C.

2. **Native Libraries**:
   - Once `libmelee.so` and runtime dependencies (`libSDL3.so`, etc.) are built, place them in:
     `platforms/android/app/libs/arm64-v8a/`

3. **Building the APK**:
   ```bash
   tools/build_android.sh
   ```
   This builds the native library, stages assets, and produces a signed
   release APK at `dist/Melee-Android-arm64.apk`.

   Signing key resolution, in order:
   - `MELEE_KEYSTORE_BASE64` (what CI sets from repository secrets), or
   - `platforms/android/melee-release.keystore` plus a
     `platforms/android/release-signing.env` holding
     `MELEE_KEYSTORE_PASSWORD`, `MELEE_KEY_ALIAS`, and `MELEE_KEY_PASSWORD`.

   Both are gitignored. Generate one with:
   ```bash
   keytool -genkeypair -keystore platforms/android/melee-release.keystore \
       -storetype PKCS12 -alias melee -keyalg RSA -keysize 4096 -validity 10000
   ```

4. **Running on Device**:
   ```bash
   adb install -r dist/Melee-Android-arm64.apk
   ```
