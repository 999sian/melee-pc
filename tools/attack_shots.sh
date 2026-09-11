#!/bin/sh
# Drive an already-running match and capture tightly-spaced frames around
# attacks, to catch the short-lived hit/slash effects. Run after the game is
# already in a match (e.g. via tools/vs_match.sh).
export MELEE_WINDOW_TITLE=${MELEE_WINDOW_TITLE-melee-pc-test}
cd "$(dirname "$0")/.." || exit 1
k() { python3 tools/devctl.py key "$@"; }
h() { python3 tools/devctl.py hold "$@"; }
shot() { python3 tools/devctl.py shot "$1"; }

n=0
for round in 1 2 3 4 5 6 7 8; do
    # walk into the opponent, then attack; grab frames right after the input
    h Right 260
    k A
    n=$((n + 1)); shot "/tmp/atk_$n.png"
    k A
    n=$((n + 1)); shot "/tmp/atk_$n.png"
    h Left 260
    k A
    n=$((n + 1)); shot "/tmp/atk_$n.png"
    k B
    n=$((n + 1)); shot "/tmp/atk_$n.png"
done
echo "captured /tmp/atk_1..$n"
