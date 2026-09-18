# Building

Needs GCC (the game code relies on `scalar_storage_order("big-endian")`, which
only GCC implements), CMake 3.25+, Ninja, and a Vulkan driver. Aurora fetches
its own Dawn/SDL3/nod prebuilts. `python3 tools/preflight.py` checks all of it
and prints the fix for anything missing.

```sh
cmake -B build -G Ninja
ninja -C build
build/melee                              # open the launcher
build/melee <disc.iso|.gcm|.ciso|.rvz>
```

No disc data is needed to build. The two HSD font atlases are pixel data from
the retail DOL, so instead of being committed they are read out of the disc
you supply, at boot (`src/pc/discfont.c`).

Keep `resources/` next to the binary when distributing. The bundled Liberation
Sans fonts are covered by `resources/FONT-LICENSE.txt`.

`MELEE_ENABLE_LTO` turns on link-time optimisation. Unit tests are described in
[testing.md](testing.md).

## Supported toolchain versions

GCC is not a preference, it is the only compiler that implements
`scalar_storage_order`, and every on-disc struct depends on it. There is no
single blessed version: the table is what CI builds the release artifacts with
and what the port is developed on, and both are supported.

| Component | CI (`.github/workflows/build.yml`) | Also tested | Floor |
| --- | --- | --- | --- |
| Linux runner | `ubuntu-24.04`, `ubuntu-24.04-arm` | Arch (GCC 16.2.1, CMake 4.4.3) | - |
| GCC (game C) | Ubuntu 24.04 `build-essential` (GCC 13.x) | 16.2.1 | 12 (aurora's C++20 needs libstdc++ 12) |
| CMake | Ubuntu 24.04 (3.28) | 4.4.3 | 3.25 (`cmake_minimum_required`) |
| Ninja | `ninja-build` | 1.13.2 | any |
| Python | 3.12 | 3.14 | 3.8 (`tools/`) |
| Windows x86-64 | Ubuntu 24.04 `mingw-w64`, POSIX threads | MinGW GCC 16.2.0 | - |
| Windows ARM64 | llvm-mingw `20260908` ucrt + `gcc-aarch64-linux-gnu` | - | that llvm-mingw release |
| Android | NDK `26.3.11579264`, temurin JDK 17, `gcc-aarch64-linux-gnu`, rustup `aarch64-linux-android` | - | exactly r26 (newer NDK libc++ has no `std::jthread`) |
| iOS | clang/lld/llvm 19, theos `iPhoneOS16.5.sdk`, `gcc-aarch64-linux-gnu` (14 preferred), rustup `aarch64-apple-ios` | - | - |
| macOS | `macos-26` (arm64), `macos-15-intel`; Xcode clang + Homebrew `gcc cmake ninja sdl3 zstd libpng freetype` | - | Homebrew gcc 13+ |

Anything newer than the CI row generally works - the port is developed on GCC
16 - but only the CI row is what releases are built from.

## Preflight

`tools/preflight.py` checks the host against that table. Per check it prints
what was found, what is required and the exact install command, and exits
nonzero if anything failed:

```sh
python3 tools/preflight.py                  # host target
python3 tools/preflight.py --target windows  # or linux, android, ios, macos
```

It also compiles `__attribute__((scalar_storage_order("big-endian")))` for
real, because Clang accepts the syntax and silently ignores it. Fix commands
use the Debian/Ubuntu package names CI installs.

## When the wrong compiler is selected

`cmake/MeleeToolchainCheck.cmake` (included right after `project()`) fails the
configure instead of letting a Clang build produce byte-swapped disc data:

```
CMake Error at cmake/MeleeToolchainCheck.cmake:24 (message):
  melee-pc requires GCC as the C compiler.

    found:    Clang 22.1.8 (/usr/bin/clang)
    required: GNU (GCC 12 or newer)
    why:      the decomp's on-disc structs use
              __attribute__((scalar_storage_order("big-endian"))), which no other
              compiler implements; Clang ignores it and every disc field comes
              back byte-swapped at runtime.
    fix:      CC=gcc CXX=g++ cmake -B build -G Ninja   (delete the stale cache first)
              python3 tools/preflight.py                # checks the whole toolchain
```

A GCC that does not accept the attribute fails the same way, from a
`check_c_source_compiles` probe rather than the compiler id. Android, Apple and
Windows-ARM64 are exempt: they build the C++ with Clang on purpose and route
the game's C through GCC (see below).

## Container build

`Containerfile` is the CI Linux environment (`ubuntu:24.04` plus the `linux`
job's apt list), for a build that does not depend on the host distribution:

```sh
docker build -t melee-pc-build -f Containerfile .
docker run --rm -v "$PWD:/src" -w /src -u "$(id -u):$(id -g)" \
    -e BUILD_DIR=/src/build-container melee-pc-build tools/package_linux.sh
docker run --rm -it -v "$PWD:/src" -w /src melee-pc-build bash   # plain build
```

`BUILD_DIR` keeps the container's objects out of a `build/` configured by the
host compiler. There is no `/dev/fuse` in a container, so the AppImage tools
self-extract (`APPIMAGE_EXTRACT_AND_RUN=1`, already set in the image).
Reproducing a release byte for byte needs the base image pinned by digest; apt
version pins rot because Ubuntu's archive only keeps the current point release.

The image itself has not been built here (this workstation has no running
container daemon): what is verified is that its apt list is a strict superset
of the `linux` CI job's and that `tools/package_linux.sh` honours `BUILD_DIR`
and the fuseless AppImage path. Report anything it needs on top of that.

## Release packaging

The release artifacts are produced by the same scripts CI runs
(`.github/workflows/build.yml`), so they work locally too.

```sh
tools/package_linux.sh      # dist/Melee-<arch>.AppImage + melee-linux-<arch>.tar.gz
tools/package_windows.sh    # dist/Melee-Windows-<arch>.zip
tools/package_macos.sh      # dist/Melee-macOS-<arch>.zip (Melee.app)
tools/build_android.sh      # dist/Melee-Android-arm64.apk (signed release)
tools/build_ios.sh          # dist/Melee-iOS-arm64.ipa
```

`MELEE_VERSION` sets the version stamped into the Android and macOS packages
(CI uses the tag name).

### Linux

x86-64 and aarch64 are built natively on their own runners. Ubuntu 24.04 has no
SDL3 package, so aurora builds it from source (`AURORA_SDL3_PROVIDER=vendor`);
the X11/Wayland/audio development packages it needs are listed in the workflow.
The AppImage needs `file`, `zip` and `desktop-file-utils`.

### Windows (cross-compiled from Linux)

x86-64 uses MinGW-w64. aurora's C++20 code needs `std::thread`, which only the
POSIX threading model provides, so on Ubuntu select the `-posix` alternatives:

```sh
sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
sudo update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
```

ARM64 uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) for the C++ and
`gcc-aarch64-linux-gnu` for the game C, routed through
`tools/gcc_windows_arm64_launcher.py` (`TARGET_ARCH=arm64 tools/package_windows.sh`).

Dawn, dxcompiler and nod come as MSVC-built prebuilts
(`AURORA_DAWN_PROVIDER=package`, see `extern/aurora/cmake/AuroraDawnProvider.cmake`),
which import the Visual C++ runtime. `package_windows.sh` downloads
`vc_redist.x64.exe` (needs `cabextract` and `curl`), extracts the four runtime
DLLs app-local, and fails packaging if any staged `.exe`/`.dll` imports a DLL
that is neither shipped nor a stock Windows DLL. `libstdc++` and `libgcc` are
linked statically. Do not treat a run under Wine/Proton (`tools/run_proton.sh`)
as proof a zip works: the prefix supplies the MSVC runtime that real Windows
does not have.

### Android

Needs an NDK (`ANDROID_NDK_HOME`, r26 is the version the port is developed
against; newer NDK libc++ does not expose `std::jthread`), a JDK 17, and
`gcc-aarch64-linux-gnu`. The NDK's clang does not implement
`scalar_storage_order`, so `tools/gcc_launcher.py` routes `melee_game` through
the GCC cross compiler with the NDK sysroot. nod has no Android prebuilt, so
aurora builds it from Rust source (`rustup target add aarch64-linux-android`).
A signed release build needs `MELEE_KEYSTORE_BASE64`, `MELEE_KEYSTORE_PASSWORD`,
`MELEE_KEY_ALIAS` and `MELEE_KEY_PASSWORD`.

### iOS

`tools/build_ios.sh` cross-compiles from Linux with clang/lld, the theos
`iPhoneOS16.5.sdk` (`IOS_SDK_PATH`), `gcc-aarch64-linux-gnu` for the game C
(`tools/gcc_ios_launcher.py`) and the `aarch64-apple-ios` Rust target for nod.
The IPA is unsigned and meant for sideloading.

### macOS

Apple Silicon (tested) and Intel (CI-built, untested). Apple's clang builds the
C++; the decomp's C still needs GCC, so `tools/gcc_launcher.py` routes
`melee_game` through Homebrew's `gcc` (the same split the Android build uses).
Dawn comes as a prebuilt with a Metal backend.

```sh
brew install gcc cmake ninja sdl3 zstd libpng freetype
cmake --preset macos-default
ninja -C build/macos
build/macos/melee <disc>
```

Homebrew GCC is a permanent requirement, not a stopgap: macOS builds need it
today and will keep needing it for as long as the game reads disc structs
through `scalar_storage_order`, because no Clang (Apple's or upstream's)
implements it. `gcc_launcher.py` picks the newest `gcc-NN` on `PATH`
(`GCC_BIN` overrides), so a Homebrew gcc bump needs no change here. Only the
game's C goes through it; everything else, including the link, stays on Apple
clang, which is why the `.app` still behaves like a normal Xcode-built binary.

arm64 macOS kills native binaries whose `__PAGEZERO` is under 4GB and requires
PIE, so the non-PIE/`MAP_32BIT` layout the other platforms use is impossible;
see the memory map in [architecture.md](architecture.md#memory-map) for how
MEM1 is placed instead.

`Melee.app` is ad-hoc signed, so the first launch of a downloaded copy needs
right-click > Open, or `xattr -d com.apple.quarantine Melee.app`.
