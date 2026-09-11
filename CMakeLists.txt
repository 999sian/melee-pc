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
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

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
target_link_libraries(melee PRIVATE melee_game aurora::main)
# Non-PIE: static data and code stay below 4GB (32-bit pointer slots in disc data).
target_link_options(melee PRIVATE -no-pie -Wl,-Ttext-segment=0x10000000)
set_target_properties(melee PROPERTIES OUTPUT_NAME melee)
