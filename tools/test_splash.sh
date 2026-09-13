#!/bin/bash
set -e
cd "/home/sian/Documents/Smash GC Port /melee-pc"

out_dir="/tmp/classic_splash_test"
mkdir -p "$out_dir"
rm -f "$out_dir"/*.png

echo "Launching melee with MELEE_CLASSIC_STAGE_OVERRIDE=8 MELEE_CLASSIC_TEAM=dk..."
MELEE_CLASSIC_STAGE_OVERRIDE=8 MELEE_CLASSIC_TEAM=dk SDL_VIDEO_DRIVER=x11 \
MELEE_WINDOW_TITLE=melee-pc-test build/melee ../melee.ciso > /tmp/melee_team_test.log 2>&1 &
MELEE_PID=$!

cleanup() {
    kill -9 "$MELEE_PID" 2>/dev/null || true
}
trap cleanup EXIT

export MELEE_WINDOW_TITLE=melee-pc-test MELEE_KEYMAP=port
k() { python3 tools/devctl.py key "$1" >/dev/null 2>&1; }
shot() { python3 tools/devctl.py shot "$1" >/dev/null 2>&1; }

echo "Waiting for game window..."
for i in {1..30}; do
    if python3 -c "from Xlib import display; import os, sys; d=display.Display(); w='melee-pc-test'; root=d.screen().root; sys.exit(0 if any(w in (d.create_resource_object('window', wid).get_wm_name() or '') for wid in root.get_full_property(d.intern_atom('_NET_CLIENT_LIST'), 0).value) else 1)" 2>/dev/null; then
        echo "Found window!"
        break
    fi
    sleep 0.5
done

sleep 5
echo "Skipping intro movie..."
k Return; sleep 3
k Return; sleep 3        # title -> main menu
k x;      sleep 3        # -> 1-P submenu
k x;      sleep 3        # -> Regular Match
k x;      sleep 4        # -> Classic character select

echo "Selecting character..."
python3 tools/csscursor.py 218 182 >/dev/null 2>&1 || true
k x; sleep 1.5

shot "$out_dir/01_css_picked.png"

echo "Starting match (entering intro splash)..."
k Return

# The splash screen lasts about 3-4 seconds before transition
for s in {1..8}; do
    sleep 0.5
    shot "$out_dir/02_splash_$s.png"
done

echo "Screenshots saved to $out_dir:"
ls -la "$out_dir"
