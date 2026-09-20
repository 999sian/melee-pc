# Compiler gate. Include right after project(); it runs at include time.
#
# The game reads on-disc structures through
# __attribute__((scalar_storage_order("big-endian"))), which only GCC
# implements, and only for C. Clang accepts the syntax and ignores it, so a
# Clang build compiles clean and then reads every disc field byte-swapped.
#
# Android, Apple and Windows-ARM64 keep Clang for the C++ and route the game's
# C through GCC (tools/gcc_launcher.py, tools/gcc_ios_launcher.py,
# tools/gcc_windows_arm64_launcher.py), so CMAKE_C_COMPILER_ID is Clang there
# on purpose and the gate does not apply. Emscripten has no GCC at all: its
# game C goes through the lowering pass in tools/browser instead.
include_guard(GLOBAL)

if (EMSCRIPTEN OR ANDROID OR APPLE OR (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$"))
    return()
endif ()

set(_melee_toolchain_fix
    "  fix:      CC=gcc CXX=g++ cmake -B build -G Ninja   (delete the stale cache first)\n"
    "            python3 tools/preflight.py                # checks the whole toolchain\n"
    "            docs/building.md lists the supported versions.")

if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "melee-pc requires GCC as the C compiler.\n"
        "  found:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})\n"
        "  required: GNU (GCC 12 or newer)\n"
        "  why:      the decomp's on-disc structs use\n"
        "            __attribute__((scalar_storage_order(\"big-endian\"))), which no other\n"
        "            compiler implements; Clang ignores it and every disc field comes\n"
        "            back byte-swapped at runtime.\n"
        ${_melee_toolchain_fix})
endif ()

# A GCC built without the attribute (or with it warning-only) would otherwise
# produce thousands of -Wattributes warnings and wrong data at runtime, so
# prove it works instead of trusting the compiler id. -Werror turns the
# "attribute ignored" warning into the configure failure it deserves to be.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-Werror")
check_c_source_compiles([[
struct __attribute__((scalar_storage_order("big-endian"))) S { int v; };
int main(void) { struct S s = { 1 }; return s.v - 1; }
]] MELEE_HAVE_SCALAR_STORAGE_ORDER)
unset(CMAKE_REQUIRED_FLAGS)

if (NOT MELEE_HAVE_SCALAR_STORAGE_ORDER)
    message(FATAL_ERROR
        "the selected C compiler rejects scalar_storage_order.\n"
        "  found:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})\n"
        "  required: a GCC that accepts\n"
        "            __attribute__((scalar_storage_order(\"big-endian\"))) without warning\n"
        "  why:      every on-disc struct depends on it; without it the game reads\n"
        "            byte-swapped data and crashes in unrelated places.\n"
        ${_melee_toolchain_fix})
endif ()

unset(_melee_toolchain_fix)
