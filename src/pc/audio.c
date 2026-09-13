/*
 * Software AX: the GameCube audio voice mixer, on an SDL3 audio stream.
 *
 * Melee's sound engine (sysdolphin synth.c / axdriver.c) drives AX voices:
 * DSP-ADPCM (or PCM) samples in ARAM, a 16.16 sample-rate-conversion ratio,
 * a per-sample volume envelope and L/R mix, plus a 5ms frame callback that
 * the engine uses to update envelopes and reap finished voices. All of that
 * is reproduced here at AX's native 32kHz in 160-sample frames, rendered on
 * demand from SDL's pull callback.
 *
 * Endianness: AXSetVoiceAddr/Adpcm/AdpcmLoop receive parameter blocks copied
 * verbatim from disc (.ssm/.hps headers) and are big-endian; every other
 * setter takes host-native values. The mirror `AXVPB.pb` the engine reads
 * back (state, currentAddress) is kept host-native.
 *
 * ponytail: no aux (reverb/chorus) busses and no ITD; dry stereo only.
 */
#include <dolphin/ai.h>
#include "pc/pc.h"
#include <dolphin/ar.h>
#include <dolphin/ax.h>
#include <dolphin/axfx.h>
#include <dolphin/os.h>

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cached once: getenv() scans the whole environment, and these guards sit
 * on per-draw / per-voice paths where that cost is not acceptable even
 * when the diagnostic is switched off. */
static int pc_dbg_audio_stats(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("MELEE_AUDIO_STATS") != NULL;
    }
    return cached;
}


#define AX_VOICES 64
#define AX_RATE 32000
#define AX_FRAME 160 /* 5ms */

#define AX_FORMAT_ADPCM 0
#define AX_FORMAT_PCM16 10
#define AX_FORMAT_PCM8 25

typedef struct Voice {
    AXVPB vpb; /* handed to the game; vpb.pb is our native mirror */
    bool used;
    s16 prev, cur;   /* last two decoded samples, for interpolation */
    u32 frac;        /* 16.16 position between prev and cur */
    u16 pred_scale;  /* ADPCM header of the current frame */
    s32 yn1, yn2;    /* ADPCM history */
    u32 loop_addr, end_addr, cur_addr;
    u32 age; /* AX frames this voice has been running; MELEE_AUDIO_STATS */
} Voice;

static Voice s_voices[AX_VOICES];
static void (*s_frame_callback)(void);
static SDL_AudioStream* s_stream;
static float s_master_volume = 1.0f;
void pc_audio_set_volume(float volume)
{
    s_master_volume = volume;
    if (s_stream) SDL_SetAudioStreamGain(s_stream, volume);
}
static u8* s_aram;
static float s_master = 1.0f;

static inline u16 be16(u16 v)
{
    return __builtin_bswap16(v);
}

static inline u32 addr32(u16 hi, u16 lo)
{
    return ((u32) hi << 16) | lo;
}

static inline void set_addr(u16* hi, u16* lo, u32 addr)
{
    *hi = (u16) (addr >> 16);
    *lo = (u16) addr;
}

static inline s16 clamp16(s64 v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16) v;
}

/* ---- sample fetch ------------------------------------------------------ */

/* Reads the next source sample; returns false once the voice has ended. */
static bool next_sample(Voice* v, s16* out)
{
    AXPB* pb = &v->vpb.pb;
    u16 format = pb->addr.format;

    if (!pb->state) {
        return false;
    }

    /* Every read below indexes the 16MB ARAM buffer directly. A voice whose
     * address pair is wrong -- a torn AXSetVoiceAddr from the game thread, a
     * bank whose ARAM allocation was freed, a .ssm header read with the wrong
     * relocation -- would otherwise read up to 2GB past the buffer. Ending
     * the voice is what the mixer already does for a finished one, so the
     * voice is reaped instead of faulting. */
    switch (format) {
    case AX_FORMAT_ADPCM: {
        /* 16 nibbles per frame: 2 header nibbles, 14 sample nibbles. */
        if ((v->cur_addr & 15) == 0) {
            if ((v->cur_addr >> 1) + 2 > PC_ARAM_SIZE) {
                return false;
            }
            v->pred_scale = s_aram[v->cur_addr >> 1];
            v->cur_addr += 2;
        }
        if ((v->cur_addr >> 1) >= PC_ARAM_SIZE) {
            return false;
        }
        u8 byte = s_aram[v->cur_addr >> 1];
        s32 nibble = (v->cur_addr & 1) ? (byte & 0xF) : (byte >> 4);
        nibble = nibble >= 8 ? nibble - 16 : nibble;
        s32 scale = 1 << (v->pred_scale & 0xF);
        u32 coef = (v->pred_scale >> 4) & 7;
        s32 c0 = (s16) pb->adpcm.a[coef][0];
        s32 c1 = (s16) pb->adpcm.a[coef][1];
        /* s64 accumulator: a bank read with the wrong relocation yields
         * coefficients and a scale whose four terms overshoot s32, and
         * signed overflow is undefined behaviour that -O2 may fold the
         * clamp away against. */
        s64 acc = (s64) nibble * scale * 2048 + 1024 +
                  (s64) c0 * v->yn1 + (s64) c1 * v->yn2;
        s32 sample = clamp16(acc >> 11);
        v->yn2 = v->yn1;
        v->yn1 = sample;
        *out = (s16) sample;
        break;
    }
    case AX_FORMAT_PCM16: {
        const u8* p;
        if (v->cur_addr >= PC_ARAM_SIZE / 2) {
            return false;
        }
        p = &s_aram[v->cur_addr * 2];
        *out = (s16) ((p[0] << 8) | p[1]);
        break;
    }
    case AX_FORMAT_PCM8:
        if (v->cur_addr >= PC_ARAM_SIZE) {
            return false;
        }
        *out = (s16) ((s8) s_aram[v->cur_addr] * 256);
        break;
    default:
        return false;
    }

    /* AX end addresses are inclusive. HPS jumps into the next ring block
     * here, then updates endAddress at the following synth callback. That
     * loop target can be ABOVE the old end: equality after decoding (not a
     * range check before it) lets the new block advance in the meantime. */
    if (v->cur_addr == v->end_addr) {
        if (pb->addr.loopFlag) {
            v->cur_addr = v->loop_addr;
            if (format == AX_FORMAT_ADPCM) {
                v->pred_scale = pb->adpcmLoop.loop_pred_scale;
                v->yn1 = (s16) pb->adpcmLoop.loop_yn1;
                v->yn2 = (s16) pb->adpcmLoop.loop_yn2;
            }
        } else {
            pb->state = 0;
        }
    } else {
        v->cur_addr++;
    }
    return true;
}

