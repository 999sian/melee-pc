#!/bin/sh
# Bisect which effect gfx_id draws the untextured white quad.
#
#   MELEE_EF_SKIP=<lo-hi> on the GAME process suppresses that id range;
#   this script then plays a match and reports the largest solid white blob.
#
#   tools/ef_bisect.sh            # after launching the game with MELEE_EF_SKIP
#
# WARNING: vs_match.sh does not yet pick a deterministic character or stage
# (the stage is chosen with an analog hold from RANDOM, and the character
# cursor sometimes fails to converge). Runs therefore differ in stage AND
# matchup, and totals are NOT comparable across runs. Largest-white-blob is
# used instead of total white fraction because it tracks the artifact rather
# than overall scene brightness, but you must still confirm two runs used the
# same stage and characters before drawing any conclusion from them.
set -e
cd "$(dirname "$0")/.."
rm -f /tmp/atk_*.png
export MELEE_WINDOW_TITLE=melee-pc-test MELEE_KEYMAP=port
tools/vs_match.sh 1 >/dev/null 2>&1
tools/attack_shots.sh >/dev/null 2>&1
python3 tools/whiteblob.py /tmp/atk_*.png | sort -k2 -rn | head -3
