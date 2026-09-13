#!/bin/sh
# run.sh plus a FIFO state dump on SIGINT/crash.
cd "$(dirname "$0")/../build" || exit 1
exec env SDL_VIDEO_DRIVER=x11 MELEE_WINDOW_TITLE=${MELEE_WINDOW_TITLE-melee-pc-test} gdb -q -batch \
    -ex "handle SIGUSR1 nostop noprint" \
    -ex run \
    -ex "printf \"FIFO processed=%lu published=%lu streamBase=%lu size=%u cap=%u wake=%u frameActive=%d pendingDraws=%u\\n\", *(unsigned long*)0x11466570, *(unsigned long*)0x11466578, *(unsigned long*)0x11466568, *(unsigned*)0x114664f8, *(unsigned*)0x114664f4, *(unsigned*)0x11466520, *(char*)0x11466584, *(unsigned*)0x11466580" \
    -ex "printf \"PAD qnum=%u qread=%u qwrite=%u qcount=%u qtype=%u  alarms=%p  lb_x48=%u\\n\", *(unsigned char*)0x114091a0, *(unsigned char*)0x114091a1, *(unsigned char*)0x114091a2, *(unsigned char*)0x114091a3, *(unsigned char*)0x114091a4, *(void**)0x113cbbc0, *(unsigned char*)(0x113dcee0+0x48)" \
    -ex "thread apply all bt 25" \
    --args ./melee "$@"