/* ---- aux busses -------------------------------------------------------- */

/* AX has two auxiliary send busses. Voices send into them with the vAuxA and
 * vAuxB mix levels, the registered AXFX callback processes the bus in place,
 * and the result is summed back into the dry mix. Melee puts stage reverb on
 * aux A. AX aux buffers are 32-bit and carry L, R and surround. */
typedef struct AuxBus {
    void (*cb)(void*, void*);
    void* ctx;
    /* `long`, not s32: AXFX_BUFFERUPDATE types its channels `long*`, which is
     * 64-bit on LP64. A 32-bit buffer here is overrun by the effect. */
    long ch[3][AX_FRAME];
} AuxBus;

static AuxBus s_auxA, s_auxB;

static void mix_voice(Voice* v, float* out)
{
    AXPB* pb = &v->vpb.pb;
    u32 ratio = addr32(pb->src.ratioHi, pb->src.ratioLo);
    s32 vol = pb->ve.currentVolume;
    s32 delta = pb->ve.currentDelta;
    float vl = pb->mix.vL / 32767.0f;
    float vr = pb->mix.vR / 32767.0f;
    float al = pb->mix.vAuxAL / 32767.0f;
    float ar = pb->mix.vAuxAR / 32767.0f;
    float bl = pb->mix.vAuxBL / 32767.0f;
    float br = pb->mix.vAuxBR / 32767.0f;
    bool send_a = (s_auxA.cb != NULL) && (al != 0.0f || ar != 0.0f);
    bool send_b = (s_auxB.cb != NULL) && (bl != 0.0f || br != 0.0f);

    for (int i = 0; i < AX_FRAME; i++) {
        v->frac += ratio;
        while (v->frac >= 0x10000) {
            s16 s;
            if (!next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16) (vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                /* The mirror the game reads back has to follow on this path
                 * too: stopRange() matches a voice against the bank being
                 * unloaded by pb.addr.currentAddress, and the HPS block swap
                 * derives the stream position from it. Without this the
                 * field stayed at the previous frame's value, reporting the
                 * voice up to one frame short of where it really stopped. */
                set_addr(&pb->addr.currentAddressHi,
                         &pb->addr.currentAddressLo, v->cur_addr);
                /* frac is deliberately left alone. It holds one whole
                 * unsatisfied sample step plus the sub-sample phase, and
                 * nothing at end-of-voice should discard that: on hardware
                 * the DSP's SRC phase survives an end too, and the reset
                 * arrives only with an explicit SRC block (AXSetVoiceSrc,
                 * which the HPS block swap does supply and which sets frac
                 * from currentAddressFrac). A voice restarted with only
                 * AXSetVoiceCurrentAddr therefore keeps its phase, and the
                 * retained whole step is what consumes the first sample of
                 * the new block instead of skipping it. */
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            v->frac -= 0x10000;
        }
        float t = (float) v->frac * (1.0f / 65536.0f);
        float s = ((float) v->prev + t * (float) (v->cur - v->prev)) * (1.0f / 32768.0f);
        float g = (float) vol * (1.0f / 32767.0f);
        float sv = s * g;
        out[i * 2] += sv * vl;
        out[i * 2 + 1] += sv * vr;
        if (send_a) {
            s_auxA.ch[0][i] += (long) (sv * al * 32767.0f);
            s_auxA.ch[1][i] += (long) (sv * ar * 32767.0f);
        }
        if (send_b) {
            s_auxB.ch[0][i] += (long) (sv * bl * 32767.0f);
            s_auxB.ch[1][i] += (long) (sv * br * 32767.0f);
        }
        vol += delta;
        if (vol < 0) {
            vol = 0;
        } else if (vol > 32767) {
            vol = 32767;
        }
    }
    pb->ve.currentVolume = (u16) vol;
    set_addr(&pb->addr.currentAddressHi, &pb->addr.currentAddressLo, v->cur_addr);
}

static void run_aux(AuxBus* bus, float* out)
{
    struct AXFX_BUFFERUPDATE bu;

    if (bus->cb == NULL) {
        return;
    }
    bu.left = bus->ch[0];
    bu.right = bus->ch[1];
    bu.surround = bus->ch[2];
    bus->cb(&bu, bus->ctx);
    for (int i = 0; i < AX_FRAME; i++) {
        float sur = (float) bus->ch[2][i] * (0.5f / 32767.0f);
        out[i * 2] += (float) bus->ch[0][i] * (1.0f / 32767.0f) + sur;
        out[i * 2 + 1] += (float) bus->ch[1][i] * (1.0f / 32767.0f) + sur;
    }
}

static void render_frame(float* out)
{
    memset(out, 0, sizeof(float) * AX_FRAME * 2);
    memset(s_auxA.ch, 0, sizeof(s_auxA.ch));
    memset(s_auxB.ch, 0, sizeof(s_auxB.ch));
    BOOL intr = OSDisableInterrupts();
    if (s_frame_callback) {
        s_frame_callback();
    }
    int n_used = 0, n_running = 0, n_stopped = 0, n_zero_mix = 0;
    int n_zero_ratio = 0, n_loop = 0, n_old = 0;
    for (int i = 0; i < AX_VOICES; i++) {
        Voice* v = &s_voices[i];
        if (!v->used) {
            v->age = 0;
            continue;
        }
        n_used++;
        /* Age every voice the pool is holding, running or not. A voice left
         * `used` with state 0 -- ended but never AXFreeVoice'd, or acquired
         * and never started -- is inaudible AND unavailable, which is
         * exactly what "sound works, then stops" looks like once 64 of them
         * accumulate. The previous code reset age whenever state != 1, so
         * the stuck-voice dump could only ever report the running variety
         * and was blind to the stopped one. */
        if (++v->age == 2001) { /* 10s of AX frames */
            if (pc_dbg_audio_stats()) {
                fprintf(stderr,
                        "stuck voice %u: state=%u fmt=%u loop=%u ratio=%u"
                        " cur=%u end=%u loopaddr=%u vol=%u prio=%d\n",
                        v->vpb.index, v->vpb.pb.state, v->vpb.pb.addr.format,
                        v->vpb.pb.addr.loopFlag,
                        addr32(v->vpb.pb.src.ratioHi, v->vpb.pb.src.ratioLo),
                        v->cur_addr, v->end_addr, v->loop_addr,
                        v->vpb.pb.ve.currentVolume, v->vpb.priority);
            }
        }
        if (v->age > 2000) {
            n_old++;
        }
        if (v->vpb.pb.state != 1) {
            n_stopped++;
            continue;
        }
        n_running++;
        if (v->vpb.pb.mix.vL == 0 && v->vpb.pb.mix.vR == 0) {
            n_zero_mix++;
        }
        if (addr32(v->vpb.pb.src.ratioHi, v->vpb.pb.src.ratioLo) == 0) {
            n_zero_ratio++;
        }
        if (v->vpb.pb.addr.loopFlag) {
            n_loop++;
        }
        mix_voice(v, out);
    }
    run_aux(&s_auxA, out);
    run_aux(&s_auxB, out);
    /* MELEE_AUDIO_STATS=1: voice census against the output clock, so a
     * silent stretch in MELEE_AUDIO_DUMP can be explained -- were there no
     * voices, were they all stopped, or were they running at zero volume?
     * `stopped` is the diagnostic for the pool leak: those slots are held
     * but inaudible, so `used` climbing to 64 with `running` low means the
     * game is not calling AXFreeVoice on voices the mixer already ended.
     * `old` counts voices held for over ten seconds, which for a one-shot
     * sound effect means it was never reaped. */
    if (pc_dbg_audio_stats()) {
        static unsigned long frames;
        frames++;
        if ((frames % 100) == 0) { /* every 100 * 5ms = 0.5s of output */
            fprintf(stderr,
                    "voices t=%.1fs used=%d running=%d stopped=%d"
                    " zero_mix=%d zero_ratio=%d loop=%d old=%d\n",
                    frames * (double) AX_FRAME / AX_RATE, n_used, n_running,
                    n_stopped, n_zero_mix, n_zero_ratio, n_loop, n_old);
        }
    }
    OSRestoreInterrupts(intr);
    /* AX sums into a 16-bit accumulator and saturates; do the same so a busy
     * scene distorts the way the hardware does instead of wrapping. */
    for (int i = 0; i < AX_FRAME * 2; i++) {
        float s = out[i] * s_master;
        out[i] = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
    }
}

/* MELEE_AUDIO_DUMP=<file>: also write the mix as raw f32 stereo 32kHz. */
static FILE* s_dump;

static void SDLCALL audio_pull(void* userdata, SDL_AudioStream* stream, int additional, int total)
{
    static float frame[AX_FRAME * 2];
    (void) userdata;
    (void) total;
    while (additional > 0) {
        render_frame(frame);
        SDL_PutAudioStreamData(stream, frame, sizeof(frame));
        if (s_dump) {
            fwrite(frame, sizeof(frame), 1, s_dump);
        }
        additional -= (int) sizeof(frame);
    }
}

/* ---- AX API ------------------------------------------------------------ */

void AXInit(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    for (int i = 0; i < AX_VOICES; i++) {
        s_voices[i].vpb.index = i;
    }
    s_aram = aurora_aram_base();
    if (s_dump == NULL && getenv("MELEE_AUDIO_DUMP") != NULL) {
        s_dump = fopen(getenv("MELEE_AUDIO_DUMP"), "wb");
    }
    if (s_stream == NULL) {
        const SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, AX_RATE };
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
            return;
        }
        s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_pull, NULL);
        if (s_stream) SDL_SetAudioStreamGain(s_stream, s_master_volume);
        if (s_stream == NULL) {
            fprintf(stderr, "audio: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(s_stream);
    }
}

