/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay match handshake: RULES host -> guest, READY back. RULES carries
 * the seed, start_frame (the frame the lobby leaves for the CSS on both
 * peers; each side applies the seed when it learns it and again entering
 * that frame, and frame checksums are only compared from it on, since the
 * two lobbies run different states until then) and everything
 * match-affecting from plan §5 item 7: the memcard GameRules, the
 * item/stage switches from GamePrefs and the frozen-stadium toggle. The
 * guest overwrites its copies (restored at disconnect) so CSS/SSS/match
 * read the same values on both peers; unlock-all is forced on for both
 * (pc_net_rules). */
#include "compat.h"
#include "pc/net_internal.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/gm/gmmain_lib.h>
#pragma GCC diagnostic pop
#include <sysdolphin/baselib/random.h>

#include <SDL3/SDL_timer.h>
#include <stdlib.h>
#include <string.h>

#define REL_RULES 0x01               /* host -> guest {seed, start_frame} */
#define REL_READY 0x02               /* guest -> host */
#define HS_TIMEOUT_MS 15000
#define HS_LEAD_FRAMES 120   /* ponytail: 2 s for READY; a slower link misses the start */

static uint64_t s_hs_t0;
static bool s_rules_on;                 /* a RULES set is in force (host or guest) */
static bool s_rules_frozen;
static bool s_rules_saved;              /* guest: s_rules_orig holds its own values */
static Rules s_rules_orig;

uint32_t pc_net_seed(void) {
    return net.seed;
}

int pc_net_handshake_state(void) {
    return net.hs;
}

static void hs_done(void) {
    net.hs = HS_DONE;
    net.ck_from = net.start_frame;
    net.desync_reported = false; /* anything before start_frame was the lobbies differing */
    pc_log_line("net: handshake done seed=%u start_frame=%d (frame %d)", net.seed, net.start_frame,
                net.tick_frame);
}

/* The match-affecting part of RULES, from (capture) or into (apply) the
 * game's own copies. */
static void rules_capture(Rules* ru) {
    const struct GamePrefs* p = gmMainLib_GetGamePrefs();
    ru->game = *gmMainLib_GetGameRules();
    ru->item_freq = p->item_freq;
    ru->item_mask = p->item_mask;
    ru->stage_mask = p->stage_mask;
    ru->frozen_stadium = pc_is_frozen_stadium_enabled();
}

static void rules_apply(const Rules* ru, bool from_peer) {
    struct GamePrefs* p = gmMainLib_GetGamePrefs();
    if (from_peer && !s_rules_saved) {
        rules_capture(&s_rules_orig);
        s_rules_saved = true;
    }
    *gmMainLib_GetGameRules() = ru->game;
    p->item_freq = ru->item_freq;
    p->item_mask = ru->item_mask;
    p->stage_mask = ru->stage_mask;
    s_rules_frozen = ru->frozen_stadium != 0;
    s_rules_on = true;
    pc_log_line("net: RULES %s mode=%u time=%u stock=%u handicap=%u dmg=%u stage_sel=%u ff=%u "
                "pause=%u sd=%u items=%u/%016llx stages=%08x frozen=%u unlock_all=1",
                from_peer ? "applied" : "in force", ru->game.mode, ru->game.time_limit,
                ru->game.stock_count, ru->game.handicap, ru->game.damage_ratio,
                ru->game.stage_sel, ru->game.friendly_fire, ru->game.pause, ru->game.unk_xc,
                ru->item_freq, (unsigned long long) ru->item_mask, ru->stage_mask,
                ru->frozen_stadium);
}

void rules_restore(void) {
    if (s_rules_saved) {
        s_rules_saved = false;
        rules_apply(&s_rules_orig, false);
        pc_log_line("net: RULES restored own settings");
    }
    s_rules_on = false;
}

bool pc_net_rules(bool* unlock_all, bool* frozen_stadium) {
    if (!s_rules_on) {
        return false;
    }
    *unlock_all = true;
    *frozen_stadium = s_rules_frozen;
    return true;
}

/* What a RULES set must look like before it is applied, NULL when fine. */
static const char* rules_invalid(const Rules* ru) {
    if (ru->hash != rules_hash(*ru)) {
        return "hash mismatch";
    }
    if (ru->start_frame < 0 || ru->start_frame > net.tick_frame + RING * 4) {
        return "start_frame out of range";
    }
    if (ru->game.mode > 3 || ru->game.time_limit > 99 || ru->game.stock_count > 99 ||
        ru->game.damage_ratio < 5 || ru->game.damage_ratio > 20 || ru->item_freq > 5 ||
        ru->stage_mask == 0) {
        return "value out of range";
    }
    return NULL;
}

