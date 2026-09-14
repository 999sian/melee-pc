set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Target Windows 10+. aurora's set_thread_name calls SetThreadDescription,
# which mingw-w64 declares only at _WIN32_WINNT >= 0x0A00. Verified on both
# mingw GCC 16 (Arch) and GCC 13 (Ubuntu 24.04); abseil cctz still builds.
add_compile_definitions(NTDDI_VERSION=0x0A000000 _WIN32_WINNT=0x0A00 WINVER=0x0A00)

# Disable host pkg-config so host /usr/include is never injected into cross-compilation
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
