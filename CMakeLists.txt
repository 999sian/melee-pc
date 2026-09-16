cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

project(melee-pc LANGUAGES C CXX)

if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT ANDROID AND NOT APPLE)
    # Game code relies on __attribute__((scalar_storage_order("big-endian"))) for
    # on-disc structures, which only GCC implements (C only). Android and macOS
    # keep Clang for C++ and route the game's C through GCC (tools/gcc_launcher.py).
    message(FATAL_ERROR "melee-pc requires GCC as the C compiler (got ${CMAKE_C_COMPILER_ID})")
endif ()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
if (WIN32)
    set(MELEE_CRYPTO_LIBS bcrypt)
    add_compile_definitions(MELEE_USE_BCRYPT)
elseif (ANDROID OR APPLE)
    # Builtin SHA-1 keeps the macOS bundle free of a Homebrew OpenSSL.
    set(MELEE_CRYPTO_LIBS)
    add_compile_definitions(MELEE_USE_BUILTIN_SHA1)
else ()
    find_package(OpenSSL COMPONENTS Crypto QUIET)
    if (OpenSSL_FOUND)
        set(MELEE_CRYPTO_LIBS OpenSSL::Crypto)
        add_compile_definitions(MELEE_USE_OPENSSL)
    else ()
        set(MELEE_CRYPTO_LIBS)
        add_compile_definitions(MELEE_USE_BUILTIN_SHA1)
    endif ()
endif ()

# ---- Game code (decomp, compiled as C) -------------------------------------
# src/thp (Melee's PPC THP decoder) is replaced by aurora::thp; see src/pc/thp.c.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
        src/melee/*.c
        src/sysdolphin/*.c)
list(APPEND GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/pc/vtxarray.c")
# PowerPC/MetroTRK debugger integration; no PC equivalent.
list(FILTER GAME_SOURCES EXCLUDE REGEX "baselib/debugconsole_main\\.c$")

add_library(melee_game STATIC ${GAME_SOURCES})
target_include_directories(melee_game PUBLIC extern/aurora/include src src/sdk_include)
target_compile_definitions(melee_game PUBLIC TARGET_PC=1 MELEE_PC=1)
target_compile_options(melee_game PRIVATE
        -fpermissive
        -Wno-all -Wno-extra
        -Werror=int-conversion
        -Werror=implicit-function-declaration
        -Werror=incompatible-pointer-types
        -fno-strict-aliasing
        -fwrapv
        -ffp-contract=off
        # The game's Japanese string literals are consumed as Shift-JIS at
        # runtime: mnnamenew.c stores name glyphs two bytes at a time into a
        # fixed 3-bytes-per-slot buffer, and compares that buffer against
        # auto-name tables read straight off the disc, which are already
        # Shift-JIS. Upstream gets this from sjiswrap around mwcc. CP932 (not
        # SHIFT_JIS) because the tables use fullwidth punctuation such as
        # U+FF0D that glibc's SHIFT_JIS converter rejects.
        $<$<C_COMPILER_ID:GNU>:-fexec-charset=CP932>
        $<$<C_COMPILER_ID:GNU>:-Wno-scalar-storage-order>
        $<$<C_COMPILER_ID:Clang>:-Wno-unknown-attributes>
        $<$<C_COMPILER_ID:Clang>:-Wno-unknown-warning-option>
        $<$<NOT:$<BOOL:${WIN32}>>:-fPIC>
        $<$<BOOL:${WIN32}>:-mno-ms-bitfields>
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/pc/compat.h)
target_link_libraries(melee_game PUBLIC
        aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx
        aurora::os aurora::dvd aurora::card aurora::thp)

if (ANDROID OR APPLE)
    set_property(TARGET melee_game PROPERTY C_COMPILER_LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/tools/gcc_launcher.py")
endif ()

# ---- PC platform layer -----------------------------------------------------
file(GLOB PC_SOURCES CONFIGURE_DEPENDS src/pc/*.c src/pc/*.cpp)
list(REMOVE_ITEM PC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/pc/vtxarray.c")
if (ANDROID)
    add_library(melee SHARED ${PC_SOURCES})
else ()
    add_executable(melee ${PC_SOURCES})
endif ()

target_compile_options(melee PRIVATE $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<BOOL:${WIN32}>>,$<NOT:$<BOOL:${ANDROID}>>,$<NOT:$<BOOL:${APPLE}>>>:-fno-pie>)
target_link_libraries(melee PRIVATE melee_game aurora::main ${MELEE_CRYPTO_LIBS})

# Non-PIE: static data and code stay below 4GB (32-bit pointer slots in disc data).
if (WIN32)
    # -static-libstdc++/-static-libgcc: linking the C++ runtime as a DLL makes the linker
    # auto-import its data symbols, including C++ vtables, through MinGW's runtime
    # pseudo-relocation (_pei386_runtime_relocator). This image carries 330 such auto-imported
    # vtable/typeinfo symbols. Constructing the first std::ifstream reads a virtual-base offset
    # out of the auto-imported std::basic_ifstream<char> vtable to compute `this`, and on native
    # Windows that yields a bad pointer: std::basic_ios<char>::init() then writes to address 0.
    # Linking the runtime statically removes the auto-imported vtables entirely.
    target_link_options(melee PRIVATE -static-libstdc++ -static-libgcc -Wl,--image-base,0x10000000 -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va -Wl,-u,WinMain)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/melee.rc")
        target_sources(melee PRIVATE platforms/windows/melee.rc)
    endif ()
    include("${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake" OPTIONAL)
    if (COMMAND aurora_copy_runtime_dlls)
        aurora_copy_runtime_dlls(melee)
    endif ()
elseif (APPLE)
    # arm64 macOS refuses to run native binaries with a __PAGEZERO under 4GB and
    # requires PIE, so nothing below 4GB is ever addressable. MEM1 is instead
    # mapped at an address whose low 32 bits are 0x80000000 and disc slots hold
    # GameCube-style addresses (see src/pc/disc.h, PC_MEM1_ALIAS).
    target_compile_definitions(melee_game PUBLIC PC_MEM1_ALIAS=1)
elseif (NOT ANDROID)
    target_link_options(melee PRIVATE -no-pie -Wl,-Ttext-segment=0x10000000)
else ()
    target_link_options(melee PRIVATE -Wl,-z,max-page-size=16384)
endif ()

set_target_properties(melee PROPERTIES OUTPUT_NAME melee)
add_custom_target(launcher_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:melee>/resources")
add_dependencies(melee launcher_resources)

option(MELEE_ENABLE_LTO "Enable Link-Time Optimization (LTO)" OFF)
if (MELEE_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if (ipo_supported)
        message(STATUS "Link-Time Optimization (IPO/LTO) enabled")
        set_property(TARGET melee PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET melee_game PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else ()
        message(WARNING "IPO/LTO not supported: ${ipo_error}")
    endif ()
endif ()

if (NOT ANDROID)
    add_executable(launcher_data_test EXCLUDE_FROM_ALL tools/test_launcher_data.cpp src/pc/launcher_data.cpp src/pc/disc_open.c)
    target_include_directories(launcher_data_test PRIVATE src)
    target_link_libraries(launcher_data_test PRIVATE aurora::dvd ${MELEE_CRYPTO_LIBS})
    target_compile_options(launcher_data_test PRIVATE -UNDEBUG)
endif ()
