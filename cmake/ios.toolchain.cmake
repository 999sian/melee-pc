# iOS CMake Cross-Compilation Toolchain for Linux
set(CMAKE_SYSTEM_NAME iOS)
set(IOS TRUE)
set(APPLE TRUE)
set(UNIX TRUE)

set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(Rust_CARGO_TARGET "aarch64-apple-ios" CACHE STRING "Rust target triple" FORCE)
set(Rust_CARGO_TARGET_LINK_NATIVE_LIBS "" CACHE INTERNAL "Rust target native libraries for iOS" FORCE)

if (NOT IOS_SDK_PATH)
    if (DEFINED ENV{IOS_SDK_PATH} AND EXISTS "$ENV{IOS_SDK_PATH}")
        set(IOS_SDK_PATH "$ENV{IOS_SDK_PATH}" CACHE PATH "Path to iPhoneOS SDK")
    elseif (DEFINED ENV{SDKROOT} AND EXISTS "$ENV{SDKROOT}")
        set(IOS_SDK_PATH "$ENV{SDKROOT}" CACHE PATH "Path to iPhoneOS SDK")
    elseif (EXISTS "/home/sian/toolchains/sdks/sdks/iPhoneOS16.5.sdk")
        set(IOS_SDK_PATH "/home/sian/toolchains/sdks/sdks/iPhoneOS16.5.sdk" CACHE PATH "Path to iPhoneOS SDK")
    elseif (DEFINED ENV{IOS_SDK_PATH})
        set(IOS_SDK_PATH "$ENV{IOS_SDK_PATH}" CACHE PATH "Path to iPhoneOS SDK")
    elseif (DEFINED ENV{SDKROOT})
        set(IOS_SDK_PATH "$ENV{SDKROOT}" CACHE PATH "Path to iPhoneOS SDK")
    else ()
        set(IOS_SDK_PATH "/home/sian/toolchains/sdks/sdks/iPhoneOS16.5.sdk" CACHE PATH "Path to iPhoneOS SDK")
    endif ()
endif ()

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES IOS_SDK_PATH)

set(CMAKE_SYSROOT "${IOS_SDK_PATH}")
set(CMAKE_OSX_SYSROOT "${IOS_SDK_PATH}")

set(IOS_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum iOS deployment target")
set(IOS_TARGET_TRIPLE "arm64-apple-ios${IOS_DEPLOYMENT_TARGET}")

set(CMAKE_C_COMPILER "/usr/bin/clang")
set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
set(CMAKE_OBJC_COMPILER "/usr/bin/clang")
set(CMAKE_OBJCXX_COMPILER "/usr/bin/clang++")

set(IOS_COMMON_FLAGS "--target=${IOS_TARGET_TRIPLE} -isysroot ${IOS_SDK_PATH} -D__APPLE__=1 -DTARGET_OS_IPHONE=1 -DTARGET_OS_IOS=1")

set(CMAKE_C_FLAGS_INIT "${IOS_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${IOS_COMMON_FLAGS} -stdlib=libc++")
set(CMAKE_OBJC_FLAGS_INIT "${IOS_COMMON_FLAGS}")
set(CMAKE_OBJCXX_FLAGS_INIT "${IOS_COMMON_FLAGS} -stdlib=libc++")

set(CMAKE_EXE_LINKER_FLAGS_INIT "--target=${IOS_TARGET_TRIPLE} -isysroot ${IOS_SDK_PATH} -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--target=${IOS_TARGET_TRIPLE} -isysroot ${IOS_SDK_PATH} -fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH "${IOS_SDK_PATH}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