void AXQuit(void)
{
    if (s_stream) {
        SDL_DestroyAudioStream(s_stream);
        s_stream = NULL;
    }
}

void AXRegisterCallback(void (*callback)())
{
    s_frame_callback = (void (*)(void)) callback;
}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    BOOL intr = OSDisableInterrupts();
    Voice* pick = NULL;
    for (int i = 0; i < AX_VOICES; i++) {
        if (!s_voices[i].used) {
            pick = &s_voices[i];
            break;
        }
    }
    if (pick == NULL) {
        /* Steal the lowest-priority voice below the request, like AX. */
        for (int i = 0; i < AX_VOICES; i++) {
            Voice* v = &s_voices[i];
            if ((u32) v->vpb.priority < priority && (pick == NULL || v->vpb.priority < pick->vpb.priority)) {
                pick = v;
            }
        }
        if (pick != NULL && pick->vpb.callback != NULL) {
            pick->vpb.callback(&pick->vpb);
        }
    }
    if (pick != NULL) {
        u32 index = pick->vpb.index;
        memset(pick, 0, sizeof(*pick));
        pick->used = true;
        pick->vpb.index = index;
        pick->vpb.priority = (int) priority;
        pick->vpb.callback = callback;
        pick->vpb.userContext = userContext;
    }
    OSRestoreInterrupts(intr);
    return pick ? &pick->vpb : NULL;
}

