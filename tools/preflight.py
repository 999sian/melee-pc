#!/usr/bin/env python3
"""Check the host toolchain for a melee-pc build target.

Prints PASS/FAIL per check with what was found, what is required and the exact
command that fixes it. Exits nonzero if anything failed.

    python3 tools/preflight.py                  # host target
    python3 tools/preflight.py --target android

Supported versions come from .github/workflows/build.yml (the CI jobs that
produce the release artifacts) and the tools/gcc_*_launcher.py scripts.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

TARGETS = ("linux", "windows", "android", "ios", "macos")

# Pinned by CI. Keep in sync with .github/workflows/build.yml.
NDK_VERSION = "26.3.11579264"          # newer NDK libc++ lacks std::jthread
LLVM_MINGW = "20260908"                # llvm-mingw release for Windows ARM64
JDK_VERSION = "17"                     # temurin 17
IOS_SDK = "iPhoneOS16.5.sdk"           # theos/sdks
GCC_FLOOR = 12                         # CI runs ubuntu-24.04's GCC 13
CMAKE_FLOOR = (3, 25)                  # cmake_minimum_required in CMakeLists.txt

# The linux job's apt list, as pkg-config names (aurora builds SDL3 from
# source on Ubuntu 24.04, so its X11/Wayland/audio headers must be present).
LINUX_PKGCONFIG = [
    ("x11", "libx11-dev"), ("xext", "libxext-dev"), ("xrandr", "libxrandr-dev"),
    ("xcursor", "libxcursor-dev"), ("xi", "libxi-dev"), ("xfixes", "libxfixes-dev"),
    ("xscrnsaver", "libxss-dev"), ("xkbcommon", "libxkbcommon-dev"),
    ("xtst", "libxtst-dev"), ("wayland-client", "libwayland-dev"),
    ("libdecor-0", "libdecor-0-dev"), ("alsa", "libasound2-dev"),
    ("libpulse", "libpulse-dev"), ("dbus-1", "libdbus-1-dev"),
    ("libudev", "libudev-dev"), ("gl", "libgl1-mesa-dev"),
    ("egl", "libegl1-mesa-dev"), ("gbm", "libgbm-dev"),
    ("openssl", "libssl-dev"), ("libcurl", "libcurl4-openssl-dev"),
]

results = []


def check(name, ok, found, required, fix=""):
    results.append((bool(ok), name, found, required, fix))


def run(*cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return ""
    return (p.stdout + p.stderr).strip()


def version_line(exe):
    out = run(exe, "--version")
    return out.splitlines()[0] if out else ""


def numbers(text):
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text or "")
    return tuple(int(g) for g in m.groups() if g) if m else ()


def tool(name, exe, required, fix, floor=()):
    """Check an executable is on PATH and new enough. Returns its path or ''."""
    path = shutil.which(exe)
    if not path:
        check(name, False, f"{exe}: not on PATH", required, fix)
        return ""
    ver = version_line(path)
    if floor and numbers(ver) and numbers(ver) < floor:
        check(name, False, f"{ver} ({path})", required, fix)
        return path
    check(name, True, f"{ver or path}", required, fix)
    return path


SSO_SOURCE = (
    'struct __attribute__((scalar_storage_order("big-endian"))) S { int v; };\n'
    "int main(void) { struct S s = { 1 }; return s.v - 1; }\n"
)


def check_sso(cc, label):
    """Compile the attribute for real; Clang accepts the syntax and ignores it."""
    if not cc:
        check(label, False, "no compiler to probe", "GCC that accepts the attribute", "")
        return
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "probe.c")
        with open(src, "w") as f:
            f.write(SSO_SOURCE)
        p = subprocess.run([cc, "-Werror", "-c", src, "-o", os.path.join(d, "probe.o")],
                           capture_output=True, text=True)
    errors = [l for l in p.stderr.splitlines() if "scalar_storage_order" in l]
    detail = (errors or p.stderr.splitlines() or ["rejected"])[0].strip() if p.returncode \
        else "accepted"
    check(label, p.returncode == 0, detail,
          '__attribute__((scalar_storage_order("big-endian"))) must compile clean',
          "install GCC; Clang ignores the attribute and reads every on-disc struct "
          "byte-swapped")


def rust_target(triple, why):
    have = "rustup" if shutil.which("rustup") else ""
    installed = run("rustup", "target", "list", "--installed") if have else ""
    check(f"rust target {triple}", triple in installed.split(),
          installed.replace("\n", " ") or "rustup not on PATH",
          f"{triple} ({why})",
          f"curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh && "
          f"rustup target add {triple}")


def common_checks():
    cc = os.environ.get("CC", "gcc")
    cxx = os.environ.get("CXX", "g++")
    gcc = tool("C compiler present", cc, f"GCC >= {GCC_FLOOR} (CI: GCC 13 on ubuntu-24.04)",
               "sudo apt-get install -y build-essential   # macOS: brew install gcc",
               floor=(GCC_FLOOR,))
    ver = version_line(gcc) if gcc else ""
    check("C compiler is GCC", "clang" not in ver.lower() and bool(gcc), ver or "none",
          "GCC; Clang accepts scalar_storage_order and ignores it",
          "CC=gcc CXX=g++ cmake -B build -G Ninja")
    tool("C++ compiler", cxx, "GCC or Clang with C++20",
         "sudo apt-get install -y build-essential")
    check_sso(gcc, "scalar_storage_order probe")
    tool("cmake", "cmake", "cmake >= %d.%d" % CMAKE_FLOOR,
         "sudo apt-get install -y cmake   # macOS: brew install cmake", floor=CMAKE_FLOOR)
    tool("ninja", "ninja", "any release",
         "sudo apt-get install -y ninja-build   # macOS: brew install ninja")
    check("python3", sys.version_info >= (3, 8), sys.version.split()[0], "python3 >= 3.8",
          "sudo apt-get install -y python3")


def linux_checks():
    pc = tool("pkg-config", "pkg-config", "any", "sudo apt-get install -y pkg-config")
    missing = [apt for name, apt in LINUX_PKGCONFIG
               if pc and subprocess.run([pc, "--exists", name]).returncode]
    check("SDL3/aurora dev headers", pc and not missing,
          "all present" if pc and not missing else "missing: " + " ".join(missing or ["pkg-config"]),
          "the linux job's apt list (aurora builds SDL3 from source on Ubuntu 24.04)",
          "sudo apt-get install -y --no-install-recommends " + " ".join(missing or ["pkg-config"]))
    for exe in ("zip", "file", "curl"):
        tool(exe, exe, "AppImage packaging (tools/package_linux.sh)",
             f"sudo apt-get install -y {exe}")
    tool("desktop-file-validate", "desktop-file-validate", "AppImage packaging",
         "sudo apt-get install -y desktop-file-utils")


def windows_checks():
    for exe in ("x86_64-w64-mingw32-gcc", "x86_64-w64-mingw32-g++"):
        path = tool(exe, exe, "mingw-w64 (CI: Ubuntu 24.04 package)",
                    "sudo apt-get install -y mingw-w64")
        if path:
            model = re.search(r"Thread model: (\S+)", run(path, "-v"))
            model = model.group(1) if model else "unknown"
            check(f"{exe} thread model", model == "posix", model,
                  "posix (aurora's C++20 needs std::thread; Ubuntu defaults to win32)",
                  f"sudo update-alternatives --set {exe} /usr/bin/{exe}-posix")
    tool("aarch64-w64-mingw32-clang++", "aarch64-w64-mingw32-clang++",
         f"llvm-mingw {LLVM_MINGW} ucrt, for TARGET_ARCH=arm64",
         "curl -sSL https://github.com/mstorsjo/llvm-mingw/releases/download/"
         f"{LLVM_MINGW}/llvm-mingw-{LLVM_MINGW}-ucrt-ubuntu-22.04-x86_64.tar.xz | "
         "sudo tar -xJ -C /opt/llvm-mingw --strip-components=1  "
         "# then add /opt/llvm-mingw/bin to PATH")
    tool("aarch64-linux-gnu-gcc", os.environ.get("GCC_AARCH64_BIN", "aarch64-linux-gnu-gcc"),
         "game C for ARM64 (tools/gcc_windows_arm64_launcher.py)",
         "sudo apt-get install -y gcc-aarch64-linux-gnu")
    for exe in ("cabextract", "curl"):
        tool(exe, exe, "vc_redist extraction in tools/package_windows.sh",
             f"sudo apt-get install -y {exe}")
    tool("zip", "zip", "zip packaging in tools/package_windows.sh",
         "sudo apt-get install -y zip")


def android_checks():
    ndk = os.environ.get("ANDROID_NDK_HOME", "")
    sysroot = os.path.join(ndk, "toolchains/llvm/prebuilt/linux-x86_64/sysroot")
    check("ANDROID_NDK_HOME", ndk and NDK_VERSION in ndk and os.path.isdir(sysroot),
          ndk or "unset", f"an r26 NDK; CI uses exactly {NDK_VERSION}",
          f'"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install "ndk;{NDK_VERSION}" '
          f'&& export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/{NDK_VERSION}"')
    java = tool("JDK", "javac", f"JDK {JDK_VERSION} (temurin)",
                f"sudo apt-get install -y openjdk-{JDK_VERSION}-jdk")
    if java:
        major = numbers(version_line(java))
        check("JDK version", major and major[0] == int(JDK_VERSION),
              version_line(java), f"JDK {JDK_VERSION}",
              f"sudo apt-get install -y openjdk-{JDK_VERSION}-jdk && "
              f"sudo update-alternatives --config javac")
    tool("aarch64-linux-gnu-gcc", os.environ.get("GCC_AARCH64_BIN", "aarch64-linux-gnu-gcc"),
         "game C for Android (tools/gcc_launcher.py; NDK clang lacks the attribute)",
         "sudo apt-get install -y gcc-aarch64-linux-gnu")
    rust_target("aarch64-linux-android", "nod has no Android prebuilt")


def ios_checks():
    for exe in ("clang", "ld64.lld", "llvm-ar", "llvm-strip"):
        tool(exe, exe, "clang/lld 19 (CI installs clang-19 lld-19 llvm-19 and symlinks them)",
             "sudo apt-get install -y clang-19 lld-19 llvm-19 && "
             f"sudo ln -sf $(command -v {exe}-19) /usr/local/bin/{exe}")
    tool("aarch64-linux-gnu-gcc", os.environ.get("GCC_AARCH64_BIN", "aarch64-linux-gnu-gcc"),
         "game C for iOS (tools/gcc_ios_launcher.py)",
         "sudo apt-get install -y gcc-aarch64-linux-gnu")
    sdk = os.environ.get("IOS_SDK_PATH", "")
    check("IOS_SDK_PATH", sdk and os.path.isdir(sdk), sdk or "unset",
          f"theos {IOS_SDK}",
          "git clone --depth=1 --filter=blob:none --sparse https://github.com/theos/sdks.git "
          f"/tmp/sdks && git -C /tmp/sdks sparse-checkout set {IOS_SDK} && "
          f"export IOS_SDK_PATH=/tmp/sdks/{IOS_SDK}")
    rust_target("aarch64-apple-ios", "nod has no iOS prebuilt")


def macos_checks():
    brew_gcc = next((f"gcc-{v}" for v in range(20, 12, -1) if shutil.which(f"gcc-{v}")), "")
    check("Homebrew GCC (gcc-NN)", bool(brew_gcc),
          version_line(brew_gcc) if brew_gcc else "no gcc-NN on PATH",
          "Homebrew gcc; Apple clang builds the C++ and tools/gcc_launcher.py routes "
          "the game's C here (set GCC_BIN to override)",
          "brew install gcc")
    tool("xcodebuild", "xcodebuild", "Xcode command line tools",
         "xcode-select --install")
    for formula in ("sdl3", "zstd", "libpng", "freetype"):
        prefix = run("brew", "--prefix", formula).splitlines()[-1:] or [""]
        check(f"brew {formula}", os.path.isdir(prefix[0]), prefix[0] or "not installed",
              f"Homebrew {formula}", "brew install gcc cmake ninja sdl3 zstd libpng freetype")


def main(argv):
    target = {"linux": "linux", "darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")
    if "--help" in argv or "-h" in argv:
        print(__doc__)
        print("targets: " + " ".join(TARGETS))
        return 0
    if "--target" in argv:
        target = argv[argv.index("--target") + 1]
    if target not in TARGETS:
        print(f"unknown target {target!r}; expected one of {' '.join(TARGETS)}", file=sys.stderr)
        return 2

    print(f"melee-pc preflight: target {target} (host {sys.platform} "
          f"{os.uname().machine if hasattr(os, 'uname') else ''})")
    # ponytail: fix lines use the package names CI installs; translating them
    # to every distro is a package database, not a preflight.
    print("fix commands use Debian/Ubuntu (CI) package names\n")
    common_checks()
    {"linux": linux_checks, "windows": windows_checks, "android": android_checks,
     "ios": ios_checks, "macos": macos_checks}[target]()

    failed = 0
    for ok, name, found, required, fix in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        print(f"        found:    {found}")
        print(f"        required: {required}")
        if not ok:
            failed += 1
            if fix:
                print(f"        fix:      {fix}")
    print(f"\n{len(results) - failed} passed, {failed} failed")
    if failed:
        print("docs/building.md has the supported version table and the container build.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
