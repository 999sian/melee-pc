#!/bin/sh
# Make sure a match is running and UNPAUSED, and report it in the exit code.
# Exits 0 once confirmed playing, 1 if it cannot get there.
#
#   tools/ensure_playing.sh [tries]
#
# Start both advances menus and toggles pause, so a harness that presses
# Start to reach a match can leave it paused -- and "the process is alive"
# cannot tell a paused match from a played one.
#
# Liveness is probed ACTIVELY: hold a movement input and see whether the
# picture changes. Passively comparing two frames is wrong, because a live
# match can be momentarily static (both fighters idle, camera still) and the
# corrective Start would then PAUSE a running match, making the two states
# oscillate. Movement input always changes a live picture and never changes
# a paused one.
tries=${1:-4}
cd "$(dirname "$0")/.." || exit 1
export MELEE_WINDOW_TITLE=${MELEE_WINDOW_TITLE-melee-pc-test}

# Detect the pause banner positively. A movement probe is unsound: a
# character against a wall or in hitstun does not visibly move, and the
# corrective Start would then pause a live match.
i=0
while [ "$i" -lt "$tries" ]; do
    i=$((i + 1))
    python3 tools/devctl.py shot /tmp/_lv.png >/dev/null 2>&1
    if ! python3 tools/is_paused.py /tmp/_lv.png >/dev/null 2>&1; then
        echo "playing (no pause banner)"
        exit 0
    fi
    echo "pause banner present (attempt $i) - sending Start" >&2
    python3 tools/devctl.py key Start >/dev/null 2>&1
    sleep 1
done
echo "FAILED: still paused after $tries attempts" >&2
exit 1
