/* Groups the decomp's static data so the netplay snapshot (src/pc/net_snapshot.c) can
 * copy it as two ranges. Everything from libmelee_game.a is game state except:
 *   - the sound machine (axdriver/synth/lbaudio_ax): the SDL audio thread
 *     owns those statics; restoring them would tear its lists;
 *   - perf.c (wall-clock stats), video.c (XFB/VI state and the anti-alias
 *     copy target, written by the render side), vtxarray.c (host-side
 *     vertex-size cache), lbmthp.c (THP movie player, wall-clock paced) and
 *     card.c (async memory-card context): engine state, not sim state;
 *   - lb_0195.c: the 1/60 s pad alarm (an OSAlarm whose fire time is
 *     wall-clock; rewinding it makes the alarm catch up and queue extra
 *     ticks after every rollback) and its frame-skip accumulator, which is
 *     idempotent per tick at 60 Hz;
 *   - devcom.c: the disc/ARAM request queues, busy flags and relay buffers
 *     driven from the DVD/ARQ worker threads (the music stream keeps them
 *     busy all match; rewinding them under a completion crashed
 *     HSD_DevComDVDMemCallback).
 * Those stay in the ordinary sections with aurora/SDL/Dawn. */
SECTIONS
{
    .melee_data :
    {
        __melee_data_start = .;
        *libmelee_game.a:*(EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o *lb_0195.c.o *devcom.c.o) .data
                          EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o *lb_0195.c.o *devcom.c.o) .data.*)
        __melee_data_end = .;
    }
}
INSERT AFTER .data;

SECTIONS
{
    .melee_bss (NOLOAD) :
    {
        __melee_bss_start = .;
        *libmelee_game.a:*(EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o *lb_0195.c.o *devcom.c.o) .bss
                          EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o *lb_0195.c.o *devcom.c.o) .bss.*
                          EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o *lb_0195.c.o *devcom.c.o) COMMON)
        __melee_bss_end = .;
    }
}
INSERT AFTER .bss;
