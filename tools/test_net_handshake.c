/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Self-check for the nonce-bound match handshake in src/pc/net_handshake.c.
 * One process plays both ends: the module's per-session state is a handful
 * of statics, visible here because the .c is included, so side_save/
 * side_load swap the host's for the guest's between steps. net_wire.c is
 * included for real, so the byte order and both payload hashes are the
 * shipping ones. Proves: a correct exchange completes and binds both
 * nonces; a RULES or READY captured off one session is refused in the next;
 * a second, conflicting RULES after the handshake is dropped and logged
 * once; a tampered hash is refused; a forged READY cannot end a live
 * handshake; two sides with *different* unlock progress both end up pinned
 * to the same forced state and complete (checks 1, 12), while a peer whose
 * forced state hashes differently is refused on either RULES or READY with
 * the player's own masks put back (checks 11, 13). From the repo root:
 *   cc -DTARGET_PC=1 -DMELEE_PC=1 -DNDEBUG -std=gnu11 \
 *      -I extern/aurora/include -I src -I src/sdk_include \
 *      -I build/_deps/sdl-src/include \
 *      tools/test_net_handshake.c -o /tmp/test_net_handshake && \
 *      /tmp/test_net_handshake
 * extern/aurora/include MUST come before src/sdk_include: aurora's
 * dolphin/* headers shadow the decomp's and the build depends on that, so
 * with sdk_include first the disc-size asserts in melee/lb/forward.h fail
 * before this file is even reached. -DNDEBUG mirrors the real build, which
 * is why assert is redefined below. */
#include "../src/pc/net_handshake.c"
#include "../src/pc/net_wire.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* not <assert.h>: the decomp's debug.h owns __assert, and -DNDEBUG must not blind this */
#define assert(c)                                                                                  \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

struct NetSession net;

/* ---- stubbed dependencies --------------------------------------------- */

static uint64_t s_now = 1;
static uint8_t s_out[REL_MAX]; /* last reliable payload queued */
static int s_out_len = -1;     /* -1: nothing queued since the last clear */
static uint8_t s_out_type;
static char s_log[64][192];
static int s_logn;

static GameRules s_game;
static struct GamePrefs s_prefs;
static u32 s_seed_store;
u32* HSD_RandSeedPtr = &s_seed_store;

/* The unlock surface (src/melee/gm/gmmain_lib.c behind the real thing): one
 * scalar standing in for the save-data masks, so a test can give the two
 * sides different starting progress, and s_unlock_all can be moved to model
 * a peer whose build disagrees about what "all unlocked" means. */
static uint64_t s_unlock_live;
static uint64_t s_unlock_all = 0x07ff07ffff010101ull;
uint64_t pc_unlock_state_get(void) {
    return s_unlock_live;
}
void pc_unlock_state_set(uint64_t s) {
    s_unlock_live = s;
}
uint64_t pc_unlock_state_all(void) {
    return s_unlock_all;
}

Uint64 SDL_GetTicksNS(void) {
    return s_now;
}
/* The handshake installs the session key under tx_lock (net_handshake.c); one
 * thread here, so the lock itself is nothing. */
void SDL_LockMutex(SDL_Mutex* m) {
    (void)m;
}
void SDL_UnlockMutex(SDL_Mutex* m) {
    (void)m;
}
void recv_inputs(void) {}
GameRules* gmMainLib_GetGameRules(void) {
    return &s_game;
}
struct GamePrefs* gmMainLib_GetGamePrefs(void) {
    return &s_prefs;
}
bool pc_is_frozen_stadium_enabled(void) {
    return true;
}
int pc_net_recv_reliable(uint8_t* t, void* p, int max) {
    (void)t;
    (void)p;
    (void)max;
    return -1;
}

static bool s_send_full; /* the reliable lane has no room (net_reliable.c) */
bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    if (s_send_full) {
        return false;
    }
    s_out_type = type;
    s_out_len = len;
    if (len > 0) {
        memcpy(s_out, payload, (size_t)len);
    }
    return true;
}

void pc_log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s_logn < (int)(sizeof s_log / sizeof s_log[0])) {
        vsnprintf(s_log[s_logn], sizeof s_log[0], fmt, ap);
        puts(s_log[s_logn++]);
    }
    va_end(ap);
}

static int log_count(const char* needle) {
    int n = 0;
    for (int i = 0; i < s_logn; i++) {
        if (strstr(s_log[i], needle) != NULL) {
            n++;
        }
    }
    return n;
}

