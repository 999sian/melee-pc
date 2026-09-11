#!/bin/sh
# Capture frames from the attract-mode demo at fixed wall-clock offsets.
#
#   tools/demo_shots.sh <seed> <out_prefix>
#
# The demo needs no input once started and is seeded by MELEE_SEED, so the
# same seed replays the same fight on the same stage with the same
# characters. That makes it a controlled scene for A/B comparisons -- unlike
# a driven VS match, where the character cursor and the RANDOM stage pick
# vary run to run and make frames incomparable.
#
# Extra env (e.g. MELEE_EF_SKIP) is inherited by the game process.
seed=${1:?usage: demo_shots.sh SEED OUT_PREFIX}
out=${2:?usage: demo_shots.sh SEED OUT_PREFIX}
cd "$(dirname "$0")/.." || exit 1
export MELEE_WINDOW_TITLE=melee-pc-test

MELEE_SEED=$seed ./tools/run.sh ../../melee.ciso >"/tmp/demo_shots_$seed.log" 2>&1 &
pid=$!
cleanup() { kill $pid 2>/dev/null; wait $pid 2>/dev/null; }
trap cleanup EXIT

sleep 10
python3 tools/devctl.py shot /tmp/_boot.png >/dev/null 2>&1

# Boot and load times vary by seconds, so sync on what is actually on screen
# rather than on fixed sleeps: skip the movie, wait for the title, start the
# demo, then wait until the fight is up before sampling.
python3 tools/devctl.py key Return >/dev/null 2>&1
tools/waitchange.sh /tmp/_boot.png 40 0.30 || exit 1
python3 tools/devctl.py shot /tmp/_title.png >/dev/null 2>&1
python3 tools/devctl.py key x >/dev/null 2>&1
sleep 3
python3 tools/devctl.py key x >/dev/null 2>&1
tools/waitchange.sh /tmp/_title.png 60 0.40 || exit 1
i=0
while [ $i -lt 12 ]; do
    i=$((i + 1))
    python3 tools/devctl.py shot "${out}_$i.png" >/dev/null 2>&1
    sleep 2
done
echo "captured ${out}_1..$i"