void AXFreeVoice(AXVPB* p)
{
    if (p) {
        BOOL intr = OSDisableInterrupts();
        Voice* v = (Voice*) p;
        v->used = false;
        v->vpb.pb.state = 0;
        v->vpb.priority = 0;
        OSRestoreInterrupts(intr);
    }
}

/* Every setter below brackets its body with OSDisableInterrupts, because
 * every one of the 21 AXSetVoice* functions in the real SDK does
 * (dolphin/src/dolphin/ax/AXVPB.c, vendored by melee-native: checked, all 21
 * take it) -- on hardware to keep the DSP update from reading a half-written
 * parameter block, and here for the same reason with render_frame() in the
 * DSP's place. src/pc/os.c:24-34 maps OSDisableInterrupts onto a recursive
 * PTHREAD_MUTEX, render_frame holds it for the whole frame on SDL's audio
 * thread, and the game calls these from its own thread and from aurora's DVD
 * worker threads. A multi-word AXSetVoiceAddr/Adpcm, or either half of a
 * set_addr() pair, torn against a concurrent mix_voice hands the voice a
 * cur/end/loop/ratio combination it can never satisfy -- and a voice that
 * never reaches its end address is never AXFreeVoice'd, so 64 of those and
 * no sound can start at all. The mutex is recursive, so the callers that are
 * already inside a bracket (the game's own, and devcom's around every
 * completion callback) just re-enter it. Single-field setters are bracketed
 * too: concurrent plain accesses are a data race whatever their width, and
 * pb.state is the field render_frame gates mixing on. */
void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    BOOL intr = OSDisableInterrupts();
    p->priority = (int) priority;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceState(AXVPB* p, u16 state)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.state = state;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.mix = *mix;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceItdOn(AXVPB* p)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.itd.flag = 1;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceItdTarget(AXVPB* p, u16 l, u16 r)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.itd.targetShiftL = l;
    p->pb.itd.targetShiftR = r;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.ve = *ve;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.ve.currentDelta = delta;
    OSRestoreInterrupts(intr);
}

