#!/bin/sh
# Run the game under gdb so crashes print a backtrace. Extra args go to melee.
# Uses X11 so tools/devctl.py can drive/capture the window.
cd "$(dirname "$0")/../build" || exit 1
exec env SDL_VIDEO_DRIVER=x11 gdb -q -batch \
    -ex "handle SIGUSR1 nostop noprint" \
    -ex run -ex "bt 30" -ex "info registers rip" \
    --args ./melee "$@"
