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
 * handshake. From the repo root:
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
#define assert(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct NetSession net;

/* ---- stubbed dependencies --------------------------------------------- */

static uint64_t s_now = 1;
static uint8_t s_out[REL_MAX];          /* last reliable payload queued */
static int s_out_len = -1;              /* -1: nothing queued since the last clear */
static uint8_t s_out_type;
static char s_log[64][192];
static int s_logn;

static GameRules s_game;
static struct GamePrefs s_prefs;
static u32 s_seed_store;
u32* HSD_RandSeedPtr = &s_seed_store;

Uint64 SDL_GetTicksNS(void) { return s_now; }
void recv_inputs(void) {}
GameRules* gmMainLib_GetGameRules(void) { return &s_game; }
struct GamePrefs* gmMainLib_GetGamePrefs(void) { return &s_prefs; }
bool pc_is_frozen_stadium_enabled(void) { return true; }
int pc_net_recv_reliable(uint8_t* t, void* p, int max) { (void) t; (void) p; (void) max; return -1; }

bool pc_net_send_reliable(uint8_t type, const void* payload, int len) {
    s_out_type = type;
    s_out_len = len;
    if (len > 0) {
        memcpy(s_out, payload, (size_t) len);
    }
    return true;
}

void pc_log_line(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s_logn < (int) (sizeof s_log / sizeof s_log[0])) {
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
}

/* Load a side that has just connected to session `id` and learned nothing:
 * HS_IDLE, no nonce, no rules. `local` 1 is the guest, 0 the host. */