/* Big-endian block from disc. */
void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
    BOOL intr = OSDisableInterrupts();
    Voice* v = (Voice*) p;
    AXPBADDR* dst = &p->pb.addr;
    dst->loopFlag = be16(addr->loopFlag);
    dst->format = be16(addr->format);
    v->loop_addr = addr32(be16(addr->loopAddressHi), be16(addr->loopAddressLo));
    v->end_addr = addr32(be16(addr->endAddressHi), be16(addr->endAddressLo));
    v->cur_addr = addr32(be16(addr->currentAddressHi), be16(addr->currentAddressLo));

    /* Every voice address is treated as an ARAM offset by next_sample(), which
     * indexes s_aram[cur_addr >> 1]. A voice whose samples are NOT in ARAM
     * would read outside that buffer and fall silent -- which is the shape of
     * "some sound effects don't play". Report any address past the end of
     * ARAM (nibble-addressed, so the limit is 2x the byte size). */
    {
        static int log_cached = -1;
        const u32 limit = (u32) (PC_ARAM_SIZE * 2u);

        if (log_cached < 0) {
            log_cached = getenv("MELEE_AUDIO_ADDR") != NULL;
        }
        if (v->end_addr > limit || v->cur_addr > limit) {
            static unsigned long bad;
            if (log_cached || ++bad <= 4) {
                fprintf(stderr,
                        "audio: voice addr outside ARAM: cur=%u end=%u"
                        " limit=%u\n",
                        v->cur_addr, v->end_addr, limit);
            }
        } else if (log_cached) {
            static unsigned long ok;
            if (++ok <= 4 || ok % 200 == 0) {
                fprintf(stderr, "audio: voice addr ok: cur=%u end=%u (%lu)\n",
                        v->cur_addr, v->end_addr, ok);
            }
        }
    }
    set_addr(&dst->loopAddressHi, &dst->loopAddressLo, v->loop_addr);
    set_addr(&dst->endAddressHi, &dst->endAddressLo, v->end_addr);
    set_addr(&dst->currentAddressHi, &dst->currentAddressLo, v->cur_addr);
    v->frac = 0;
    v->prev = v->cur = 0;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceLoop(AXVPB* p, u16 loop)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.addr.loopFlag = loop;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr)
{
    BOOL intr = OSDisableInterrupts();
    Voice* v = (Voice*) p;
    v->loop_addr = addr;
    set_addr(&p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo, addr);
    OSRestoreInterrupts(intr);
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr)
{
    BOOL intr = OSDisableInterrupts();
    Voice* v = (Voice*) p;
    v->end_addr = addr;
    set_addr(&p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo, addr);
    OSRestoreInterrupts(intr);
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr)
{
    BOOL intr = OSDisableInterrupts();
    Voice* v = (Voice*) p;
    v->cur_addr = addr;
    set_addr(&p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo, addr);
    OSRestoreInterrupts(intr);
}

/* Big-endian block from disc. */
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* a)
{
    BOOL intr = OSDisableInterrupts();
    Voice* v = (Voice*) p;
    AXPBADPCM* dst = &p->pb.adpcm;
    for (int i = 0; i < 8; i++) {
        dst->a[i][0] = be16(a->a[i][0]);
        dst->a[i][1] = be16(a->a[i][1]);
    }
    dst->gain = be16(a->gain);
    dst->pred_scale = be16(a->pred_scale);
    dst->yn1 = be16(a->yn1);
    dst->yn2 = be16(a->yn2);
    v->pred_scale = dst->pred_scale;
    v->yn1 = (s16) dst->yn1;
    v->yn2 = (s16) dst->yn2;
    OSRestoreInterrupts(intr);
}

/* Big-endian block from disc. */
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* l)
{
    BOOL intr = OSDisableInterrupts();
    p->pb.adpcmLoop.loop_pred_scale = be16(l->loop_pred_scale);
    p->pb.adpcmLoop.loop_yn1 = be16(l->loop_yn1);
    p->pb.adpcmLoop.loop_yn2 = be16(l->loop_yn2);
    OSRestoreInterrupts(intr);
}

/* AX_MAX_RATIO is not a guess: retail AXSetVoiceSrcRatio does exactly this,
 *     r = 65536.0f * ratio;
 *     if (r > 0x40000) { r = 0x40000; }
 * (dolphin/src/dolphin/ax/AXVPB.c:1240-1243 in the SDK tree vendored by
 * melee-native). 4.0 is the ceiling of AX's 4-tap sample-rate converter, so
 * a ratio above it is not a pitch the hardware would have played either --
 * saturating is the faithful behaviour, not a new policy.
 *
 * It also has to be bounded for a reason retail did not have: mix_voice
 * decodes ratio/65536 source samples per output sample in software, so at
 * the nominal-1.0-byte-swapped value of 65535.0 one 5ms frame costs 58ms of
 * CPU (measured: 20 frames = 100ms of audio took 1160ms). The SDL pull
 * callback then never refills the stream again and ALL audio stops for the
 * rest of the session while the game runs on. The cap additionally keeps
 * `v->frac += ratio` from wrapping u32, which would silently slow a voice so
 * it never reached its end address.
 *
 * Retail clamps only in AXSetVoiceSrcRatio, because the DSP does fixed work
 * regardless of what a raw AXSetVoiceSrc block contains. Both paths are
 * clamped here since the software mixer does not have that luxury; it is
 * reported so the difference is never silent. */
#define AX_MAX_RATIO 0x40000u /* 4.0 in 16.16 */

static u32 clamp_ratio(u32 ratio)
{
    if (ratio > AX_MAX_RATIO) {
        static unsigned long capped;
        if (++capped <= 8 || pc_dbg_audio_stats()) {
            fprintf(stderr,
                    "audio: SRC ratio %u (%.2fx) capped to AX's 4.0 (#%lu)\n",
                    ratio, ratio / 65536.0, capped);
        }
        return AX_MAX_RATIO;
    }
    return ratio;
}

