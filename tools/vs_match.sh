#!/bin/sh
# Drive a freshly booted game (with a save) into a 2-minute VS match:
# title -> main menu -> VS Mode -> Melee -> P1 Ness (HMN), P2 CPU -> stage.
# Then mashes attacks until the timer runs out and screenshots the end.
# Expects the game window titled melee-pc-test (tools/run.sh) and Xwayland.
export MELEE_WINDOW_TITLE=${MELEE_WINDOW_TITLE-melee-pc-test}
cd "$(dirname "$0")/.." || exit 1
k() { python3 tools/devctl.py key "$@"; }
h() { python3 tools/devctl.py hold "$@"; }
shot() { python3 tools/devctl.py shot "$1"; }

sleep 11; k Return; sleep 5; k Return; sleep 4          # movie skip, title -> menu
k Down; sleep 1; k x; sleep 4; k x; sleep 8              # VS Mode -> Melee -> CSS
c() { python3 tools/csscursor.py "$@"; }
h Up 1500; sleep 0.5                                     # hand appears (P1 joins as HMN)
c 370 300; k x; sleep 0.5                                # drop the token on Ness (row 2, col 2)
c 405 545; k x; sleep 0.8                                # port 2 tag -> CPU
shot /tmp/vs_css.png
k Return; sleep 6; h Up 200; sleep 0.3; k x; sleep 10    # SSS -> random stage -> fight
n=0
while [ $n -lt "${1:-7}" ]; do
    for i in 1 2 3 4 5 6; do h Left 300; k x; k c; k x; h Right 300; k z; done
    n=$((n+1)); shot "/tmp/vs_$n.png"
done
# sudden death (if any) then results, then back to the CSS and straight into a rematch
for i in 1 2 3 4 5 6 7 8; do h Left 300; k x; k c; k x; h Right 300; k z; sleep 1; done
for i in 1 2 3 4 5 6; do k x; sleep 3; done; shot /tmp/vs_results.png
for i in 1 2 3; do k Return; sleep 4; done; shot /tmp/vs_after.png
k Return; sleep 6; h Up 200; sleep 0.3; k x; sleep 12; shot /tmp/vs_rematch.png
echo done
