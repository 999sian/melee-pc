# SPDX-License-Identifier: GPL-3.0-or-later
# Static wasm link: position-independent objects only add GOT indirection.
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)
set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
add_compile_definitions(ENABLE_BACKEND_WEBGPU)
# -fsigned-char: wasm32's default, stated as the top-level CMakeLists.txt does
# for every native target (it returns before reaching that line for this one).
add_compile_options(--use-port=emdawnwebgpu -pthread -fsigned-char)
add_link_options(--use-port=emdawnwebgpu -pthread -sALLOW_MEMORY_GROWTH=1 -sASYNCIFY=1)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/aurora ${CMAKE_BINARY_DIR}/aurora)
# Same floating-point contract as the native build (top-level CMakeLists.txt).
set(MELEE_FP_FLAGS -ffp-contract=off -fno-fast-math)
target_compile_options(aurora_mtx PRIVATE ${MELEE_FP_FLAGS}
  -fno-builtin-sinf -fno-builtin-cosf -fno-builtin-tanf -fno-builtin-atanf
  -include ${CMAKE_SOURCE_DIR}/src/pc/libm/pc_trig.h)
target_compile_options(aurora_gx PRIVATE ${MELEE_FP_FLAGS})
target_compile_options(aurora_gd PRIVATE ${MELEE_FP_FLAGS})
# The upstream PC layer, minus what the browser replaces: main.c and
# disc_open.c (platforms/browser/main.c, dvd.c), and the launcher, updater,
# GameCube adapter and file cache (pc_stubs.c). Keep in step with PC_SOURCES in
# the top-level CMakeLists.txt.
set(BROWSER_PC_SOURCES
 audio.c discfont.c gx.c input_poll.c keyboard.c misc.c os.c region.c stb_vorbis.c thp.c touch.c vi.c widescreen.c
 android_compat.cpp music_stream.cpp textures.cpp
 net.c net_chat.c net_dht.c net_dht_item.c net_handshake.c net_identity.c net_lan.c net_match.c net_rank.c
 net_rank_session.c net_rank_store.c net_reliable.c net_sfx.c net_sim.c net_snapshot.c net_sync.c net_watchdog.c net_wire.c
 libm/pc_atanf.c libm/pc_cosdf.c libm/pc_cosf.c libm/pc_perturb.c libm/pc_rank_exp.c libm/pc_rank_sqrt.c
 libm/pc_rem_pio2_large.c libm/pc_rem_pio2f.c libm/pc_sindf.c libm/pc_sinf.c libm/pc_tandf.c libm/pc_tanf.c)
list(TRANSFORM BROWSER_PC_SOURCES PREPEND ${CMAKE_SOURCE_DIR}/src/pc/)
# Vendored netplay dependencies, listed in PC_SOURCES upstream as well.
list(APPEND BROWSER_PC_SOURCES
 ${CMAKE_SOURCE_DIR}/extern/dht/dht.c ${CMAKE_SOURCE_DIR}/extern/dht/sha1.c
 ${CMAKE_SOURCE_DIR}/extern/monocypher/monocypher.c ${CMAKE_SOURCE_DIR}/extern/monocypher/monocypher-ed25519.c)
file(GLOB_RECURSE GAME_OBJECTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/build/browser/game/*.o")
set_source_files_properties(${GAME_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_executable(melee_browser ${GAME_OBJECTS}
 main.c dvd.c scheduler.c pc_stubs.c
 ${BROWSER_PC_SOURCES})
target_include_directories(melee_browser PRIVATE ${CMAKE_SOURCE_DIR}/extern/aurora/include ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/sdk_include ${CMAKE_SOURCE_DIR}/extern/monocypher)
target_link_libraries(melee_browser PRIVATE aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx aurora::os aurora::card aurora::thp)
target_compile_definitions(melee_browser PRIVATE TARGET_PC=1 MELEE_PC=1 MELEE_USE_BUILTIN_SHA1)
target_compile_options(melee_browser PRIVATE -Wno-incompatible-pointer-types -Wno-int-conversion ${MELEE_FP_FLAGS})
target_link_options(melee_browser PRIVATE -O1 --profiling-funcs -sEMULATE_FUNCTION_POINTER_CASTS=1 -sSTACK_SIZE=8388608 -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=2147483648 -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=0 -sASSERTIONS=0 -lidbfs.js -sFORCE_FILESYSTEM=1 -sEXPORTED_RUNTIME_METHODS=callMain,FS,ENV -sINVOKE_RUN=0 -sASYNCIFY_IMPORTS=browser_disc_read -sASYNCIFY_STACK_SIZE=1048576 -sERROR_ON_UNDEFINED_SYMBOLS=1 -Wl,--error-limit=0)
set_target_properties(melee_browser PROPERTIES SUFFIX ".js")
# emcc's single-pass link fails validation at -O2/-O3 when --fpcast-emu runs
# ahead of the optimizer (direct calls into cast thunks keep the original
# arity), so link at -O1 and optimize the finished module as a second pass.
# Bound inlining after Asyncify: giant functions cause large Safari OMG JIT
# register-allocation spikes, even while the game heap stays at 256 MiB.
find_program(WASM_OPT wasm-opt HINTS "${EMSCRIPTEN_ROOT_PATH}/../bin" REQUIRED)
add_custom_command(TARGET melee_browser POST_BUILD
  COMMAND ${WASM_OPT} -O3 -g --inline-max-combined-binary-size=20000 --one-caller-inline-max-function-size=2000 --mvp-features --enable-threads --enable-bulk-memory --enable-bulk-memory-opt
    --enable-call-indirect-overlong --enable-multivalue --enable-mutable-globals --enable-nontrapping-float-to-int
    --enable-reference-types --enable-sign-ext --pass-arg=directize-initial-contents-immutable
    $<TARGET_FILE_DIR:melee_browser>/melee_browser.wasm -o $<TARGET_FILE_DIR:melee_browser>/melee_browser.wasm
  COMMENT "Optimizing melee_browser.wasm at -O3")

# Keep the standalone validation server runnable from a clean build.
file(GLOB BROWSER_ASSETS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.mjs" "${CMAKE_CURRENT_SOURCE_DIR}/*.js" "${CMAKE_CURRENT_SOURCE_DIR}/*.html")
foreach(asset IN LISTS BROWSER_ASSETS)
  get_filename_component(name "${asset}" NAME)
  configure_file("${asset}" "${CMAKE_CURRENT_BINARY_DIR}/${name}" COPYONLY)
endforeach()