/* The 16.16 ratio is held in the block's two halves in field order
 * (ratioHi = high 16 bits), not as one host u32: reading it with a memcpy
 * gives (Lo << 16) | Hi, which for the synth's nominal 1.0 is a ratio of 1 --
 * one source sample every 65536 output samples, so the voice is inaudible and
 * never reaches its end address, and the voice pool fills with stuck voices
 * until no sound effect can be started at all. */
void AXSetVoiceSrc(AXVPB* p, AXPBSRC* s)
{
    BOOL intr = OSDisableInterrupts();
    Voice* v = (Voice*) p;
    int i;

    set_addr(&p->pb.src.ratioHi, &p->pb.src.ratioLo,
             clamp_ratio(addr32(s->ratioHi, s->ratioLo)));
    /* Retail copies all seven u16s of the block and raises
     * AX_SYNC_FLAG_COPYSRC, so the DSP adopts the supplied SRC phase and FIR
     * history as well as the ratio (AXVPB.c:1197-1232). currentAddressFrac
     * is the sub-sample position -- the same unit as the low half of
     * v->frac -- so it is applied, not ignored. That also means an
     * AXSetVoiceSrc resets the resampling phase exactly as it does on
     * hardware, which is why mix_voice's end-of-voice path may preserve
     * frac: nothing resets it there, the reset arrives with the SRC block
     * the HPS swap supplies. last_samples is the 4-tap SRC history, which a
     * 2-point linear interpolator has no equivalent for, so it is mirrored
     * for the game to read back but not consumed. Melee supplies zeros for
     * both anyway: synth.c:529 is
     * `HSD_Synth_80407FD8 = { 1, 0, 0, { 0, 0, 0, 0 } }` and setSrcRatio
     * (synth.c:533-537) only ever writes the ratio pair. */
    p->pb.src.currentAddressFrac = s->currentAddressFrac;
    for (i = 0; i < 4; i++) {
        p->pb.src.last_samples[i] = s->last_samples[i];
    }
    v->frac = s->currentAddressFrac;
    OSRestoreInterrupts(intr);
}