/* ---- side swapping ---------------------------------------------------- */

typedef struct Side {
    uint64_t t0, nonce_local, nonce_peer;
    uint32_t nonce_session, logged;
    bool rules_on, rules_frozen, rules_saved;
    Rules rules_orig;
    int hs;
    bool hs_host;
    uint32_t seed, session;
    int32_t start_frame, ck_from;
    int local, remote;
    /* the side's own save data plus what the module saved off it */
    uint64_t unlock_live, unlock_orig;
    bool unlock_saved;
} Side;

static void side_save(Side* s) {
    s->t0 = s_hs_t0;
    s->nonce_local = s_nonce_local;
    s->nonce_peer = s_nonce_peer;
    s->nonce_session = s_nonce_session;
    s->logged = s_hs_logged;
    s->rules_on = s_rules_on;
    s->rules_frozen = s_rules_frozen;
    s->rules_saved = s_rules_saved;
    s->rules_orig = s_rules_orig;
    s->hs = net.hs;
    s->hs_host = net.hs_host;
    s->seed = net.seed;
    s->session = net.session;
    s->start_frame = net.start_frame;
    s->ck_from = net.ck_from;
    s->local = net.local;
    s->remote = net.remote;
    s->unlock_live = s_unlock_live;
    s->unlock_orig = s_unlock_orig;
    s->unlock_saved = s_unlock_saved;
}

static void side_load(const Side* s) {
    s_hs_t0 = s->t0;
    s_nonce_local = s->nonce_local;
    s_nonce_peer = s->nonce_peer;
    s_nonce_session = s->nonce_session;
    s_hs_logged = s->logged;
    s_rules_on = s->rules_on;
    s_rules_frozen = s->rules_frozen;
    s_rules_saved = s->rules_saved;
    s_rules_orig = s->rules_orig;
    net.hs = s->hs;
    net.hs_host = s->hs_host;
    net.seed = s->seed;
    net.session = s->session;
    net.start_frame = s->start_frame;
    net.ck_from = s->ck_from;
    net.local = s->local;
    net.remote = s->remote;
    s_unlock_live = s->unlock_live;
    s_unlock_orig = s->unlock_orig;
    s_unlock_saved = s->unlock_saved;
}

/* Load a side that has just connected to session `id` and learned nothing:
 * HS_IDLE, no nonce, no rules, and its own untouched unlock progress
 * (s_fresh_unlock, which a test moves to make the two sides differ).
 * `local` 1 is the guest, 0 the host. */
static uint64_t s_fresh_unlock = 0x0003000100000000ull; /* partial progress */
static void load_fresh(uint32_t id, int local) {
    Side s = {0};
    s.hs = HS_IDLE;
    s.session = id;
    s.start_frame = -1;
    s.local = local;
    s.remote = 1 - local;
    s.unlock_live = s_fresh_unlock;
    side_load(&s);
}

/* Rebuild a RULES payload for `session` with `nonce`, keeping the rule
 * values from a captured one: the host's own send path in one helper. */
static void forge_rules(uint8_t* out, const uint8_t* from, uint32_t session, uint64_t nonce,
    uint32_t seed, int32_t start_frame) {
    Rules ru;
    memcpy(&ru, from, sizeof ru);
    wire_rules(&ru);
    ru.nonce = nonce;
    ru.seed = seed;
    ru.start_frame = start_frame;
    ru.hash = rules_hash(ru, session);
    wire_rules(&ru);
    memcpy(out, &ru, sizeof ru);
}

/* What unlock_hash_now() will return once a side has forced its masks: the
 * value a peer that agrees puts on the wire, computable before forcing. */
static uint32_t hash_of_all(void) {
    uint64_t was = s_unlock_live;
    uint32_t h;
    s_unlock_live = s_unlock_all;
    h = unlock_hash_now();
    s_unlock_live = was;
    return h;
}

/* Rewrite a packed RULES' unlock hash and re-bind it: a peer whose build
 * disagrees about what "all unlocked" is, with everything else identical. */
static void reforge_unlock(uint8_t* buf, uint32_t session, uint32_t unlock_hash) {
    Rules ru;
    memcpy(&ru, buf, sizeof ru);
    wire_rules(&ru);
    ru.unlock_hash = unlock_hash;
    ru.hash = rules_hash(ru, session);
    wire_rules(&ru);
    memcpy(buf, &ru, sizeof ru);
}

