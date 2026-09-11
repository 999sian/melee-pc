#!/bin/sh
#
# Drive 1-P Classic to a playable fight and hold there.
#
#   tools/classic_run.sh [seconds]
#
# Classic needs only one player, so it avoids the port-2 CPU toggle that the
# VS harness could never set reliably. The menu path is fully deterministic
# (verified by reading each screen):
#
#   Start            movie -> title
#   Start            title -> main menu       (1-P Mode already highlighted)
#   A                main menu -> 1-P submenu (Regular Match already highlighted)
#   A                submenu -> Regular Match (Classic already highlighted)
#   A                -> Classic character select
#   pick + Start     -> first fight
#
# An earlier version of this script walked the cursor with repeated Up
# presses, which moved it onto the mode-cycle control and silently switched
# to Training. There is no remembered-submenu problem: every screen above
# opens with the entry we want already highlighted, so no nudging is needed.
#
# Reports liveness through tools/is_paused.py (positive pause-banner
# detection) and exits nonzero if the fight never starts, rather than
# reporting a survival it did not earn.
set -e
cd "$(dirname "$0")/.."
secs=${1:-120}
export MELEE_WINDOW_TITLE=melee-pc-test MELEE_KEYMAP=port

k() { python3 tools/devctl.py key "$1" >/dev/null 2>&1; }
shot() { python3 tools/devctl.py shot "$1" >/dev/null 2>&1; }

sleep 13
k Return; sleep 5        # skip opening movie
k Return; sleep 4        # title -> main menu
k x;      sleep 4        # -> 1-P submenu
k x;      sleep 4        # -> Regular Match
k x;      sleep 5        # -> Classic character select

# Mario's portrait: ~(68,57) in a 400x300 view, x3.2 for the 1280x960 frame.
python3 tools/csscursor.py 218 182 >/dev/null 2>&1 || true
k x; sleep 2

# Confirm the pick actually landed: the P1 panel fills with the character.
shot /tmp/classic_pick.png
if ! python3 tools/regioncheck.py /tmp/classic_pick.png 60 380 250 620 >/dev/null 2>&1; then
	echo "note: could not confirm the P1 panel filled; continuing" >&2
fi

k Return; sleep 10       # start the match

# The fight must be running and unpaused before anything is claimed.
if ! tools/ensure_playing.sh 3 >/dev/null 2>&1; then
	echo "FAILED: Classic never reached a running fight" >&2
	exit 1
fi
echo "Classic fight is running"

end=$(( $(date +%s) + secs ))
r=0
while [ "$(date +%s)" -lt "$end" ]; do
	r=$(( r + 1 ))
	if ! pgrep -f "build/melee" >/dev/null; then
		echo "PROCESS GONE during Classic fight at round $r"
		exit 2
	fi
	# Play: attacks, movement, jumps.
	for key in h x z k b h x; do k "$key"; done
	sleep 2
	shot "/tmp/classic_r$r.png"
done
echo "Classic survived ${secs}s over $r rounds"