void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{
    BOOL intr = OSDisableInterrupts();
    /* Both ends have to be handled in float, before the cast: converting a
     * NaN, a negative, or anything >= 65536.0 to u32 is undefined behaviour
     * (x86-64 cvttss2si yields 0x80000000, i.e. a 32768x ratio). Melee's
     * pause path legitimately asks for 0, so the bottom is clamped to 0 and
     * only clamp_ratio caps -- and reports -- the top.  */
    if (!(isfinite(ratio) && ratio > 0.0f)) {
        ratio = 0.0f;
    } else if (ratio > 65535.0f) {
        ratio = 65535.0f;
    }
    u32 fixed = clamp_ratio((u32) (ratio * 65536.0f));
    set_addr(&p->pb.src.ratioHi, &p->pb.src.ratioLo, fixed);
    OSRestoreInterrupts(intr);
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{
    BOOL intr = OSDisableInterrupts();
    s_auxA.cb = callback;
    s_auxA.ctx = context;
    OSRestoreInterrupts(intr);
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{
    BOOL intr = OSDisableInterrupts();
    s_auxB.cb = callback;
    s_auxB.ctx = context;
    OSRestoreInterrupts(intr);
}

/* ---- AXFX -------------------------------------------------------------- */

void* (*__AXFXAlloc)(unsigned long) = NULL;
void (*__AXFXFree)(void*) = NULL;

void* AXFXAllocFunction(unsigned long size)
{
    return __AXFXAlloc != NULL ? __AXFXAlloc(size) : calloc(1, size);
}

void AXFXFreeFunction(void* ptr)
{
    if (__AXFXFree != NULL) {
        __AXFXFree(ptr);
    } else {
        free(ptr);
    }
}

void AXFXSetHooks(void* (*alloc_hook)(unsigned long), void (*free_hook)(void*))
{
    __AXFXAlloc = alloc_hook;
    __AXFXFree = free_hook;
}

/* A Schroeder reverberator over the work area the SDK structure already
 * describes: three comb filters with a damping one-pole in the feedback,
 * followed by three all-pass sections, per channel. The SDK's own network is
 * larger, but the parameters Melee sets (coloration, mix, time, damping,
 * pre-delay, crosstalk) map onto this one directly and the audible result is
 * stage reverb rather than silence.
 * ponytail: 3+3 sections per channel, not the SDK's 9; raise if a stage
 * sounds obviously thin. */
#define AXFX_CHANNELS 3

static const long kCombLen[AXFX_CHANNELS][3] = {
    { 1789, 1999, 2333 },
    { 1847, 2063, 2399 },
    { 1693, 1931, 2267 },
};
static const long kAllPassLen[AXFX_CHANNELS][3] = {
    { 433, 149, 53 },
    { 449, 157, 59 },
    { 419, 139, 47 },
};

/* The delay lines come from the host heap, not AXFXAllocFunction: the game's
 * AXFX heap is a fixed ~GameCube-sized scratch block (AXDriverAlloc asserts
 * against axfxmaxsize) and these lines are far larger than it. */
static bool axfx_line_alloc(struct AXFX_REVHI_DELAYLINE* d, long length)
{
    d->inputs = calloc((size_t) length, sizeof(float));
    if (d->inputs == NULL) {
        return false;
    }
    d->length = length;
    d->inPoint = 0;
    d->outPoint = 0;
    d->lastOutput = 0.0f;
    return true;
}

static void axfx_line_free(struct AXFX_REVHI_DELAYLINE* d)
{
    if (d->inputs != NULL) {
        free(d->inputs);
        d->inputs = NULL;
    }
    d->length = 0;
}

static float axfx_line_step(struct AXFX_REVHI_DELAYLINE* d, float in)
{
    float out = d->inputs[d->outPoint];
    d->inputs[d->inPoint] = in;
    if (++d->inPoint >= d->length) {
        d->inPoint = 0;
    }
    if (++d->outPoint >= d->length) {
        d->outPoint = 0;
    }
    d->lastOutput = out;
    return out;
}

static int axfx_reverb_init(struct AXFX_REVHI_WORK* rv, float coloration,
                            float mix, float time, float damping,
                            float crosstalk)
{
    int ch, k;

    memset(rv, 0, sizeof(*rv));
    for (ch = 0; ch < AXFX_CHANNELS; ch++) {
        for (k = 0; k < 3; k++) {
            if (!axfx_line_alloc(&rv->C[ch * 3 + k], kCombLen[ch][k]) ||
                !axfx_line_alloc(&rv->AP[ch * 3 + k], kAllPassLen[ch][k]))
            {
                return 0;
            }
        }
    }
    if (time < 0.01f) {
        time = 0.01f;
    }
    for (ch = 0; ch < AXFX_CHANNELS; ch++) {
        for (k = 0; k < 3; k++) {
            /* -60 dB after `time` seconds for a line of this length. */
            rv->combCoef[ch * 3 + k] = powf(
                10.0f, -3.0f * (float) kCombLen[ch][k] / (time * AX_RATE));
        }
    }
    rv->allPassCoeff = coloration;
    rv->level = mix;
    rv->damping = damping;
    rv->crosstalk = crosstalk;
    return 1;
}

static void axfx_reverb_shutdown(struct AXFX_REVHI_WORK* rv)
{
    int i;
    for (i = 0; i < 9; i++) {
        axfx_line_free(&rv->C[i]);
        axfx_line_free(&rv->AP[i]);
    }
}

static void axfx_reverb_run(struct AXFX_REVHI_WORK* rv,
                            struct AXFX_BUFFERUPDATE* b)
{
    long* chan[AXFX_CHANNELS];
    int ch, k, i;
    float damp = rv->damping;
    float ap = rv->allPassCoeff;
    float wet = rv->level;

    chan[0] = b->left;
    chan[1] = b->right;
    chan[2] = b->surround;

    if (damp < 0.0f) {
        damp = 0.0f;
    } else if (damp > 0.95f) {
        damp = 0.95f;
    }

    for (i = 0; i < AX_FRAME; i++) {
        float in[AXFX_CHANNELS];

        for (ch = 0; ch < AXFX_CHANNELS; ch++) {
            in[ch] = (float) chan[ch][i];
        }
        /* Crosstalk bleeds each side into the other before the network. */
        if (rv->crosstalk > 0.0f) {
            float c = rv->crosstalk;
            float l = in[0] + c * in[1];
            float r = in[1] + c * in[0];
            in[0] = l;
            in[1] = r;
        }
        for (ch = 0; ch < AXFX_CHANNELS; ch++) {
            float acc = 0.0f;
            float y;

            for (k = 0; k < 3; k++) {
                struct AXFX_REVHI_DELAYLINE* c = &rv->C[ch * 3 + k];
                float out = c->inputs[c->outPoint];
                float lp = rv->lpLastout[ch] =
                    out * (1.0f - damp) + rv->lpLastout[ch] * damp;
                axfx_line_step(c, in[ch] + lp * rv->combCoef[ch * 3 + k]);
                acc += out;
            }
            y = acc * (1.0f / 3.0f);
            for (k = 0; k < 3; k++) {
                struct AXFX_REVHI_DELAYLINE* a = &rv->AP[ch * 3 + k];
                float out = a->inputs[a->outPoint];
                float v = y + ap * out;
                axfx_line_step(a, v);
                y = out - ap * v;
            }
            chan[ch][i] = (long) (y * wet);
        }
    }
}

int AXFXReverbHiInit(struct AXFX_REVERBHI* rev)
{
    return axfx_reverb_init(&rev->rv, rev->coloration, rev->mix, rev->time,
                            rev->damping, rev->crosstalk);
}

int AXFXReverbHiShutdown(struct AXFX_REVERBHI* rev)
{
    axfx_reverb_shutdown(&rev->rv);
    return 1;
}

int AXFXReverbHiSettings(struct AXFX_REVERBHI* rev)
{
    AXFXReverbHiShutdown(rev);
    return AXFXReverbHiInit(rev);
}

void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE* b,
                          struct AXFX_REVERBHI* r)
{
    if (!r->tempDisableFX) {
        axfx_reverb_run(&r->rv, b);
    }
}

/* REVERBSTD keeps its own (smaller) work area; drive the same network from a
 * side allocation so both aux slots behave. */
static struct AXFX_REVHI_WORK s_revstd_work[2];
static struct AXFX_REVERBSTD* s_revstd_owner[2];

static struct AXFX_REVHI_WORK* revstd_work(struct AXFX_REVERBSTD* rev,
                                           bool claim)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (s_revstd_owner[i] == rev) {
            return &s_revstd_work[i];
        }
    }
    if (!claim) {
        return NULL;
    }
    for (i = 0; i < 2; i++) {
        if (s_revstd_owner[i] == NULL) {
            s_revstd_owner[i] = rev;
            return &s_revstd_work[i];
        }
    }
    return NULL;
}

