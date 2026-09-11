#!/bin/sh
# Boot to the title, enter the attract-mode Vs demo, and report how it went.
#   tools/demo_run.sh <seed> [seconds]   -> prints "survived" or the crash frames
# Needs an existing save (so boot goes straight to the title) and Xwayland.
seed=${1:-1}
secs=${2:-45}
dir=$(dirname "$0")
log=/tmp/demo_$seed.log
cd "$dir/.." || exit 1
export MELEE_WINDOW_TITLE=melee-pc-test
MELEE_HEAP_CHECK=${MELEE_HEAP_CHECK-1} MELEE_SEED=$seed ./tools/run.sh ../../melee.ciso >"$log" 2>&1 &
pid=$!
sleep 12
python3 tools/devctl.py key Return >/dev/null 2>&1; sleep 5
python3 tools/devctl.py key x >/dev/null 2>&1; sleep 4
python3 tools/devctl.py key x >/dev/null 2>&1
end=$(( $(date +%s) + secs ))
mid=$(( $(date +%s) + 60 ))
while kill -0 $pid 2>/dev/null && [ "$(date +%s)" -lt "$end" ]; do
    sleep 1
    if [ "$(date +%s)" -ge "$mid" ]; then
        python3 tools/devctl.py shot "/tmp/demo_${seed}_match.png" >/dev/null 2>&1
        mid=$end
    fi
done
if kill -0 $pid 2>/dev/null; then
    python3 tools/devctl.py shot "/tmp/demo_$seed.png" >/dev/null 2>&1
    kill -INT $pid; sleep 2; kill $pid 2>/dev/null
    echo "seed $seed: survived ${secs}s (shot /tmp/demo_$seed.png)"
else
    echo "seed $seed: died"
    grep -E 'PANIC|received signal|^#[0-9] .*melee-pc/src' "$log" | head -8
fi
