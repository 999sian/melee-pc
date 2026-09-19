/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay match handshake: RULES host -> guest, READY back. RULES carries
 * the seed, start_frame (the frame the lobby leaves for the CSS on both
 * peers; each side applies the seed when it learns it and again entering
 * that frame, and frame checksums are only compared from it on, since the
 * two lobbies run different states until then) and everything
 * match-affecting from plan §5 item 7: the memcard GameRules, the
 * item/stage switches from GamePrefs and the frozen-stadium toggle. The
 * guest overwrites its copies (restored at disconnect) so CSS/SSS/match
 * read the same values on both peers.
 *
 * Unlock state gets the same treatment, but written for real: both sides
 * pin the whole unlock surface to pc_unlock_state_all() before RULES is
 * built (unlock_force below) and put the player's own back at disconnect,
 * and RULES/READY each carry an FNV hash of what the masks actually came
 * out as, so two builds that disagree refuse the handshake. Before this,
 * unlock-all was a read-time override on three of the four predicates and
 * the direct mask readers bypassed it, so the two peers could differ by a
 * single HSD_Randi draw and desync on the first frame of the match.
 *
 * Freshness: each side draws a 64-bit nonce from the platform CSPRNG at
 * handshake time. RULES carries the host's; READY carries the guest's plus
 * the host's echoed back, and the session id is folded into both payload
 * hashes (rules_hash/ready_hash, net_wire.c). So a RULES/READY captured off
 * one session cannot be replayed into another, and a stale process at the
 * peer's address cannot drive the handshake with an old payload. After the
 * handshake is done a further RULES or READY is logged once and dropped,
 * never applied.
 *
 * What this is NOT: authentication. The nonces travel in the clear, so an
 * on-path attacker who can read them can still forge either side and
 * impersonate a peer; and the session id (net.c:879) is a perf-counter/pid
 * mix, not a secret. This raises the bar to "must see the traffic" and no
 * higher. Real peer identity is the M5 ed25519 work in docs/netcode-plan.md
 * §9 (signed RULES/READY with a long-term key); Monocypher is not vendored
 * yet, so none of it is implemented here. */
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

/* ---- nonce randomness -------------------------------------------------
 * The nonces are the only value in the session that must be unguessable,
 * so they come from the platform CSPRNG and nowhere else: pc_install_id()
 * (pc.h:65) is persistent and public, and HSD_Rand is the game's
 * deterministic RNG whose seed is on the wire. There is deliberately no
 * fallback — a machine that cannot produce 8 random bytes fails the
 * handshake instead of producing a predictable nonce. */
#if defined(MELEE_USE_BCRYPT)
#include <bcrypt.h>
static bool csprng(void* out, size_t n) {
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}
#else
#include <errno.h>
/* Bionic declares getrandom() only from API 28 and this ships against 26, so
 * on older Android the /dev/urandom path below is the one that runs. */