int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev)
{
    struct AXFX_REVHI_WORK* w = revstd_work(rev, true);
    if (w == NULL) {
        return 0;
    }
    return axfx_reverb_init(w, rev->coloration, rev->mix, rev->time,
                            rev->damping, 0.0f);
}

int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev)
{
    struct AXFX_REVHI_WORK* w = revstd_work(rev, false);
    int i;
    if (w != NULL) {
        axfx_reverb_shutdown(w);
        for (i = 0; i < 2; i++) {
            if (s_revstd_owner[i] == rev) {
                s_revstd_owner[i] = NULL;
            }
        }
    }
    return 1;
}

int AXFXReverbStdSettings(struct AXFX_REVERBSTD* rev)
{
    AXFXReverbStdShutdown(rev);
    return AXFXReverbStdInit(rev);
}

void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* b,
                           struct AXFX_REVERBSTD* r)
{
    struct AXFX_REVHI_WORK* w = revstd_work(r, false);
    if (w != NULL && !r->tempDisableFX) {
        axfx_reverb_run(w, b);
    }
}

/* Chorus is not selected by Melee (lbaudio_ax.c puts REVERB_STD on aux A and
 * DELAY on aux B); leaving the bus untouched passes the send through dry.
 * ponytail: implement if a stage turns out to select it. */
int AXFXChorusInit(struct AXFX_CHORUS* c) { (void) c; return 1; }
int AXFXChorusShutdown(struct AXFX_CHORUS* c) { (void) c; return 1; }
int AXFXChorusSettings(struct AXFX_CHORUS* c) { (void) c; return 1; }
void AXFXChorusCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_CHORUS* c) { (void) b; (void) c; }

/* Delay: one circular line per channel. `delay[i]` is milliseconds,
 * `feedback[i]` and `output[i]` are percentages -- the values the driver
 * defaults to are 260/310/6 ms at 24% feedback and 35% output. State lives in
 * the SDK structure's own currentSize/currentPos/left/right/sur fields; the
 * lines come from the host heap for the same reason the reverb's do. */
int AXFXDelayShutdown(struct AXFX_DELAY* d)
{
    long** lines[3] = { &d->left, &d->right, &d->sur };
    int i;
    for (i = 0; i < 3; i++) {
        free(*lines[i]);
        *lines[i] = NULL;
        d->currentSize[i] = 0;
        d->currentPos[i] = 0;
    }
    return 1;
}

int AXFXDelayInit(struct AXFX_DELAY* d)
{
    long** lines[3] = { &d->left, &d->right, &d->sur };
    int i;

    for (i = 0; i < 3; i++) {
        *lines[i] = NULL;
    }
    for (i = 0; i < 3; i++) {
        u32 n = d->delay[i] * (AX_RATE / 1000u);
        if (n == 0) {
            n = 1;
        }
        *lines[i] = calloc(n, sizeof(long));
        if (*lines[i] == NULL) {
            AXFXDelayShutdown(d);
            return 0;
        }
        d->currentSize[i] = n;
        d->currentPos[i] = 0;
        d->currentFeedback[i] = d->feedback[i];
        d->currentOutput[i] = d->output[i];
    }
    return 1;
}

int AXFXDelaySettings(struct AXFX_DELAY* d)
{
    AXFXDelayShutdown(d);
    return AXFXDelayInit(d);
}

void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_DELAY* d)
{
    long* chan[3] = { b->left, b->right, b->surround };
    long* line[3] = { d->left, d->right, d->sur };
    int c, i;

    for (c = 0; c < 3; c++) {
        u32 size = d->currentSize[c];
        u32 pos = d->currentPos[c];
        long fb = (long) d->currentFeedback[c];
        long og = (long) d->currentOutput[c];

        if (line[c] == NULL || size == 0) {
            continue;
        }
        for (i = 0; i < AX_FRAME; i++) {
            long tap = line[c][pos];
            line[c][pos] = chan[c][i] + tap * fb / 100;
            if (++pos >= size) {
                pos = 0;
            }
            chan[c][i] = tap * og / 100;
        }
        d->currentPos[c] = pos;
    }
}

/* AI: the DTK stream is unused -- Melee's music is HPS played through AX
 * voices, and HSD_SynthStreamSetVolume() scales those voices itself via
 * updateAllVolume(). Routing this onto the master gain faded every sound
 * effect out with the music. */
void AIInit(u8* stack) { (void) stack; }
void AISetDSPSampleRate(u32 rate) { (void) rate; }
void AISetStreamVolLeft(u8 vol) { (void) vol; }
void AISetStreamVolRight(u8 vol) { (void) vol; }
