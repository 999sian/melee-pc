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
   cd platforms/android
   ./gradlew :app:assembleDebug
   ```
   The resulting APK will be placed at:
   `platforms/android/app/build/outputs/apk/debug/app-debug.apk`

4. **Running on Device**:
   ```bash
   adb install -r platforms/android/app/build/outputs/apk/debug/app-debug.apk
   ```