static void load_fresh(uint32_t id, int local) {
    Side s = { 0 };
    s.hs = HS_IDLE;
    s.session = id;
    s.start_frame = -1;
    s.local = local;
    s.remote = 1 - local;
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

static void forge_ready(uint8_t* out, uint32_t session, uint64_t nonce, uint64_t echo) {
    Ready rd = { nonce, echo, 0 };
    rd.hash = ready_hash(rd, session);
    wire_ready(&rd);
    memcpy(out, &rd, sizeof rd);
}

int main(void) {
    const uint32_t sess_a = 0xa1b2c3d4, sess_b = 0x0badf00d, sess_c = 0x77c0ffee;
    uint8_t rules_a[sizeof(Rules)], ready_a[sizeof(Ready)];
    uint64_t host_nonce, guest_nonce;
    int32_t sf = -1;
    Side host, guest;

    /* every check below depends on these sizes being the wire ones */
    assert(sizeof(Rules) == 16 + sizeof(GameRules) + 18 && sizeof(Ready) == 20);

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
    load_fresh(sess_a, 0);
    assert(!pc_net_host_match(1234, &sf));  /* queued RULES, now waiting for READY */
    assert(net.hs == HS_PENDING);
    assert(s_out_type == REL_RULES && s_out_len == (int) sizeof(Rules));
    memcpy(rules_a, s_out, sizeof rules_a);
    host_nonce = s_nonce_local;
    assert(host_nonce != 0);
    side_save(&host);

    load_fresh(sess_a, 1);
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, (int) sizeof rules_a);
    assert(net.hs == HS_DONE);
    assert(net.seed == 1234 && net.start_frame == 300 + HS_LEAD_FRAMES);
    assert(s_out_type == REL_READY && s_out_len == (int) sizeof(Ready));
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
    }
    side_save(&guest);

    side_load(&host);
    handshake_msg(REL_READY, ready_a, (int) sizeof ready_a);
    assert(net.hs == HS_DONE && s_nonce_peer == guest_nonce);
    side_save(&host);
    printf("ok 1: exchange completes, host %016llx / guest %016llx bound both ways\n",
           (unsigned long long) host_nonce, (unsigned long long) guest_nonce);

    /* ---- 2. that RULES replayed into the next session is refused ------- */
    load_fresh(sess_b, 1);
    s_logn = 0;
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, (int) sizeof rules_a);
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
        handshake_msg(REL_RULES, rules_b, (int) sizeof rules_b);
        assert(net.hs == HS_DONE && net.seed == 777 && net.start_frame == 310);
        assert(s_out_type == REL_READY && s_out_len == (int) sizeof(Ready));
        assert(s_nonce_peer == 0x0123456789abcdefull);
        printf("ok 3: same rules, this session's binding, fresh nonce -> accepted\n");

        /* ---- 3. a second, conflicting RULES after done is dropped once -- */
        uint8_t rules_c[sizeof(Rules)];
        forge_rules(rules_c, rules_a, sess_b, 0xfedcba9876543210ull, 4321, 500);
        s_logn = 0;
        s_out_len = -1;
        handshake_msg(REL_RULES, rules_c, (int) sizeof rules_c);
        handshake_msg(REL_RULES, rules_c, (int) sizeof rules_c);
        handshake_msg(REL_RULES, rules_c, (int) sizeof rules_c);
        assert(net.hs == HS_DONE && net.seed == 777 && net.start_frame == 310);
        assert(s_nonce_peer == 0x0123456789abcdefull && s_out_len == -1);
        assert(log_count("net: RULES ignored (conflicting nonce after done)") == 1);
        assert(s_logn == 1);
        printf("ok 4: second RULES after done dropped, logged once (3 arrivals, 1 line)\n");

        /* the accepted set arriving again is the benign duplicate class */
        s_logn = 0;
        handshake_msg(REL_RULES, rules_b, (int) sizeof rules_b);
        assert(net.hs == HS_DONE && net.seed == 777);
        assert(log_count("net: RULES ignored (already applied)") == 1 && s_logn == 1);
        printf("ok 5: re-arrival of the applied RULES logged as a duplicate, not a conflict\n");

        /* ---- 4. a tampered hash is refused ------------------------------ */
        uint8_t bad[sizeof(Rules)];
        memcpy(bad, rules_b, sizeof bad);
        bad[sizeof bad - 1] ^= 1u;      /* last byte of .hash */
        load_fresh(sess_b, 1);
        s_logn = 0;
        s_out_len = -1;
        handshake_msg(REL_RULES, bad, (int) sizeof bad);
        assert(net.hs == HS_FAILED && net.seed == 0 && s_out_len == -1);
        assert(log_count("net: RULES rejected: hash mismatch") == 1);

        /* the same, tampering a rule value instead of the hash */
        memcpy(bad, rules_b, sizeof bad);
        bad[offsetof(Rules, nonce)] ^= 0x80u;
        load_fresh(sess_b, 1);
        s_logn = 0;
        handshake_msg(REL_RULES, bad, (int) sizeof bad);
        assert(net.hs == HS_FAILED && log_count("net: RULES rejected: hash mismatch") == 1);

        /* a zero nonce never validates: an old peer or a stripped field */
        forge_rules(bad, rules_a, sess_b, 0, 777, 310);
        load_fresh(sess_b, 1);
        s_logn = 0;
        handshake_msg(REL_RULES, bad, (int) sizeof bad);
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
        assert(hn != 0 && hn != host_nonce);  /* a new session redraws */
        s_logn = 0;

        /* right hash for this session, wrong echo: an off-path forgery */
        forge_ready(k, sess_c, 0x1111111111111111ull, hn ^ 1u);
        handshake_msg(REL_READY, k, (int) sizeof k);
        assert(net.hs == HS_PENDING && s_nonce_peer == 0);
        assert(log_count("net: READY ignored (echoed nonce mismatch)") == 1);

        /* session 1's captured READY: right echo for *that* host nonce, but
         * it hashes under sess_a, so it dies before the nonce check */
        handshake_msg(REL_READY, ready_a, (int) sizeof ready_a);
        assert(net.hs == HS_PENDING && s_nonce_peer == 0);
        assert(log_count("net: READY ignored (hash mismatch)") == 1);

        /* and repeats of both stay at one line per class */
        handshake_msg(REL_READY, k, (int) sizeof k);
        handshake_msg(REL_READY, ready_a, (int) sizeof ready_a);
        assert(s_logn == 2);

        /* the genuine one still completes the handshake afterwards */
        forge_ready(k, sess_c, 0x2222222222222222ull, hn);
        handshake_msg(REL_READY, k, (int) sizeof k);
        assert(net.hs == HS_DONE && s_nonce_peer == 0x2222222222222222ull);
        printf("ok 7: forged and replayed READY dropped (handshake stays PENDING), real one completes\n");

        /* a further READY after done is dropped and logged once */
        s_logn = 0;
        handshake_msg(REL_READY, k, (int) sizeof k);
        handshake_msg(REL_READY, k, (int) sizeof k);
        assert(net.hs == HS_DONE);
        assert(log_count("net: READY ignored (already done)") == 1 && s_logn == 1);

        printf("ok 8: READY after done dropped and logged once\n");
    }

    /* a short RULES is refused on its length, before any field is read */
    load_fresh(sess_b, 1);
    s_logn = 0;
    s_out_len = -1;
    handshake_msg(REL_RULES, rules_a, (int) sizeof rules_a - 1);
    assert(net.hs == HS_IDLE && net.seed == 0 && s_out_len == -1);
    assert(log_count("net: RULES ignored (wrong length)") == 1 && s_logn == 1);
    printf("ok 9: short RULES refused on length, handshake untouched\n");
    {
        /* and the host never takes a RULES at all */
        load_fresh(sess_b, 0);
        net.hs_host = true;
        s_logn = 0;
        handshake_msg(REL_RULES, rules_a, (int) sizeof rules_a);
        assert(net.hs == HS_IDLE && log_count("net: RULES ignored (we host)") == 1);
        printf("ok 10: RULES arriving at the host refused\n");
    }

    printf("test_net_handshake: all checks passed\n");
    return 0;
}
