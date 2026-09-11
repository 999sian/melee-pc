#!/bin/sh
# Run the game under gdb so crashes (or SIGINT) print every thread's
# backtrace. Extra args go to melee. Uses X11 so tools/devctl.py can
# drive/capture the window.
cd "$(dirname "$0")/../build" || exit 1
exec env SDL_VIDEO_DRIVER=x11 MELEE_WINDOW_TITLE=${MELEE_WINDOW_TITLE-melee-pc-test} gdb -q -batch \
    -ex "handle SIGUSR1 nostop noprint" \
    -ex run -ex "thread apply all bt 25" \
    --args ./melee "$@"
