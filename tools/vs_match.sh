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

# The cursor sometimes stalls short of the target and exits nonzero. Without a
# retry the token lands on whatever portrait it stopped on, so runs end up
# with different characters and are not comparable.
for attempt in 1 2 3; do
    c 370 300 12 && break
    echo "P1 cursor missed the portrait (attempt $attempt); retrying" >&2
done
k x; sleep 0.5                                           # drop the token on Ness

# Setting port 2 to CPU is the flaky step: if it does not take, Melee refuses
# to start (a match needs two players) and Start silently does nothing.
#
# The tag is only a few pixels tall and the cursor hotspot is the glove's
# fingertip, so "cursor converged" does not imply "tag was hit". Close the
# loop on the outcome instead: press A, and check the panel actually changed
# (N/A -> CPU). Nudge up slightly between tries, since the fingertip tends to
# settle just below the tag.
port2_set=0
for attempt in 1 2 3 4 5 6; do
    for inner in 1 2 3; do
        c 427 522 10 && break
    done
    shot /tmp/_p2a.png
    k x; sleep 0.8                                       # port 2 tag -> CPU
    shot /tmp/_p2b.png
    if python3 tools/framediff.py /tmp/_p2a.png /tmp/_p2b.png 0.01; then
        port2_set=1
        break
    fi
    echo "port 2 tag not hit (attempt $attempt); nudging" >&2
    h Up 40; sleep 0.3
done
[ "$port2_set" = 1 ] || { echo "FAILED: could not set port 2 to CPU" >&2; exit 1; }

k Return; sleep 6                                        # Start -> stage select

# Pick a fixed stage. The stage cursor is analog, so there is no tap-per-cell;
# instead pin it against the bottom-left corner (the clamp is a deterministic
# origin) and then step out by fixed holds. Saving the frame lets a caller
# confirm two runs really did choose the same stage.
h Left 1200; h Down 1200; sleep 0.4
h Right 260; sleep 0.2; h Up 260; sleep 0.4
shot /tmp/vs_stage.png
k x; sleep 10                                            # start the match
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
