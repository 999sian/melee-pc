cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

project(melee-pc LANGUAGES C CXX)

if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # Game code relies on __attribute__((scalar_storage_order("big-endian"))) for
    # on-disc structures, which only GCC implements (C only).
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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# ---- Game code (decomp, compiled as C) -------------------------------------
# src/thp (Melee's PPC THP decoder) is replaced by aurora::thp; see src/pc/thp.c.
file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
        src/melee/*.c
        src/sysdolphin/*.c)
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
        -fexec-charset=CP932
        -fno-pie
        -Wno-scalar-storage-order
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/pc/compat.h)
target_link_libraries(melee_game PUBLIC
        aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx
        aurora::os aurora::dvd aurora::card aurora::thp)

# ---- PC platform layer -----------------------------------------------------
file(GLOB PC_SOURCES CONFIGURE_DEPENDS src/pc/*.c src/pc/*.cpp)
add_executable(melee ${PC_SOURCES})
target_compile_options(melee PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-pie>)
target_link_libraries(melee PRIVATE melee_game aurora::main OpenSSL::Crypto)
# Non-PIE: static data and code stay below 4GB (32-bit pointer slots in disc data).
target_link_options(melee PRIVATE -no-pie -Wl,-Ttext-segment=0x10000000)
set_target_properties(melee PROPERTIES OUTPUT_NAME melee)
add_custom_target(launcher_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:melee>/resources")
add_dependencies(melee launcher_resources)

add_executable(launcher_data_test EXCLUDE_FROM_ALL tools/test_launcher_data.cpp src/pc/launcher_data.cpp)
target_include_directories(launcher_data_test PRIVATE src)
target_link_libraries(launcher_data_test PRIVATE aurora::dvd OpenSSL::Crypto)
target_compile_options(launcher_data_test PRIVATE -UNDEBUG)