/* Reliable types below 0x10: the match handshake. */
void handshake_msg(uint8_t type, const uint8_t* payload, int len) {
    if (type == REL_RULES && len == (int) sizeof(Rules)) {
        if (net.hs == HS_DONE || net.hs_host) {
            pc_log_line("net: RULES ignored (%s)", net.hs_host ? "we host" : "already applied");
            return;
        }
        Rules ru;
        memcpy(&ru, payload, sizeof ru);
        wire_rules(&ru);
        const char* bad = rules_invalid(&ru);
        if (bad != NULL) {
            pc_log_line("net: RULES rejected: %s", bad);
            net.hs = HS_FAILED;
            return;
        }
        net.seed = ru.seed;
        net.start_frame = ru.start_frame;
        *HSD_RandSeedPtr = net.seed;
        rules_apply(&ru, true);
        if (net.start_frame <= net.tick_frame) {
            pc_log_line("net: RULES late, start_frame %d already passed (frame %d)", net.start_frame,
                        net.tick_frame);
        }
        if (!pc_net_send_reliable(REL_READY, NULL, 0)) {
            pc_log_line("net: READY not queued, reliable queue full");
        }
        hs_done();
    } else if (type == REL_READY && net.hs == HS_PENDING && net.hs_host) {
        hs_done();
    }
}

/* Drive the pending handshake a step; true once done, with start_frame. */
static bool hs_poll(int32_t* start_frame) {
    if (net.hs == HS_PENDING) {
        recv_inputs();
        if (SDL_GetTicksNS() - s_hs_t0 > HS_TIMEOUT_MS * 1000000ull) {
            net.hs = HS_FAILED;
            pc_log_line("net: handshake timed out, no %s", net.hs_host ? "READY" : "RULES");
        }
    }
    if (net.hs != HS_DONE) {
        return false;
    }
    *start_frame = net.start_frame;
    return true;
}

bool pc_net_host_match(uint32_t seed, int32_t* start_frame) {
    if (net.hs == HS_IDLE) {
        if (!net.active) {
            return false;
        }
        net.hs = HS_PENDING;
        net.hs_host = true;
        s_hs_t0 = SDL_GetTicksNS();
        net.seed = seed;
        *HSD_RandSeedPtr = seed;
        net.start_frame = net.tick_frame + HS_LEAD_FRAMES;
        Rules ru = { seed, net.start_frame };
        rules_capture(&ru);
        ru.hash = rules_hash(ru);
        rules_apply(&ru, false);
        wire_rules(&ru);
        pc_net_send_reliable(REL_RULES, &ru, sizeof ru);
        pc_log_line("net: RULES sent seed=%u start_frame=%d", seed, net.start_frame);
    }
    return hs_poll(start_frame);
}

bool pc_net_guest_wait_match(uint32_t* seed, int32_t* start_frame) {
    if (net.hs == HS_IDLE) {
        if (!net.active) {
            return false;
        }
        net.hs = HS_PENDING; /* RULES may already have landed: then hs is DONE */
        net.hs_host = false;
        s_hs_t0 = SDL_GetTicksNS();
    }
    if (!hs_poll(start_frame)) {
        return false;
    }
    *seed = net.seed;
    return true;
}

/* MELEE_NET_HANDSHAKE_TEST=1: the lobby handshake without a lobby, from
 * frame 300, plus one caller-typed message each way at frame 600. */
void handshake_test(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("MELEE_NET_HANDSHAKE_TEST") != NULL;
    }
    if (!on || net.tick_frame < 300 || net.hs == HS_FAILED) {
        return;
    }
    int32_t sf;
    uint32_t seed;
    if (net.local == 0) {
        pc_net_host_match(1234, &sf);
    } else {
        pc_net_guest_wait_match(&seed, &sf);
    }
    if (net.tick_frame == 600) {
        pc_net_send_reliable(0x10, "ping", 4);
    }
    uint8_t type;
    char buf[REL_MAX];
    int n = pc_net_recv_reliable(&type, buf, sizeof buf);
    if (n >= 0) {
        pc_log_line("net: reliable recv type %02x len %d '%.*s' at frame %d", type, n, n, buf,
                    net.tick_frame);
    }
}
