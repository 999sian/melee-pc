# Verified PE rollback sections for GNU MinGW x86-64. The fixture in
# tools/test_pe_snapshot.py exercises the same launcher/markers under Wine.
# Call only after melee and melee_game exist. Other toolchains remain lockstep.
function(melee_enable_windows_snapshots executable game_library)
    if (NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
        NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        return()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if (NOT CMAKE_OBJCOPY OR NOT CMAKE_OBJDUMP OR NOT CMAKE_NM)
        message(FATAL_ERROR "Windows snapshots require toolchain objcopy, objdump and nm")
    endif()
    set(_launcher "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/pe_snapshot_compile.py")
    set(_verify "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/pe_snapshot_verify.py")
    set(_markers "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/pc/melee_state_pe.c")
    set(_exclusions "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/pc/melee_state.ld")
    get_target_property(_existing_launcher ${game_library} C_COMPILER_LAUNCHER)
    if (NOT _existing_launcher)
        set(_existing_launcher "")
    endif()
    set(_wrapped_launcher "${Python3_EXECUTABLE};${_launcher};--objcopy;${CMAKE_OBJCOPY};--objdump;${CMAKE_OBJDUMP};--exclude-script;${_exclusions};--")
    if (_existing_launcher)
        list(APPEND _wrapped_launcher ${_existing_launcher})
    endif()
    set_property(TARGET ${game_library} PROPERTY C_COMPILER_LAUNCHER "${_wrapped_launcher}")
    target_compile_options(${game_library} PRIVATE -fno-common -fno-lto)
    set_property(TARGET ${game_library} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    get_target_property(_game_sources ${game_library} SOURCES)
    set_property(SOURCE ${_game_sources} APPEND PROPERTY OBJECT_DEPENDS
        "${_launcher};${_exclusions}")
    target_sources(${executable} PRIVATE "${_markers}")
    target_compile_definitions(${executable} PRIVATE MELEE_STATE_SECTIONS=1)
    add_custom_command(TARGET ${executable} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${_verify}" --nm "${CMAKE_NM}"
            "$<TARGET_FILE:${executable}>"
            --tracked HSD_PadLibData HSD_Rumble_804C22E0 gmVsMelee_StartData
            --excluded net HSD_Synth_804D6018
        VERBATIM
        COMMENT "Verifying Windows rollback data ranges and engine exclusions")
    set(MELEE_STATE_SECTIONS ON PARENT_SCOPE)
    message(STATUS "melee: verified GNU MinGW x86-64 rollback sections enabled")
endfunction()
