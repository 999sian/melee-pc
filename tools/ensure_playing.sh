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

probe_moves() {
    python3 tools/devctl.py shot /tmp/_lv_a.png >/dev/null 2>&1
    python3 tools/devctl.py hold Right 300 >/dev/null 2>&1
    python3 tools/devctl.py hold Left 300 >/dev/null 2>&1
    python3 tools/devctl.py shot /tmp/_lv_b.png >/dev/null 2>&1
    python3 tools/framediff.py /tmp/_lv_a.png /tmp/_lv_b.png 0.01 >/dev/null 2>&1
}

i=0
while [ "$i" -lt "$tries" ]; do
    i=$((i + 1))
    if probe_moves; then
        echo "playing (responds to input)"
        exit 0
    fi
    echo "no response to input (attempt $i) - sending Start" >&2
    python3 tools/devctl.py key Start >/dev/null 2>&1
    sleep 1
done
echo "FAILED: no response to input; not in a running match" >&2
exit 1
