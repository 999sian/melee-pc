#!/bin/sh
# Drive 1-P Classic and play until it crashes or the time budget runs out.
#
#   tools/classic_run.sh [seconds]
#
# Classic needs only one player, so it avoids the port-2 CPU toggle that the
# VS harness cannot hit reliably. Runs under tools/run.sh (gdb -batch), so a
# crash prints every thread's backtrace into the log.
secs=${1:-150}
cd "$(dirname "$0")/.." || exit 1
export MELEE_WINDOW_TITLE=melee-pc-test
log=/tmp/classic.log
MELEE_HEAP_CHECK=${MELEE_HEAP_CHECK-1} ./tools/run.sh ../../melee.ciso >"$log" 2>&1 &
pid=$!
k() { python3 tools/devctl.py key "$@" >/dev/null 2>&1; }
h() { python3 tools/devctl.py hold "$@" >/dev/null 2>&1; }
shot() { python3 tools/devctl.py shot "$1" >/dev/null 2>&1; }

sleep 11
shot /tmp/_c_boot.png
k Start; sleep 5                      # skip the opening movie
k Start; sleep 4                      # title -> main menu
# The main menu opens on 1-P Mode; its submenu opens on ADVENTURE, not
# Classic, so move up before confirming or the wrong mode launches.
k A; sleep 4
k Up; sleep 1
k A; sleep 7                          # Classic -> 1-P character select

# The blob detector is unreliable on this screen: the bright LEVEL/OPTION
# panel is a bigger white region than the glove, so it locks onto that
# instead. Select by outcome instead of by cursor position -- nudge onto a
# portrait, press A, and check the P1 panel actually filled in.
picked=0
for attempt in 1 2 3 4 5 6; do
    h Up 1500; sleep 0.3            # lift clear of the bottom edge
    h Right $((120 + attempt * 60)); sleep 0.3
    shot /tmp/_c_pre.png
    k A; sleep 1.2
    shot /tmp/_c_post.png
    if python3 tools/framediff.py /tmp/_c_pre.png /tmp/_c_post.png 0.02; then
        picked=1
        break
    fi
    echo "no character selected (attempt $attempt); nudging" >&2
done
if [ "$picked" != 1 ]; then
    echo "FAILED: could not select a character; no fight was entered" >&2
    kill $pid 2>/dev/null
    exit 2
fi

k Start; sleep 10                     # confirm -> versus screen -> stage 1

end=$(( $(date +%s) + secs ))
n=0
while [ "$(date +%s)" -lt "$end" ]; do
    if ! kill -0 $pid 2>/dev/null; then
        echo "CRASHED after ${n} action rounds"
        grep -aE "signal SIGSEGV|signal SIGABRT|assertion|heap stomp|PANIC" "$log" | head -5
        echo "--- backtrace:"
        grep -aE "^#[0-9]+ " "$log" | head -14
        exit 1
    fi
    # mash attacks and movement so the fight actually progresses
    for i in 1 2 3; do h Left 260; k A; k X; k A; h Right 260; k B; done
    n=$((n + 1)); shot "/tmp/classic_$n.png"
    k Start; sleep 1; k Start         # nudge through any results/continue prompt
done
echo "survived ${secs}s over ${n} rounds"
kill $pid 2>/dev/null
