# Apple game objects receive dedicated Mach-O data/zerofill sections. This
# wraps (rather than replaces) the GCC compiler bridge and verifies the final
# image before packaging/stripping. Both Apple ld and ld64.lld provide section
# boundary symbols, including after ASLR and dead stripping.
function(melee_enable_apple_snapshots executable game_library)
    if (NOT APPLE)
        return()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    set(_launcher "${_root}/tools/macho_snapshot_compile.py")
    set(_format "${_root}/tools/macho_snapshot.py")
    set(_shared "${_root}/tools/pe_snapshot_compile.py")
    set(_exclusions "${_root}/src/pc/melee_state.ld")
    get_target_property(_existing_launcher ${game_library} C_COMPILER_LAUNCHER)
    set(_wrapped_launcher "${Python3_EXECUTABLE};${_launcher};--exclude-script;${_exclusions};--")
    if (_existing_launcher)
        list(APPEND _wrapped_launcher ${_existing_launcher})
    endif()
    set_property(TARGET ${game_library} PROPERTY C_COMPILER_LAUNCHER "${_wrapped_launcher}")
    target_compile_options(${game_library} PRIVATE -fno-common -fno-lto)
    set_property(TARGET ${game_library} PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    get_target_property(_game_sources ${game_library} SOURCES)
    set_property(SOURCE ${_game_sources} APPEND PROPERTY OBJECT_DEPENDS
        "${_launcher};${_format};${_shared};${_exclusions}")
    target_sources(${executable} PRIVATE "${_root}/src/pc/melee_state_macho.c")
    add_custom_command(TARGET ${executable} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${_root}/tools/macho_snapshot_verify.py"
            "$<TARGET_FILE:${executable}>"
            --tracked HSD_PadLibData HSD_Rumble_804C22E0 gmVsMelee_StartData
            --excluded net HSD_Synth_804D6018
        VERBATIM
        COMMENT "Verifying Apple rollback data ranges and engine exclusions")
    set(MELEE_STATE_SECTIONS ON PARENT_SCOPE)
    message(STATUS "melee: macOS/iOS rollback sections enabled")
endfunction()
