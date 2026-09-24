cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

project(melee-pc LANGUAGES C CXX)

if (APPLE)
    enable_language(OBJC)
endif ()

include(cmake/MeleeToolchainCheck.cmake)

# Every platform builds the simulation at -O2 (Windows/Android packaging use
# Release, Linux RelWithDebInfo): -O3 vectorises float reductions differently
# and netplay needs identical bits on both peers.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# The browser target has its own platform layer and link; see platforms/browser.
if (EMSCRIPTEN)
    add_subdirectory(platforms/browser)
    return()
endif ()

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE)
if (IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif ()
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
# FP determinism for netplay: no fused multiply-add anywhere game logic can
# reach (AArch64 emits fmadd by default), no platform trig (see
# src/pc/libm/pc_trig.h). aurora_mtx is C and used by collision, so it gets
# the trig routing too; gx/gd only render.
set(MELEE_FP_FLAGS -ffp-contract=off -fno-fast-math)
target_compile_options(aurora_mtx PRIVATE ${MELEE_FP_FLAGS}
        -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-tanf -fno-builtin-atanf
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/pc/libm/pc_trig.h)
target_compile_options(aurora_gx PRIVATE ${MELEE_FP_FLAGS})
target_compile_options(aurora_gd PRIVATE ${MELEE_FP_FLAGS})

# Plain char is signed in the original game: mwcceppc (GC/1.2.5n, the
# decomp's compiler; configure.py passes no -char) documents "-char signed
# ... default", and the byte-matching mnname.c/mnnamenew.c compile to
# different code under -char unsigned. x86-64 agrees, but AArch64 Linux and
# Android default to unsigned, and tools/gcc_*launcher.py build the game with
# aarch64-linux-gnu-gcc for Android, iOS and Windows ARM64 alike. Compiling
# every game TU both ways changes 33 functions on x86-64 and 53 on AArch64;
# one computes something different, ifPrize's lead-in counter (char x2 = -1,
# counted up to 5), which skipped its six frames on ARM. Stated on every
# platform so nothing depends on a default. Directory scope covers
# melee_game, melee and the test targets created below; aurora's own targets
# get it by hand, its fetched dependencies keep their defaults.
add_compile_options(-fsigned-char)
get_property(_aurora_targets DIRECTORY extern/aurora PROPERTY BUILDSYSTEM_TARGETS)
foreach (_target IN LISTS _aurora_targets)
    target_compile_options(${_target} PRIVATE -fsigned-char)
endforeach ()

# Determinism-harness hook, off by default: perturb one routed trig function
# by one ULP so tools/net_determinism.py can prove it catches a libm
# divergence. Every math symbol the simulation uses is defined inside the
# executable (src/pc/libm + lbtrigf.c), so there is nothing for LD_PRELOAD to
# interpose and the injection has to happen at build time. See the header
# comment of src/pc/libm/pc_perturb.c. Such a build desyncs against every
# other build by design; it warns here, logs "net: FP PERTURB" on every run
# (src/pc/net.c) and tools/package_windows.sh refuses to package it.
set(MELEE_FP_PERTURB "" CACHE STRING
        "Determinism test only: return one ULP off from sinf|cosf|tanf|atanf")
set_property(CACHE MELEE_FP_PERTURB PROPERTY STRINGS "" sinf cosf tanf atanf)
set(MELEE_FP_PERTURB_DEFS)
if (MELEE_FP_PERTURB)
    if (NOT MELEE_FP_PERTURB MATCHES "^(sinf|cosf|tanf|atanf)$")
        message(FATAL_ERROR "MELEE_FP_PERTURB must be sinf, cosf, tanf or atanf "
                "(got '${MELEE_FP_PERTURB}')")
    endif ()
    string(TOUPPER "${MELEE_FP_PERTURB}" _perturb_upper)
    set(MELEE_FP_PERTURB_DEFS MELEE_FP_PERTURB_${_perturb_upper}=1
            MELEE_FP_PERTURB_NAME="${MELEE_FP_PERTURB}")
    message(WARNING "MELEE_FP_PERTURB=${MELEE_FP_PERTURB}: this build returns "
            "${MELEE_FP_PERTURB} one ULP off on purpose. It desyncs against every other build "
            "and must never be shipped or used for anything but tools/net_determinism.py.")
endif ()
# aurora_mtx is already configured above, so it needs the definition by hand;
# melee_game and melee are created below and pick it up from the directory.
target_compile_definitions(aurora_mtx PRIVATE ${MELEE_FP_PERTURB_DEFS})
add_compile_definitions(${MELEE_FP_PERTURB_DEFS})
if (WIN32)
    set(MELEE_CRYPTO_LIBS bcrypt)
    set(MELEE_NETWORK_LIBS winhttp ws2_32 iphlpapi) # iphlpapi: GetAdaptersAddresses in src/pc/net_lan.c
    add_compile_definitions(MELEE_USE_BCRYPT)
elseif (ANDROID OR IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(MELEE_CRYPTO_LIBS)
    set(MELEE_NETWORK_LIBS)
    add_compile_definitions(MELEE_USE_BUILTIN_SHA1)
elseif (APPLE)
    # Builtin SHA-1 keeps the bundle free of a Homebrew OpenSSL; libcurl ships
    # with macOS, so the updater can use it without bundling anything.
    set(MELEE_CRYPTO_LIBS)
    add_compile_definitions(MELEE_USE_BUILTIN_SHA1)
    find_package(CURL QUIET)
    if (CURL_FOUND)
        set(MELEE_NETWORK_LIBS CURL::libcurl)
        add_compile_definitions(MELEE_USE_CURL)
    else ()
        set(MELEE_NETWORK_LIBS)
    endif ()
else ()
    find_package(OpenSSL COMPONENTS Crypto QUIET)
    if (OpenSSL_FOUND)
        set(MELEE_CRYPTO_LIBS OpenSSL::Crypto)
        add_compile_definitions(MELEE_USE_OPENSSL)
    else ()
        set(MELEE_CRYPTO_LIBS)
        add_compile_definitions(MELEE_USE_BUILTIN_SHA1)
    endif ()
    find_package(CURL QUIET)
    if (CURL_FOUND)
        set(MELEE_NETWORK_LIBS CURL::libcurl)
        add_compile_definitions(MELEE_USE_CURL)
    else ()
        set(MELEE_NETWORK_LIBS)
    endif ()
endif ()

# ---- Game code (decomp, compiled as C) -------------------------------------
include(cmake/GameSources.cmake)

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
        ${MELEE_FP_FLAGS}
        -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-tanf -fno-builtin-atanf
        -ftrivial-auto-var-init=zero
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

# iOS is APPLE too, so it must be matched before the macOS launcher.
if (IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_property(TARGET melee_game PROPERTY C_COMPILER_LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/tools/gcc_ios_launcher.py")
elseif (ANDROID OR APPLE)
    set_property(TARGET melee_game PROPERTY C_COMPILER_LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/tools/gcc_launcher.py")
elseif (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set_property(TARGET melee_game PROPERTY C_COMPILER_LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/tools/gcc_windows_arm64_launcher.py")
endif ()

# ---- PC platform layer -----------------------------------------------------
# Explicit, not a glob: this list links straight into the shipped binary, so a
# stray .c/.cpp dropped into src/pc must not silently join the build.
# src/pc/vtxarray.c is deliberately absent - it belongs to melee_game.
set(PC_SOURCES
        src/pc/audio.c
        src/pc/disc_open.c
        src/pc/discfont.c
        src/pc/gcadapter.c
        src/pc/gx.c
        src/pc/input_poll.c
        src/pc/keyboard.c
        src/pc/main.c
        src/pc/misc.c
        src/pc/os.c
        src/pc/region.c
        src/pc/slp.c
        src/pc/slp_format.c
        src/pc/stb_vorbis.c
        src/pc/thp.c
        src/pc/touch.c
        src/pc/vi.c
        src/pc/widescreen.c
        src/pc/windows_arm64_softfp.c
        src/pc/android_compat.cpp
        src/pc/file_cache.cpp
        src/pc/launcher.cpp
        src/pc/launcher_data.cpp
        src/pc/music_stream.cpp
        src/pc/textures.cpp
        src/pc/updater.cpp
        src/pc/version.cpp
        # Netplay (docs/netcode-plan.md). net.c is the session and rollback
        # loop; the rest are its modules behind src/pc/net_internal.h.
        src/pc/net.c
        src/pc/net_chat.c
        src/pc/net_dht.c
        src/pc/net_dht_item.c
        src/pc/net_identity.c
        src/pc/net_match.c
        src/pc/net_rank.c
        src/pc/net_rank_store.c
        src/pc/net_rank_session.c
        src/pc/libm/pc_rank_exp.c
        src/pc/libm/pc_rank_sqrt.c
        extern/dht/dht.c
        extern/dht/sha1.c
        extern/monocypher/monocypher.c
        extern/monocypher/monocypher-ed25519.c
        src/pc/net_handshake.c
        src/pc/net_lan.c
        src/pc/net_reliable.c
        src/pc/net_sfx.c
        src/pc/net_sim.c
        src/pc/net_snapshot.c
        src/pc/net_sync.c
        src/pc/net_watchdog.c
        src/pc/net_wire.c
        # Vendored musl trig: platform libm differs by an ULP between targets,
        # which is a cross-platform desync (docs/netcode-plan.md section 5).
        src/pc/libm/pc_atanf.c
        src/pc/libm/pc_cosdf.c
        src/pc/libm/pc_cosf.c
        src/pc/libm/pc_perturb.c
        src/pc/libm/pc_rem_pio2_large.c
        src/pc/libm/pc_rem_pio2f.c
        src/pc/libm/pc_sindf.c
        src/pc/libm/pc_sinf.c
        src/pc/libm/pc_tandf.c
        src/pc/libm/pc_tanf.c)
# iOS, not APPLE: ios_dialog.m imports UIKit, which the macOS SDK has no
# headers for. The glob this replaced matched src/pc/*.m on macOS too.
if (IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND PC_SOURCES src/pc/ios_dialog.m)
endif ()
if (ANDROID)
    add_library(melee SHARED ${PC_SOURCES})
else ()
    add_executable(melee ${PC_SOURCES})
endif ()

target_include_directories(melee PRIVATE extern/monocypher)

target_compile_options(melee PRIVATE
        ${MELEE_FP_FLAGS}
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<BOOL:${WIN32}>>,$<NOT:$<BOOL:${ANDROID}>>,$<NOT:$<BOOL:${APPLE}>>>:-fno-pie>
        $<$<BOOL:${ANDROID}>:-march=armv8-a+crc+crypto>
        $<$<BOOL:${ANDROID}>:-mtune=cortex-a73>)
target_link_libraries(melee PRIVATE melee_game aurora::main ${MELEE_CRYPTO_LIBS} ${MELEE_NETWORK_LIBS})

if (IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(melee PRIVATE
        "-framework UIKit"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AudioToolbox"
        "-framework CoreHaptics"
        "-framework GameController"
        "-framework Foundation"
        "-framework CoreGraphics"
        "-framework UniformTypeIdentifiers"
    )
endif ()

# Non-PIE: static data and code stay below 4GB (32-bit pointer slots in disc data).
if (WIN32)
    # -static-libstdc++/-static-libgcc: linking the C++ runtime as a DLL makes the linker
    # auto-import its data symbols, including C++ vtables, through MinGW's runtime
    # pseudo-relocation (_pei386_runtime_relocator). This image carries 330 such auto-imported
    # vtable/typeinfo symbols. Constructing the first std::ifstream reads a virtual-base offset
    # out of the auto-imported std::basic_ifstream<char> vtable to compute `this`, and on native
    # Windows that yields a bad pointer: std::basic_ios<char>::init() then writes to address 0.
    # Linking the runtime statically removes the auto-imported vtables entirely.
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        target_link_options(melee PRIVATE -Wl,--image-base,0x10000000 -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va -Wl,-u,WinMain)
    else ()
        target_link_options(melee PRIVATE -static-libstdc++ -static-libgcc -Wl,--image-base,0x10000000 -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va -Wl,-u,WinMain)
    endif ()
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/melee.rc")
        target_sources(melee PRIVATE platforms/windows/melee.rc)
    endif ()
    include("${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake" OPTIONAL)
    if (COMMAND aurora_copy_runtime_dlls)
        aurora_copy_runtime_dlls(melee)
    endif ()
    # Two DLLs the exe imports that nothing stages, so a fresh build-win died
    # with STATUS_DLL_NOT_FOUND (c0000135) before the first log line:
    #   nod.dll            aurora_copy_runtime_dlls copies $<TARGET_RUNTIME_DLLS>,
    #                      i.e. each imported target's IMPORTED_LOCATION file
    #                      name, and nodConfig.cmake points that at
    #                      bin/libnod.dll (CMAKE_SHARED_LIBRARY_PREFIX). The
    #                      MinGW import library in the same package was built
    #                      against nod.dll, so nod.dll is the name in our
    #                      import table; the prebuilt ships both, byte for
    #                      byte identical. find_package(nod) ran in aurora's
    #                      directory scope, so nod::nod_shared is not visible
    #                      here and the file has to be found on disc.
    #   libwinpthread-1.dll  a toolchain file, not a CMake target, so it is in
    #                      no runtime-DLL set at all. tools/package_windows.sh
    #                      finds it for the zip, which is why this only ever
    #                      broke running from the build directory.
    # The Clang names mirror MINGW_DLLS in package_windows.sh; under GCC they
    # resolve to nothing and are skipped.
    set(_melee_runtime_dlls nod.dll libwinpthread-1.dll)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _melee_runtime_dlls libc++.dll libunwind.dll)
    endif ()
    get_filename_component(_cxx_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    # aurora fetched the nod package during its own configure, so its bin/ is
    # already on disc; globbed rather than named so a renamed FetchContent
    # directory still resolves.
    file(GLOB _nod_bins LIST_DIRECTORIES true "${CMAKE_BINARY_DIR}/_deps/*nod*/bin")
    foreach (_dll IN LISTS _melee_runtime_dlls)
        string(MAKE_C_IDENTIFIER "MELEE_RUNTIME_${_dll}" _cache_var)
        # The sysroot's bin/ is where mingw-w64 keeps its DLLs; -print-file-name
        # only knows the link-time libraries, so it misses them.
        find_file(${_cache_var} NAMES ${_dll}
                PATHS ${_nod_bins} ${CMAKE_FIND_ROOT_PATH} ${_cxx_bin}/.. ${_cxx_bin}
                PATH_SUFFIXES bin
                NO_CMAKE_FIND_ROOT_PATH)
        if (${_cache_var})
            message(STATUS "melee: staging ${_dll} from ${${_cache_var}}")
            add_custom_command(TARGET melee POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${${_cache_var}}" "$<TARGET_FILE_DIR:melee>")
        elseif (NOT _dll MATCHES "^libc\\+\\+|^libunwind")
            message(WARNING "melee: ${_dll} not found for ${CMAKE_CXX_COMPILER}; "
                    "melee.exe will not start from the build directory")
        endif ()
    endforeach ()
elseif (IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_options(melee PRIVATE -Wl,-dead_strip)
elseif (APPLE)
    # arm64 macOS requires PIE and refuses binaries with a __PAGEZERO under 4GB;
    # MEM1 lives above 4GB and pc_resolve_dp restores its high half (disc.h).
elseif (NOT ANDROID)
    target_link_options(melee PRIVATE -no-pie -Wl,-Ttext-segment=0x10000000
            -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/pc/melee_state.ld)
    set_property(TARGET melee APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pc/melee_state.ld)
    set(MELEE_STATE_SECTIONS ON)
else ()
    # lld (NDK 26+) supports INSERT AFTER as well.
    target_link_options(melee PRIVATE -Wl,-z,max-page-size=16384
            -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/pc/melee_state.ld)
    set_property(TARGET melee APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/pc/melee_state.ld)
    set(MELEE_STATE_SECTIONS ON)
endif ()
include(cmake/WindowsSnapshot.cmake)
melee_enable_windows_snapshots(melee melee_game)
include(cmake/AppleSnapshot.cmake)
melee_enable_apple_snapshots(melee melee_game)
# Every shipped platform must supply complete simulation ranges. Do not ship
# a platform that silently drops every online session to lockstep.
if (NOT MELEE_STATE_SECTIONS)
    message(FATAL_ERROR "Rollback snapshot sections are required on this platform")
endif ()
target_compile_definitions(melee PRIVATE MELEE_STATE_SECTIONS=1)
if (NOT WIN32 AND NOT APPLE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(TARGET melee POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/pe_snapshot_verify.py"
            --nm "${CMAKE_NM}" "$<TARGET_FILE:melee>"
            --tracked HSD_PadLibData HSD_Rumble_804C22E0 gmVsMelee_StartData
            --excluded net HSD_Synth_804D6018
        VERBATIM
        COMMENT "Verifying ELF rollback data ranges and engine exclusions")
endif()

set_target_properties(melee PROPERTIES OUTPUT_NAME melee)
if (IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(melee PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platforms/ios/Info.plist"
    )
endif ()
add_custom_target(launcher_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:melee>/resources")
add_dependencies(melee launcher_resources)

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    set(MELEE_PYTHON "${Python3_EXECUTABLE}")
else ()
    find_program(MELEE_PYTHON NAMES python3 python)
endif ()

if (MELEE_PYTHON AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/initial_pipeline_cache.db.gz")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/initial_pipeline_cache.db"
               "${CMAKE_CURRENT_BINARY_DIR}/resources/initial_pipeline_cache.db"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/resources"
        COMMAND "${MELEE_PYTHON}" -c "import gzip, sys; data = gzip.open(sys.argv[1], 'rb').read(); open(sys.argv[2], 'wb').write(data); open(sys.argv[3], 'wb').write(data)"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/initial_pipeline_cache.db.gz"
            "${CMAKE_CURRENT_BINARY_DIR}/initial_pipeline_cache.db"
            "${CMAKE_CURRENT_BINARY_DIR}/resources/initial_pipeline_cache.db"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/initial_pipeline_cache.db.gz"
        COMMENT "Unpacking pre-seeded Vulkan pipeline cache"
        VERBATIM
    )
    add_custom_target(pipeline_cache ALL DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/initial_pipeline_cache.db"
        "${CMAKE_CURRENT_BINARY_DIR}/resources/initial_pipeline_cache.db")
    add_dependencies(melee pipeline_cache)
endif ()

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

if (NOT ANDROID AND NOT IOS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Unit tests: `ninja -C build unit_tests && ctest --test-dir build -L melee`
    # (aurora registers its own tests, unbuilt here).
    enable_testing()
    add_custom_target(unit_tests)
    add_executable(launcher_data_test EXCLUDE_FROM_ALL tools/test_launcher_data.cpp src/pc/launcher_data.cpp src/pc/disc_open.c)
    target_include_directories(launcher_data_test PRIVATE src)
    target_link_libraries(launcher_data_test PRIVATE aurora::dvd ${MELEE_CRYPTO_LIBS})
    target_compile_options(launcher_data_test PRIVATE -UNDEBUG)
    add_test(NAME launcher_data COMMAND launcher_data_test)
    set_tests_properties(launcher_data PROPERTIES LABELS melee)
    add_dependencies(unit_tests launcher_data_test)

    add_executable(version_test EXCLUDE_FROM_ALL tools/test_version.cpp src/pc/version.cpp)
    target_include_directories(version_test PRIVATE src)
    target_compile_options(version_test PRIVATE -UNDEBUG)
    add_test(NAME version COMMAND version_test)
    set_tests_properties(version PROPERTIES LABELS melee)
    add_dependencies(unit_tests version_test)
    add_executable(endian_test EXCLUDE_FROM_ALL tools/test_endian.cpp)
    target_include_directories(endian_test PRIVATE src src/sdk_include)
    target_compile_options(endian_test PRIVATE -UNDEBUG)
    add_test(NAME endian COMMAND endian_test)
    set_tests_properties(endian PROPERTIES LABELS melee)
    add_dependencies(unit_tests endian_test)

    add_executable(thp_test EXCLUDE_FROM_ALL tools/test_thp.cpp src/pc/thp_jpeg.cpp src/pc/thp_stream.cpp)
    target_include_directories(thp_test PRIVATE src src/sdk_include)
    target_compile_options(thp_test PRIVATE -UNDEBUG)
    add_test(NAME thp COMMAND thp_test)
    set_tests_properties(thp PROPERTIES LABELS melee)
    add_dependencies(unit_tests thp_test)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        foreach(case IN ITEMS net_resume net_handshake net_lan_txt net_reliable)
            if (case STREQUAL "net_reliable")
                add_executable(${case}_test EXCLUDE_FROM_ALL tools/test_net_chat_transport.c src/pc/net_chat.c)
            else()
                add_executable(${case}_test EXCLUDE_FROM_ALL tools/test_${case}.c)
            endif()
            if (case STREQUAL "net_resume" OR case STREQUAL "net_handshake")
                # both include net_wire.c, which keys the per-datagram MAC
                target_sources(${case}_test PRIVATE extern/monocypher/monocypher.c)
                target_include_directories(${case}_test PRIVATE extern/monocypher)
            endif()
            target_include_directories(${case}_test PRIVATE extern/aurora/include src src/sdk_include)
            target_compile_definitions(${case}_test PRIVATE TARGET_PC=1 MELEE_PC=1 AURORA NDEBUG XXH_INLINE_ALL)
            target_link_libraries(${case}_test PRIVATE ${AURORA_SDL3_TARGET} xxhash)
            add_test(NAME ${case} COMMAND ${case}_test)
            set_tests_properties(${case} PROPERTIES LABELS melee TIMEOUT 30)
            add_dependencies(unit_tests ${case}_test)
        endforeach()
        add_executable(net_chat_ui_test EXCLUDE_FROM_ALL tools/test_net_chat_ui.c)
        target_include_directories(net_chat_ui_test PRIVATE src extern/aurora/include src/sdk_include)
        target_compile_definitions(net_chat_ui_test PRIVATE TARGET_PC=1 MELEE_PC=1 AURORA)
        target_compile_options(net_chat_ui_test PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/pc/compat.h)
        add_test(NAME net_chat_ui COMMAND net_chat_ui_test)
        set_tests_properties(net_chat_ui PROPERTIES LABELS melee TIMEOUT 30)
        add_dependencies(unit_tests net_chat_ui_test)
        add_executable(slp_test EXCLUDE_FROM_ALL tools/test_slp.c src/pc/slp_format.c)
        target_include_directories(slp_test PRIVATE src)
        target_link_libraries(slp_test PRIVATE ${AURORA_SDL3_TARGET})
        target_compile_options(slp_test PRIVATE -UNDEBUG)
        add_test(NAME slp COMMAND slp_test)
        set_tests_properties(slp PROPERTIES LABELS melee TIMEOUT 30)
        add_dependencies(unit_tests slp_test)
        add_executable(net_chat_test EXCLUDE_FROM_ALL tools/test_net_chat.c src/pc/net_chat.c)
        target_include_directories(net_chat_test PRIVATE src extern/aurora/include)
        target_link_libraries(net_chat_test PRIVATE ${AURORA_SDL3_TARGET})
        target_compile_options(net_chat_test PRIVATE -UNDEBUG)
        add_test(NAME net_chat COMMAND net_chat_test)
        set_tests_properties(net_chat PROPERTIES LABELS melee TIMEOUT 30)
        add_dependencies(unit_tests net_chat_test)
        add_executable(net_dht_bootstrap_test EXCLUDE_FROM_ALL tools/test_net_dht_bootstrap.c)
        target_compile_options(net_dht_bootstrap_test PRIVATE -UNDEBUG)
        add_test(NAME net_dht_bootstrap COMMAND net_dht_bootstrap_test)
        set_tests_properties(net_dht_bootstrap PROPERTIES LABELS melee TIMEOUT 30)
        add_dependencies(unit_tests net_dht_bootstrap_test)
        add_executable(net_sfx_test EXCLUDE_FROM_ALL tools/test_net_sfx.c src/pc/net_sfx.c)
        target_include_directories(net_sfx_test PRIVATE src)
        target_compile_options(net_sfx_test PRIVATE -UNDEBUG)
        add_test(NAME net_sfx COMMAND net_sfx_test)
        set_tests_properties(net_sfx PROPERTIES LABELS melee TIMEOUT 30)
        add_dependencies(unit_tests net_sfx_test)
        set(NET_IDENTITY_SOURCES src/pc/net_identity.c src/pc/net_dht.c src/pc/net_dht_item.c
            extern/dht/dht.c extern/dht/sha1.c extern/monocypher/monocypher.c
            extern/monocypher/monocypher-ed25519.c)
        foreach(case IN ITEMS net_identity net_dht net_rank net_rank_store)
            add_executable(${case}_test EXCLUDE_FROM_ALL tools/test_${case}.c ${NET_IDENTITY_SOURCES})
            if (case STREQUAL "net_rank" OR case STREQUAL "net_rank_store")
                target_sources(${case}_test PRIVATE src/pc/net_rank.c src/pc/libm/pc_rank_exp.c src/pc/libm/pc_rank_sqrt.c)
            endif()
            if (case STREQUAL "net_rank_store")
                target_sources(${case}_test PRIVATE src/pc/net_rank_store.c)
            endif()
            target_include_directories(${case}_test PRIVATE src src/pc extern/monocypher)
            target_compile_definitions(${case}_test PRIVATE PC_DHT_TEST_NO_BOOTSTRAP)
            target_compile_options(${case}_test PRIVATE -UNDEBUG ${MELEE_FP_FLAGS})
            target_link_libraries(${case}_test PRIVATE ${AURORA_SDL3_TARGET} m)
            add_test(NAME ${case} COMMAND ${case}_test)
            set_tests_properties(${case} PROPERTIES LABELS melee TIMEOUT 60)
            add_dependencies(unit_tests ${case}_test)
        endforeach()
        foreach(case IN ITEMS net_snapshot net_stage net_set_flow net_replay_seed net_scene_timing net_fixture_env net_rank_rules net_match net_magnify net_late_poll snapshot_platforms sim_prefs)
            add_test(NAME ${case} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_${case}.py)
            set_tests_properties(${case} PROPERTIES LABELS melee TIMEOUT 60)
        endforeach()
        add_test(NAME net_match_direct COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_net_match.py)
        set_tests_properties(net_match_direct PROPERTIES LABELS melee TIMEOUT 90
            ENVIRONMENT "MATCH_DIRECT=1")
        foreach(mode IN ITEMS ranked proof_timeout proof_mismatch complete recover)
            add_test(NAME net_match_${mode} COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_net_match.py)
            set_tests_properties(net_match_${mode} PROPERTIES LABELS melee TIMEOUT 90
                ENVIRONMENT "MATCH_RANKED=1")
            if (mode STREQUAL "proof_timeout")
                set_property(TEST net_match_${mode} APPEND PROPERTY ENVIRONMENT "MATCH_PROOF_TIMEOUT=1")
            elseif (mode STREQUAL "proof_mismatch")
                set_property(TEST net_match_${mode} APPEND PROPERTY ENVIRONMENT "MATCH_PROOF_MISMATCH=1")
            elseif (mode STREQUAL "complete" OR mode STREQUAL "recover")
                set_property(TEST net_match_${mode} APPEND PROPERTY ENVIRONMENT "MATCH_COMPLETE=1")
                if (mode STREQUAL "recover")
                    set_property(TEST net_match_${mode} APPEND PROPERTY ENVIRONMENT "MATCH_RECOVER=1")
                endif()
            endif()
        endforeach()
        add_executable(net_dht_observation_test EXCLUDE_FROM_ALL tools/test_net_dht_observation.c
            src/pc/net_dht_item.c src/pc/net_identity.c extern/dht/dht.c extern/dht/sha1.c
            extern/monocypher/monocypher.c extern/monocypher/monocypher-ed25519.c)
        add_executable(net_dht_item_test EXCLUDE_FROM_ALL tools/test_net_dht_item.c
            src/pc/net_identity.c extern/dht/sha1.c extern/monocypher/monocypher.c
            extern/monocypher/monocypher-ed25519.c)
        add_executable(net_rank_session_test EXCLUDE_FROM_ALL tools/test_net_rank_session.c
            extern/dht/sha1.c src/pc/net_rank.c src/pc/net_rank_store.c src/pc/net_identity.c
            src/pc/libm/pc_rank_exp.c src/pc/libm/pc_rank_sqrt.c
            extern/monocypher/monocypher.c extern/monocypher/monocypher-ed25519.c)
        foreach(case IN ITEMS net_dht_observation net_dht_item net_rank_session)
            target_include_directories(${case}_test PRIVATE src src/pc extern/monocypher)
            target_compile_options(${case}_test PRIVATE -UNDEBUG ${MELEE_FP_FLAGS} -ffunction-sections -fdata-sections)
            target_link_options(${case}_test PRIVATE -Wl,--gc-sections)
            target_link_libraries(${case}_test PRIVATE ${AURORA_SDL3_TARGET} m)
            add_test(NAME ${case} COMMAND ${case}_test)
            set_tests_properties(${case} PROPERTIES LABELS melee TIMEOUT 60)
            add_dependencies(unit_tests ${case}_test)
        endforeach()
        add_test(NAME pad_regressions COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_pad_regressions.py)
        add_test(NAME gcadapter COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_gcadapter.py --build ${CMAKE_BINARY_DIR})
        set_tests_properties(pad_regressions gcadapter PROPERTIES LABELS melee TIMEOUT 60)
        add_test(NAME wobble COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_wobble.py)
        set_tests_properties(wobble PROPERTIES LABELS melee TIMEOUT 60)
    endif ()

    if (NOT WIN32)
        add_executable(updater_test EXCLUDE_FROM_ALL tools/test_updater.cpp src/pc/version.cpp)
        target_include_directories(updater_test PRIVATE src)
        target_link_libraries(updater_test PRIVATE ${MELEE_NETWORK_LIBS} aurora::main)
        target_compile_options(updater_test PRIVATE -UNDEBUG)
        add_test(NAME updater COMMAND updater_test)
        add_test(NAME updater_browser COMMAND updater_test browser)
        set_tests_properties(updater updater_browser PROPERTIES LABELS melee TIMEOUT 10)
        add_dependencies(unit_tests updater_test)
    endif ()
endif ()