#if defined(__ANDROID__)
#if __ANDROID_API__ >= 28
#define MELEE_HAVE_GETRANDOM 1
#endif
#elif defined(__linux__)
#define MELEE_HAVE_GETRANDOM 1
#endif
#if defined(MELEE_HAVE_GETRANDOM)
#include <sys/random.h>
#endif
static bool csprng(void* out, size_t n) {
    size_t got = 0;
#if defined(MELEE_HAVE_GETRANDOM)
    while (got < n) {
        ssize_t r = getrandom((uint8_t*)out + got, n - got, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        got += (size_t)r;
    }
    if (got == n) {
        return true;
    }
    got = 0;
#endif
    /* no getrandom (pre-3.17 kernel, a seccomp filter, or a BSD build
     * without it): the classic source, drawn from the same pool */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    while (got < n) {
        ssize_t r = read(fd, (uint8_t*)out + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}
#endif

#define REL_RULES 0x01 /* host -> guest {seed, start_frame, nonce} */
#define REL_READY 0x02 /* guest -> host {nonce, echo} */
#define HS_TIMEOUT_MS 15000
#define HS_LEAD_FRAMES 120 /* ponytail: 2 s for READY; a slower link misses the start */

static uint64_t s_hs_t0;
static bool s_rules_on; /* a RULES set is in force (host or guest) */
static bool s_rules_frozen;
static bool s_rules_saved; /* guest: s_rules_orig holds its own values */
static Rules s_rules_orig;
static uint64_t s_nonce_local;   /* ours this session; 0: not drawn yet */
static uint32_t s_nonce_session; /* net.session s_nonce_local was drawn for */
static uint64_t s_nonce_peer;    /* theirs, from RULES (guest) or READY (host) */
static bool s_unlock_saved;      /* s_unlock_orig holds what the player had */
static uint64_t s_unlock_orig;
/* One log line per refusal class per session (the log-line rule): a peer, or
 * a stale process at its address, that keeps resending must not flood it. */
enum {
    LOG_RULES_HOST = 1 << 0,
    LOG_RULES_LEN = 1 << 1,
    LOG_RULES_DUP = 1 << 2,
    LOG_RULES_CONFLICT = 1 << 3,
    LOG_READY_GUEST = 1 << 4,
    LOG_READY_DONE = 1 << 5,
    LOG_READY_IDLE = 1 << 6,
    LOG_READY_LEN = 1 << 7,
    LOG_READY_HASH = 1 << 8,
    LOG_READY_NONCE = 1 << 9,
};
static uint32_t s_hs_logged; /* LOG_* classes already logged this session */

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
        ru->game.stock_count, ru->game.handicap, ru->game.damage_ratio, ru->game.stage_sel,
        ru->game.friendly_fire, ru->game.pause, ru->game.unk_xc, ru->item_freq,
        (unsigned long long)ru->item_mask, ru->stage_mask, ru->frozen_stadium);
}

/* ---- unlock state -----------------------------------------------------
 * Unlock progress is per-install save data (or, with no card, whatever the
 * card-absent default builder wrote), and several unlock predicates gate
 * HSD_Rand draws: gm_80164ABC gates the alternate-BGM HSD_Randi(100) roll
 * reached from ground.c case 6, and gmMainLib_8015ECBC gates an
 * HSD_Randi(4). One peer drawing where the other does not is a one-draw
 * seed divergence that shows up as a desync on the first frame that
 * advances fighter state, with every fighter field still bit-identical.
 *
 * So the session pins the whole unlock surface: pc_unlock_state_all() is
 * written into the real masks before RULES is built, which is what makes
 * every reader agree -- the three predicates with a pc_is_unlock_all_enabled
 * short circuit, gm_80164600 which has none, and the direct mask readers in
 * gm_1601.c and gm_16F1.c that bypass any read-time override. The state
 * each side actually ended up with is hashed onto the wire and compared, so
 * two builds that disagree about what "all unlocked" means refuse the
 * handshake instead of desyncing mid-match.
 *
 * These masks are the RAM save image, and rules_restore() puts the player's
 * own back at disconnect. Nothing reaches the card in between: the only
 * flush is lbCardGame_SaveChanges(), whose callers are all single-player
 * mode-end paths (gm_17C0.c, gmmultiman.c, gmhomerun.c, gmscmemcard.c,
 * tyfigupon.c), so a VS session never saves and a crash mid-session loses
 * only the forced value, never the card.
 * ponytail: that is "no VS path saves today", not an enforced invariant. A
 * save trigger added to a VS path must run after rules_restore(), or the
 * flush has to be suppressed while s_unlock_saved is set. */
static void unlock_force(void) {
    if (s_unlock_saved) {
        return; /* already pinned for this session */
    }
    s_unlock_orig = pc_unlock_state_get();
    s_unlock_saved = true;
    pc_unlock_state_set(pc_unlock_state_all());
    pc_log_line("net: unlock forced %016llx (was %016llx)",
        (unsigned long long)pc_unlock_state_get(), (unsigned long long)s_unlock_orig);
}

static void unlock_restore(void) {
    if (s_unlock_saved) {
        s_unlock_saved = false;
        pc_unlock_state_set(s_unlock_orig);
        pc_log_line("net: unlock state restored (%016llx)", (unsigned long long)s_unlock_orig);
    }
}

/* Hash of the unlock state as read back out of the save data, not of the
 * constant we asked for: what goes on the wire is what the readers will
 * see. Folded into the RULES/READY hash discipline by riding inside their
 * wire images, so tampering with it fails the payload hash first. */
static uint32_t unlock_hash_now(void) {
    uint64_t s = pc_unlock_state_get();
    uint8_t be[8];
    for (int i = 0; i < 8; i++) {
        be[i] = (uint8_t)(s >> (56 - 8 * i));
    }
    return fnv1a(2166136261u, be, sizeof be);
}

void rules_restore(void) {
    unlock_restore();
    if (s_rules_saved) {
        s_rules_saved = false;
        rules_apply(&s_rules_orig, false);
        pc_log_line("net: RULES restored own settings");
    }
    s_rules_on = false;
    /* The session is over; its nonces must never be reused, and the next
     * one gets a fresh log budget for each refusal class. */
    s_nonce_local = s_nonce_peer = 0;
    s_hs_logged = 0;
}

bool pc_net_rules(bool* unlock_all, bool* frozen_stadium) {
    if (!s_rules_on) {
        return false;
    }
    *unlock_all = true;
    *frozen_stadium = s_rules_frozen;
    return true;
}

/* What a RULES set must look like before it is applied, NULL when fine. The
 * hash binds the session id (rules_hash, net_wire.c), so a RULES captured
 * off an earlier session between the same two peers fails here instead of
 * replaying into this one. */
static const char* rules_invalid(const Rules* ru) {
    if (ru->hash != rules_hash(*ru, net.session)) {
        return "hash mismatch";
    }
    if (ru->nonce == 0) {
        return "no nonce"; /* the sender's CSPRNG failed, or a forgery */
    }
    if (ru->start_frame < 0 || ru->start_frame > net.tick_frame + RING * 4) {
        return "start_frame out of range";
    }
    if (ru->game.mode > 3 || ru->game.time_limit > 99 || ru->game.stock_count > 99 ||
        ru->game.damage_ratio < 5 || ru->game.damage_ratio > 20 || ru->item_freq > 5 ||
        ru->stage_mask == 0)
    {
        return "value out of range";
    }
    return NULL;
}

static void hs_drop(uint32_t cls, const char* what, const char* why) {
    if ((s_hs_logged & cls) == 0) {
        s_hs_logged |= cls;
        pc_log_line("net: %s ignored (%s)", what, why);
    }
}

/* Our nonce for this session, drawn on first use: RULES can land before the
 * lobby calls pc_net_guest_wait_match. Keyed on net.session so a second
 * session can never inherit the first one's nonce, whatever order connect,
 * disconnect and the lobby run in. 0 means the CSPRNG failed and the
 * handshake must not proceed.
 * ponytail: 0 doubles as "not drawn yet", so an all-zero draw (2^-64) is
 * simply drawn again on the next call. */
static uint64_t nonce_local(void) {
    if (s_nonce_session != net.session) {
        s_nonce_session = net.session;
        s_nonce_local = 0;
    }
    if (s_nonce_local == 0 && !csprng(&s_nonce_local, sizeof s_nonce_local)) {
        s_nonce_local = 0;
    }
    return s_nonce_local;
}

static void on_rules(const uint8_t* payload, int len) {
    if (net.hs_host) {
        hs_drop(LOG_RULES_HOST, "RULES", "we host");
        return;
    }
    if (len != (int)sizeof(Rules)) {
        hs_drop(LOG_RULES_LEN, "RULES", "wrong length");
        return;
    }
    Rules ru;
    memcpy(&ru, payload, sizeof ru);
    wire_rules(&ru);
    if (net.hs == HS_DONE) {
        /* A plain retransmit never reaches here (the reliable lane dedups by
         * sequence, net_reliable.c), so this is a second, distinct RULES:
         * either the host changed its mind too late or someone injected it.
         * Logged once and dropped; the rules in force do not move. */
        hs_drop(ru.nonce == s_nonce_peer ? LOG_RULES_DUP : LOG_RULES_CONFLICT, "RULES",
            ru.nonce == s_nonce_peer ? "already applied" : "conflicting nonce after done");
        return;
    }
    const char* bad = rules_invalid(&ru);
    if (bad != NULL) {
        pc_log_line("net: RULES rejected: %s", bad);
        net.hs = HS_FAILED;
        return;
    }
    /* Pin our unlock surface, then check the host's came out the same. Both
     * sides write the same constant, so a mismatch means the two builds
     * disagree about what "all unlocked" is -- one peer would run with a
     * reader still saying "locked", which is a silent mid-match seed
     * divergence. Refuse instead (local://UnlockSync-doc.md).
     * ponytail: refusing sends no READY, so the host only finds out from its
     * own 15 s timeout. Sending a READY and then failing would cut that to
     * one RTT, but it would make the guest's refusal depend on the host
     * checking too; a NAK message type is the fix if this path ever stops
     * being one-in-never. */
    unlock_force();
    uint32_t unlock_mine = unlock_hash_now();
    if (unlock_mine != ru.unlock_hash) {
        pc_log_line("net: RULES rejected: unlock state mismatch (ours %08x/%016llx, host %08x)",
            unlock_mine, (unsigned long long)pc_unlock_state_get(), ru.unlock_hash);
        unlock_restore();
        net.hs = HS_FAILED;
        return;
    }
    Ready rd = {nonce_local(), ru.nonce, unlock_mine, 0};
    if (rd.nonce == 0) {
        pc_log_line("net: RULES rejected: no random source");
        unlock_restore();
        net.hs = HS_FAILED;
        return;
    }
    rd.hash = ready_hash(rd, net.session);
    s_nonce_peer = ru.nonce;
    net.seed = ru.seed;
    net.start_frame = ru.start_frame;
    *HSD_RandSeedPtr = net.seed;
    rules_apply(&ru, true);
    if (net.start_frame <= net.tick_frame) {
        pc_log_line("net: RULES late, start_frame %d already passed (frame %d)", net.start_frame,
            net.tick_frame);
    }
    wire_ready(&rd);
    if (!pc_net_send_reliable(REL_READY, &rd, sizeof rd)) {
        pc_log_line("net: READY not queued, reliable queue full");
    }
    hs_done();
}

static void on_ready(const uint8_t* payload, int len) {
    if (!net.hs_host) {
        hs_drop(LOG_READY_GUEST, "READY", "we are the guest");
        return;
    }
    if (net.hs == HS_DONE) {
        hs_drop(LOG_READY_DONE, "READY", "already done");
        return;
    }
    if (net.hs != HS_PENDING) {
        hs_drop(LOG_READY_IDLE, "READY", "no handshake pending");
        return;
    }
    if (len != (int)sizeof(Ready)) {
        hs_drop(LOG_READY_LEN, "READY", "wrong length");
        return;
    }
    Ready rd;
    memcpy(&rd, payload, sizeof rd);
    wire_ready(&rd);
    /* A READY is ours only if it hashes under this session id and echoes the
     * nonce we put in RULES; a capture from any earlier session fails both,
     * and an off-path forgery has to guess 64 bits. Refusals are dropped,
     * not failed, so an injected READY cannot end a live handshake — the
     * genuine one still arrives, or the 15 s timeout fires. */
    if (rd.hash != ready_hash(rd, net.session)) {
        hs_drop(LOG_READY_HASH, "READY", "hash mismatch");
        return;
    }
    if (s_nonce_local == 0 || rd.echo != s_nonce_local) {
        hs_drop(LOG_READY_NONCE, "READY", "echoed nonce mismatch");
        return;
    }
    /* The guest's unlock hash rides inside the image the two checks above
     * just authenticated, so a mismatch here is a real disagreement rather
     * than an injection: fail hard instead of waiting out the timeout. */
    if (rd.unlock_hash != unlock_hash_now()) {
        pc_log_line("net: READY rejected: unlock state mismatch (ours %08x/%016llx, guest %08x)",
            unlock_hash_now(), (unsigned long long)pc_unlock_state_get(), rd.unlock_hash);
        net.hs = HS_FAILED;
        return;
    }
    s_nonce_peer = rd.nonce;
    hs_done();
}

/* Reliable types below 0x10: the match handshake. */
void handshake_msg(uint8_t type, const uint8_t* payload, int len) {
    if (type == REL_RULES) {
        on_rules(payload, len);
    } else if (type == REL_READY) {
        on_ready(payload, len);
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
        net.hs_host = true;
        if (nonce_local() == 0) {
            /* No CSPRNG: refuse rather than send a guessable nonce. */
            net.hs = HS_FAILED;
            pc_log_line("net: handshake failed, no random source");
            return false;
        }
        net.hs = HS_PENDING;
        s_hs_t0 = SDL_GetTicksNS();
        net.seed = seed;
        *HSD_RandSeedPtr = seed;
        net.start_frame = net.tick_frame + HS_LEAD_FRAMES;
        Rules ru = {0};
        ru.seed = seed;
        ru.start_frame = net.start_frame;
        ru.nonce = s_nonce_local;
        rules_capture(&ru);
        unlock_force();
        ru.unlock_hash = unlock_hash_now();
        ru.hash = rules_hash(ru, net.session);
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
