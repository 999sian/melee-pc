# Reproducible Linux build image. The base is the CI runner's distribution
# (ubuntu-24.04 in .github/workflows/build.yml), so the GCC, glibc and apt
# packages in here are the ones the release AppImage is actually built with.
# Works with docker or podman:
#
#   docker build -t melee-pc-build -f Containerfile .
#   docker run --rm -v "$PWD:/src" -w /src -u "$(id -u):$(id -g)" \
#       -e BUILD_DIR=/src/build-container melee-pc-build tools/package_linux.sh
#
# BUILD_DIR keeps the container's objects out of a host build/ configured by a
# different compiler. Drop the command to get a shell for an ordinary build:
#
#   docker run --rm -it -v "$PWD:/src" -w /src melee-pc-build bash
#   cmake -B build-container -G Ninja && ninja -C build-container melee
#
# ponytail: apt version pins rot within days because Ubuntu's archive only
# keeps the current point release of each package, so the reproducibility
# boundary here is the base image tag plus the noble pocket, not a pin list.
# Pin the base by digest (docker inspect melee-pc-build) if a release needs to
# be reproduced byte for byte.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Verbatim the apt list of the linux job, plus four the GitHub runner image
# already has: curl + ca-certificates (package_linux.sh fetches appimagetool
# and linuxdeploy; aurora's FetchContent downloads), git (submodule/FetchContent
# checkouts) and python3 (tools/). SDL3 is not in Ubuntu 24.04, so aurora builds
# it from source (AURORA_SDL3_PROVIDER=vendor) and needs its X11/Wayland/audio
# headers here.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build cmake pkg-config \
        libssl-dev libcurl4-openssl-dev zip file desktop-file-utils \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
        libxfixes-dev libxss-dev libxkbcommon-dev libxtst-dev \
        libwayland-dev wayland-protocols libdecor-0-dev \
        libasound2-dev libpulse-dev libdbus-1-dev libudev-dev \
        libgl1-mesa-dev libegl1-mesa-dev libgbm-dev \
        curl ca-certificates git python3 \
    && rm -rf /var/lib/apt/lists/*

# No /dev/fuse in a container, so the AppImage tools must self-extract.
# package_linux.sh sets this itself when /dev/fuse is absent; setting it here
# too means a hand-run appimagetool behaves the same.
ENV APPIMAGE_EXTRACT_AND_RUN=1

WORKDIR /src
CMD ["tools/package_linux.sh"]
