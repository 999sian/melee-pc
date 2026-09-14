#!/usr/bin/env bash
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam"
export STEAM_COMPAT_DATA_PATH="/tmp/proton_melee_test"
export WINEDEBUG="-all"
exec "/home/sian/.local/share/Steam/compatibilitytools.d/GE-Proton10-32/proton" run "/home/sian/Documents/Smash GC Port /melee-pc/build-win/melee.exe" "$@"
