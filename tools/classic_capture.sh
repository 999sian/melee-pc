#!/bin/sh
#
# Launch the game with its output captured to a file we control, drive Classic,
# and report what the log says about the gmRegClear crash.
#
#   tools/classic_capture.sh <logfile> [seconds]
#
# The supervised log stream proved unreliable for this (its cursor froze and a
# panic line showed up in a run that had actually survived), so crash evidence
# is read from this file instead.
set -e
cd "$(dirname "$0")/.."
log=${1:-/tmp/classic_capture.log}
secs=${2:-150}
: >"$log"

MELEE_HEAP_CHECK=1 MELEE_SCENE_LOG=1 SDL_VIDEO_DRIVER=x11 \
	MELEE_WINDOW_TITLE=melee-pc-test build/melee ../melee.ciso >"$log" 2>&1 &
gpid=$!

rc=0
tools/classic_run.sh "$secs" || rc=$?

kill "$gpid" 2>/dev/null || true
wait "$gpid" 2>/dev/null || true

echo "--- harness rc=$rc"
echo "--- archive open failures: $(grep -ac 'Cannot open archive' "$log" || true)"
echo "--- jobj panics:           $(grep -ac "jobj don't get" "$log" || true)"
echo "--- scene_model probe hits:$(grep -ac 'scene_model:' "$log" || true)"
grep -a 'Cannot open archive' "$log" | head -3 || true
grep -a 'scene_model:' "$log" | head -4 || true