static void forge_ready(
    uint8_t* out, uint32_t session, uint64_t nonce, uint64_t echo, uint32_t unlock_hash) {
    Ready rd = {nonce, echo, unlock_hash, 0};
    rd.hash = ready_hash(rd, session);
    wire_ready(&rd);
    memcpy(out, &rd, sizeof rd);
}

int main(void) {
    const uint32_t sess_a = 0xa1b2c3d4, sess_b = 0x0badf00d, sess_c = 0x77c0ffee;
    uint8_t rules_a[sizeof(Rules)], ready_a[sizeof(Ready)], key_guest[32];
    uint64_t host_nonce, guest_nonce;
    int32_t sf = -1;
    Side host, guest;

    /* every check below depends on these sizes being the wire ones */
    assert(sizeof(Rules) == 16 + sizeof(GameRules) + 22 && sizeof(Ready) == 24);

    s_game.mode = 1;
    s_game.time_limit = 8;
    s_game.stock_count = 4;
    s_game.damage_ratio = 10;
    s_game.stage_sel = 1;
    s_prefs.item_freq = 3;
    s_prefs.item_mask = 0x1234567890abcdefull;
    s_prefs.stage_mask = 0x00ff00ffu;

    /* ---- 1. a correct exchange completes and binds both nonces -------- */
    net.active = true;
    net.tick_frame = 300;
    s_fresh_unlock = 0x0003000100000000ull; /* the host's own save progress */
    load_fresh(sess_a, 0);
    assert(!pc_net_host_match(1234, &sf)); /* queued RULES, now waiting for READY */
    assert(net.hs == HS_PENDING);
    assert(s_out_type == REL_RULES && s_out_len == (int)sizeof(Rules));
    memcpy(rules_a, s_out, sizeof rules_a);
    host_nonce = s_nonce_local;
    assert(host_nonce != 0);
    side_save(&host);
    /* the host pinned its own masks before hashing, and RULES carries that */
    assert(s_unlock_live == s_unlock_all && s_unlock_saved);
    assert(s_unlock_orig == 0x0003000100000000ull);
    {
        Rules ru;
        memcpy(&ru, rules_a, sizeof ru);
        wire_rules(&ru);
        assert(ru.unlock_hash == unlock_hash_now() && ru.unlock_hash != 0);
    }

    s_fresh_unlock = 0x000000ff00000000ull; /* the guest's differs: the regression */
    load_fresh(sess_a, 1);
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, (int)sizeof rules_a);
    assert(net.hs == HS_DONE);
    assert(net.seed == 1234 && net.start_frame == 300 + HS_LEAD_FRAMES);
    assert(s_out_type == REL_READY && s_out_len == (int)sizeof(Ready));
    memcpy(ready_a, s_out, sizeof ready_a);
    guest_nonce = s_nonce_local;
    assert(guest_nonce != 0 && guest_nonce != host_nonce);
    assert(s_nonce_peer == host_nonce);
    {
        Ready rd;
        memcpy(&rd, ready_a, sizeof rd);
        wire_ready(&rd);
        assert(rd.nonce == guest_nonce && rd.echo == host_nonce);
        assert(rd.hash == ready_hash(rd, sess_a));
        assert(rd.unlock_hash == unlock_hash_now());
    }
    /* The guest has both nonces now, so it has the datagram key (net_wire.c)
     * before the READY it just built has even gone out. */
    assert(net_key_ready());
    memcpy(key_guest, s_key, sizeof key_guest);
    /* both peers now hold the same unlock state, each with its own saved */
    assert(s_unlock_live == s_unlock_all && s_unlock_orig == 0x000000ff00000000ull);
    side_save(&guest);

    side_load(&host);
    handshake_msg(REL_READY, ready_a, (int)sizeof ready_a);
    assert(net.hs == HS_DONE && s_nonce_peer == guest_nonce);
    /* ...and the host derives the same 32 bytes from the same session id and
     * the same two nonces, with nothing sent for the key itself. */
    assert(net_key_ready() && memcmp(s_key, key_guest, sizeof key_guest) == 0);
    {
        /* A datagram tagged under it verifies, one bit of the tag or of the
         * body does not, and a key for another session id does not either --
         * so a recording of this session is inert in the next one, whose
         * nonces differ anyway. */
        uint8_t dg[40 + NET_MAC_LEN];
        memset(dg, 0x5A, sizeof dg);
        net_mac_stamp(dg, 40);
        assert(net_mac_ok(dg, 40));
        dg[40] ^= 1;
        assert(!net_mac_ok(dg, 40));
        dg[40] ^= 1;
        dg[7] ^= 1;
        assert(!net_mac_ok(dg, 40));
        dg[7] ^= 1;
        net.session = sess_c;
        net_key_clear();
        net_key_session(host_nonce, guest_nonce);
        assert(memcmp(s_key, key_guest, sizeof key_guest) != 0 && !net_mac_ok(dg, 40));
        net.session = sess_a;
        net_key_clear();
        net_key_session(host_nonce, guest_nonce);
        assert(memcmp(s_key, key_guest, sizeof key_guest) == 0);
    }
    side_save(&host);
    printf("ok 1: exchange completes, host %016llx / guest %016llx bound both ways\n",
        (unsigned long long)host_nonce, (unsigned long long)guest_nonce);

    /* ---- 2. that RULES replayed into the next session is refused ------- */
    load_fresh(sess_b, 1);
    s_logn = 0;
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, (int)sizeof rules_a);
    assert(net.hs == HS_FAILED);
    assert(log_count("net: RULES rejected: hash mismatch") == 1);
    assert(net.seed == 0 && net.start_frame == -1 && s_out_len == -1);
    printf("ok 2: replayed RULES refused in a new session, nothing applied, no READY\n");

    /* Positive control: the same rule values with this session's binding and
     * a fresh nonce are accepted, so it was the binding that refused above. */
    {
        uint8_t rules_b[sizeof(Rules)];
        forge_rules(rules_b, rules_a, sess_b, 0x0123456789abcdefull, 777, 310);
        load_fresh(sess_b, 1);
        s_logn = 0;
        s_out_len = -1;
        net.tick_frame = 300;
        handshake_msg(REL_RULES, rules_b, (int)sizeof rules_b);
        assert(net.hs == HS_DONE && net.seed == 777 && net.start_frame == 310);
        assert(s_out_type == REL_READY && s_out_len == (int)sizeof(Ready));
        assert(s_nonce_peer == 0x0123456789abcdefull);
        printf("ok 3: same rules, this session's binding, fresh nonce -> accepted\n");

        /* ---- 3. a second, conflicting RULES after done is dropped once -- */
        uint8_t rules_c[sizeof(Rules)];
        forge_rules(rules_c, rules_a, sess_b, 0xfedcba9876543210ull, 4321, 500);
        s_logn = 0;
        s_out_len = -1;
        handshake_msg(REL_RULES, rules_c, (int)sizeof rules_c);
        handshake_msg(REL_RULES, rules_c, (int)sizeof rules_c);
        handshake_msg(REL_RULES, rules_c, (int)sizeof rules_c);
        assert(net.hs == HS_DONE && net.seed == 777 && net.start_frame == 310);
        assert(s_nonce_peer == 0x0123456789abcdefull && s_out_len == -1);
        assert(log_count("net: RULES ignored (conflicting nonce after done)") == 1);
        assert(s_logn == 1);
        printf("ok 4: second RULES after done dropped, logged once (3 arrivals, 1 line)\n");

        /* the accepted set arriving again is the benign duplicate class */
        s_logn = 0;
        handshake_msg(REL_RULES, rules_b, (int)sizeof rules_b);
        assert(net.hs == HS_DONE && net.seed == 777);
        assert(log_count("net: RULES ignored (already applied)") == 1 && s_logn == 1);
        printf("ok 5: re-arrival of the applied RULES logged as a duplicate, not a conflict\n");

        /* ---- 4. a tampered hash is refused ------------------------------ */
        uint8_t bad[sizeof(Rules)];
        memcpy(bad, rules_b, sizeof bad);
        bad[sizeof bad - 1] ^= 1u; /* last byte of .hash */
        load_fresh(sess_b, 1);
        s_logn = 0;
        s_out_len = -1;
        handshake_msg(REL_RULES, bad, (int)sizeof bad);
        assert(net.hs == HS_FAILED && net.seed == 0 && s_out_len == -1);
        assert(log_count("net: RULES rejected: hash mismatch") == 1);

        /* the same, tampering a rule value instead of the hash */
        memcpy(bad, rules_b, sizeof bad);
        bad[offsetof(Rules, nonce)] ^= 0x80u;
        load_fresh(sess_b, 1);
        s_logn = 0;
        handshake_msg(REL_RULES, bad, (int)sizeof bad);
        assert(net.hs == HS_FAILED && log_count("net: RULES rejected: hash mismatch") == 1);

        /* a zero nonce never validates: an old peer or a stripped field */
        forge_rules(bad, rules_a, sess_b, 0, 777, 310);
        load_fresh(sess_b, 1);
        s_logn = 0;
        handshake_msg(REL_RULES, bad, (int)sizeof bad);
        assert(net.hs == HS_FAILED && log_count("net: RULES rejected: no nonce") == 1);
        printf("ok 6: tampered hash, tampered nonce and a zero nonce all refused\n");
    }

    /* ---- 5. a forged or replayed READY cannot drive the host ----------- */
    net.tick_frame = 300;
    load_fresh(sess_c, 0);
    assert(!pc_net_host_match(99, &sf));
    assert(net.hs == HS_PENDING);
    {
        uint64_t hn = s_nonce_local;
        uint8_t k[sizeof(Ready)];
        assert(hn != 0 && hn != host_nonce); /* a new session redraws */
        s_logn = 0;

        /* right hash for this session, wrong echo: an off-path forgery */
        forge_ready(k, sess_c, 0x1111111111111111ull, hn ^ 1u, unlock_hash_now());
        handshake_msg(REL_READY, k, (int)sizeof k);
        assert(net.hs == HS_PENDING && s_nonce_peer == 0);
        assert(log_count("net: READY ignored (echoed nonce mismatch)") == 1);

        /* session 1's captured READY: right echo for *that* host nonce, but
         * it hashes under sess_a, so it dies before the nonce check */
        handshake_msg(REL_READY, ready_a, (int)sizeof ready_a);
        assert(net.hs == HS_PENDING && s_nonce_peer == 0);
        assert(log_count("net: READY ignored (hash mismatch)") == 1);

        /* and repeats of both stay at one line per class */
        handshake_msg(REL_READY, k, (int)sizeof k);
        handshake_msg(REL_READY, ready_a, (int)sizeof ready_a);
        assert(s_logn == 2);

        /* the genuine one still completes the handshake afterwards */
        forge_ready(k, sess_c, 0x2222222222222222ull, hn, unlock_hash_now());
        handshake_msg(REL_READY, k, (int)sizeof k);
        assert(net.hs == HS_DONE && s_nonce_peer == 0x2222222222222222ull);
        printf("ok 7: forged and replayed READY dropped (handshake stays PENDING), real one "
               "completes\n");

        /* a further READY after done is dropped and logged once */
        s_logn = 0;
        handshake_msg(REL_READY, k, (int)sizeof k);
        handshake_msg(REL_READY, k, (int)sizeof k);
        assert(net.hs == HS_DONE);
        assert(log_count("net: READY ignored (already done)") == 1 && s_logn == 1);

        printf("ok 8: READY after done dropped and logged once\n");
    }

    /* a short RULES is refused on its length, before any field is read */
    load_fresh(sess_b, 1);
    s_logn = 0;
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, (int)sizeof rules_a - 1);
    assert(net.hs == HS_IDLE && net.seed == 0 && s_out_len == -1);
    assert(log_count("net: RULES ignored (wrong length)") == 1 && s_logn == 1);
    printf("ok 9: short RULES refused on length, handshake untouched\n");
    {
        /* and the host never takes a RULES at all */
        load_fresh(sess_b, 0);
        net.hs_host = true;
        s_logn = 0;
        handshake_msg(REL_RULES, rules_a, (int)sizeof rules_a);
        assert(net.hs == HS_IDLE && log_count("net: RULES ignored (we host)") == 1);
        printf("ok 10: RULES arriving at the host refused\n");
    }

    /* ---- 6. unlock state the two sides disagree on ---------------------
     * Both sides write pc_unlock_state_all() before hashing, so the hashes
     * match whatever save progress each started from (checked in 1 above,
     * where the two differ). A mismatch therefore means the two builds
     * disagree about what "all unlocked" is, i.e. one peer would keep a
     * reader saying "locked" and draw one HSD_Rand fewer: refuse. */
    {
        uint8_t ru_bad[sizeof(Rules)];
        uint32_t peer_hash;

        /* the guest refuses a RULES whose unlock hash is not what it got */
        net.tick_frame = 300;
        s_fresh_unlock = 0x0003000100000000ull;
        load_fresh(sess_b, 1);
        forge_rules(ru_bad, rules_a, sess_b, 0x5555555555555555ull, 777, 310);
        reforge_unlock(ru_bad, sess_b, hash_of_all() ^ 0x1u);
        s_logn = 0;
        s_out_len = -1;
        handshake_msg(REL_RULES, ru_bad, (int)sizeof ru_bad);
        assert(net.hs == HS_FAILED);
        assert(log_count("net: RULES rejected: unlock state mismatch") == 1);
        assert(s_out_len == -1); /* no READY: nothing agreed */
        assert(net.seed == 0 && net.start_frame == -1);
        /* and the refusal left the player's own progress alone */
        assert(!s_unlock_saved && s_unlock_live == 0x0003000100000000ull);
        printf("ok 11: RULES with a differing unlock state refused, own masks restored\n");

        /* the same set with the guest's own hash is accepted: it was the
         * unlock comparison that refused above, not anything else */
        forge_rules(ru_bad, rules_a, sess_b, 0x5555555555555555ull, 777, 310);
        reforge_unlock(ru_bad, sess_b, hash_of_all());
        load_fresh(sess_b, 1);
        s_logn = 0;
        s_out_len = -1;
        handshake_msg(REL_RULES, ru_bad, (int)sizeof ru_bad);
        assert(net.hs == HS_DONE && net.seed == 777);
        assert(s_out_type == REL_READY && s_unlock_live == s_unlock_all);
        printf("ok 12: same RULES with the matching unlock state accepted\n");

        /* the host refuses a READY whose unlock hash differs, at once */
        net.tick_frame = 300;
        load_fresh(sess_c, 0);
        assert(!pc_net_host_match(99, &sf));
        peer_hash = unlock_hash_now() ^ 0x2u;
        {
            uint8_t k[sizeof(Ready)];
            forge_ready(k, sess_c, 0x3333333333333333ull, s_nonce_local, peer_hash);
            s_logn = 0;
            handshake_msg(REL_READY, k, (int)sizeof k);
            assert(net.hs == HS_FAILED);
            assert(log_count("net: READY rejected: unlock state mismatch") == 1);
            assert(s_logn == 1);
        }
        /* rules_restore at disconnect puts the host's own masks back */
        rules_restore();
        assert(!s_unlock_saved && s_unlock_live == 0x0003000100000000ull);
        assert(log_count("net: unlock state restored") == 1);
        printf("ok 13: READY with a differing unlock state refused, restore undoes the force\n");
    }

    /* ---- 14. a message the reliable lane refused never completes a
     * handshake: the guest keeps the READY and the poll that puts it out is
     * what finishes the exchange, and a RULES that never goes out times the
     * host out rather than leaving it agreed with nobody ---------------- */
    {
        uint32_t seed_out = 0;
        net.tick_frame = 300;
        load_fresh(sess_a, 1);
        s_send_full = true;
        s_out_len = -1;
        handshake_msg(REL_RULES, rules_a, (int)sizeof rules_a);
        assert(net.hs != HS_DONE && s_out_len == -1);
        assert(s_hs_tx.len == sizeof(Ready)); /* held for the retry */
        assert(!pc_net_guest_wait_match(&seed_out, &sf) && net.hs == HS_PENDING);
        s_send_full = false;
        assert(pc_net_guest_wait_match(&seed_out, &sf));
        assert(net.hs == HS_DONE && seed_out == 1234 && sf == 300 + HS_LEAD_FRAMES);
        assert(s_out_type == REL_READY && s_out_len == (int)sizeof(Ready) && s_hs_tx.len == 0);
        rules_restore();

        net.tick_frame = 300;
        load_fresh(sess_c, 0);
        s_send_full = true;
        s_out_len = -1;
        assert(!pc_net_host_match(55, &sf) && net.hs == HS_PENDING);
        assert(s_out_len == -1 && s_hs_tx.len == sizeof(Rules));
        s_now += (uint64_t)HS_TIMEOUT_MS * 1000000ull + 1;
        assert(!pc_net_host_match(55, &sf) && net.hs == HS_FAILED);
        s_send_full = false;
        rules_restore();
        assert(s_hs_tx.len == 0);
        printf("ok 14: a refused RULES/READY is retried, never a completed handshake\n");
    }

    net.tick_frame = 300;
    load_fresh(sess_a, 1);
    net.tick_frame = 300 + HS_LEAD_FRAMES;
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, sizeof rules_a);
    assert(net.hs == HS_FAILED);
    assert(s_out_len == -1);
    assert(!s_unlock_saved);

    printf("test_net_handshake: all checks passed\n");
    return 0;
}
