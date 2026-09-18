# Game code (decomp, compiled as C) source set.
# src/thp (Melee's PPC THP decoder) is replaced by aurora::thp; see src/pc/thp.c.
#
# 981 files across src/melee and src/sysdolphin, so this stays a glob - an
# explicit list there is churn no human reads. A glob silently absorbs a stray
# file and silently drops a renamed one, so the result is pinned against a
# committed manifest and any drift is a hard configure error.
# ponytail: plain sorted text file plus list(REMOVE_ITEM) set difference; no
# python helper, no hashing, no per-directory CMakeLists.

option(MELEE_UPDATE_SOURCE_MANIFEST "Rewrite cmake/game_sources.manifest from the source tree" OFF)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
        src/melee/*.c
        src/sysdolphin/*.c)

set(_melee_manifest "${CMAKE_CURRENT_LIST_DIR}/game_sources.manifest")
set(_melee_glob_rel "")
foreach (_melee_src IN LISTS GAME_SOURCES)
    file(RELATIVE_PATH _melee_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_melee_src}")
    list(APPEND _melee_glob_rel "${_melee_rel}")
endforeach ()
list(SORT _melee_glob_rel)

if (MELEE_UPDATE_SOURCE_MANIFEST)
    string(JOIN "\n" _melee_manifest_text ${_melee_glob_rel})
    file(WRITE "${_melee_manifest}" "${_melee_manifest_text}\n")
    list(LENGTH _melee_glob_rel _melee_count)
    message(STATUS "Rewrote ${_melee_manifest} (${_melee_count} files)")
else ()
    file(STRINGS "${_melee_manifest}" _melee_expected)
    set(_melee_added ${_melee_glob_rel})
    list(REMOVE_ITEM _melee_added ${_melee_expected})
    set(_melee_removed ${_melee_expected})
    list(REMOVE_ITEM _melee_removed ${_melee_glob_rel})
    if (_melee_added OR _melee_removed)
        string(REPLACE ";" "\n    " _melee_added_text "${_melee_added}")
        string(REPLACE ";" "\n    " _melee_removed_text "${_melee_removed}")
        message(FATAL_ERROR
                "game source manifest drift (cmake/game_sources.manifest)\n"
                "  added (on disk, not in manifest):\n    ${_melee_added_text}\n"
                "  removed (in manifest, not on disk):\n    ${_melee_removed_text}\n"
                "If this is intended, regenerate the manifest:\n"
                "  cmake -B build -G Ninja -DMELEE_UPDATE_SOURCE_MANIFEST=ON")
    endif ()
endif ()

list(APPEND GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/pc/vtxarray.c")
# PowerPC/MetroTRK debugger integration; no PC equivalent.
list(FILTER GAME_SOURCES EXCLUDE REGEX "baselib/debugconsole_main\\.c$")
