#!/bin/sh
# Drive a target (the port or Dolphin) from the title screen into a 1-vs-CPU
# VS match and capture frames at fixed offsets, so the port and the emulator
# can be compared frame-for-frame on the same scene.
#
#   MELEE_WINDOW_TITLE=melee-pc-test                 tools/vs_compare.sh /tmp/port
#   MELEE_WINDOW_TITLE=Dolphin MELEE_KEYMAP=dolphin  tools/vs_compare.sh /tmp/dol
#
# The stage cursor is left where it starts (deterministic on both targets), so
# both runs land on the same stage with the same characters.
set -e
out=${1:?usage: vs_compare.sh OUT_PREFIX}
cd "$(dirname "$0")"
k() { python3 devctl.py key "$@"; }
h() { python3 devctl.py hold "$@"; }
shot() { python3 devctl.py shot "$1"; }

k Start; sleep 5            # skip the opening movie
k Start; sleep 4            # title -> main menu
k Down; sleep 1             # main menu starts on 1-P Mode; move to VS Mode
k A; sleep 4                # VS Mode
k A; sleep 6                # Melee -> character select
h Up 1500                   # raise the hand off the bottom edge
sleep 0.5
python3 csscursor.py 365 300   # drop the token on a fixed portrait
k A; sleep 1
k Start; sleep 6            # -> stage select
k A; sleep 10               # pick the stage under the cursor; match loads

# Fixed-offset frames: the match plays itself out the same way on both.
for i in 1 2 3 4 5 6; do
    shot "${out}_$i.png"
    sleep 4
done
echo "captured ${out}_1..6"
