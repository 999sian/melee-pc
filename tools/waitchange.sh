#!/bin/sh
# Block until the game window stops matching a reference frame.
#
#   tools/waitchange.sh REF.png [TIMEOUT_S] [MIN_FRACTION]
#
# Boot and load times vary by several seconds, so driving the game with
# fixed sleeps lands different runs on different screens. Syncing on what is
# actually on screen makes a capture sequence reproducible.
ref=${1:?usage: waitchange.sh REF.png [TIMEOUT_S] [MIN_FRACTION]}
timeout=${2:-60}
frac=${3:-0.30}
cd "$(dirname "$0")/.." || exit 1
end=$(( $(date +%s) + timeout ))
while [ "$(date +%s)" -lt "$end" ]; do
    python3 tools/devctl.py shot /tmp/_wait.png >/dev/null 2>&1
    if python3 tools/framediff.py "$ref" /tmp/_wait.png "$frac" >/dev/null 2>&1; then
        exit 0
    fi
    sleep 1
done
echo "waitchange: timed out after ${timeout}s" >&2
exit 1
