/* Groups the decomp's static data so the netplay snapshot (src/pc/net.c) can
 * copy it as two ranges. Everything from libmelee_game.a is game state except:
 *   - the sound machine (axdriver/synth/lbaudio_ax): the SDL audio thread
 *     owns those statics; restoring them would tear its lists;
 *   - perf.c (wall-clock stats), video.c (XFB/VI state and the anti-alias
 *     copy target, written by the render side), vtxarray.c (host-side
 *     vertex-size cache), lbmthp.c (THP movie player, wall-clock paced) and
 *     card.c (async memory-card context): engine state, not sim state.
 * Those stay in the ordinary sections with aurora/SDL/Dawn. */
SECTIONS
{
    .melee_data :
    {
        __melee_data_start = .;
        *libmelee_game.a:(EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o) .data
                          EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o) .data.*)
        __melee_data_end = .;
    }
}
INSERT AFTER .data;

SECTIONS
{
    .melee_bss (NOLOAD) :
    {
        __melee_bss_start = .;
        *libmelee_game.a:(EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o) .bss
                          EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o) .bss.*
                          EXCLUDE_FILE(*axdriver.c.o *synth.c.o *lbaudio_ax.c.o *perf.c.o *video.c.o *vtxarray.c.o *lbmthp.c.o *card.c.o) COMMON)
        __melee_bss_end = .;
    }
}
INSERT AFTER .bss;
