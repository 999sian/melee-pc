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
} Voice;

static Voice s_voices[AX_VOICES];
static void (*s_frame_callback)(void);
static SDL_AudioStream* s_stream;
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

static inline s16 clamp16(s32 v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16) v;
}

/* ---- sample fetch ------------------------------------------------------ */

/* Reads the next source sample; returns false once the voice has ended. */
static bool next_sample(Voice* v, s16* out)
{
    AXPB* pb = &v->vpb.pb;
    u16 format = pb->addr.format;

    if (v->cur_addr >= v->end_addr) {
        if (!pb->addr.loopFlag) {
            return false;
        }
        v->cur_addr = v->loop_addr;
        if (format == AX_FORMAT_ADPCM) {
            v->pred_scale = pb->adpcmLoop.loop_pred_scale;
            v->yn1 = (s16) pb->adpcmLoop.loop_yn1;
            v->yn2 = (s16) pb->adpcmLoop.loop_yn2;
        }
    }

    switch (format) {
    case AX_FORMAT_ADPCM: {
        /* 16 nibbles per frame: 2 header nibbles, 14 sample nibbles. */
        if ((v->cur_addr & 15) == 0) {
            v->pred_scale = s_aram[v->cur_addr >> 1];
            v->cur_addr += 2;
        }
        u8 byte = s_aram[v->cur_addr >> 1];
        s32 nibble = (v->cur_addr & 1) ? (byte & 0xF) : (byte >> 4);
        nibble = (nibble << 28) >> 28; /* sign-extend */
        s32 scale = 1 << (v->pred_scale & 0xF);
        u32 coef = (v->pred_scale >> 4) & 7;
        s32 c0 = (s16) pb->adpcm.a[coef][0];
        s32 c1 = (s16) pb->adpcm.a[coef][1];
        s32 sample = clamp16(((nibble * scale << 11) + 1024 + c0 * v->yn1 + c1 * v->yn2) >> 11);
        v->yn2 = v->yn1;
        v->yn1 = sample;
        v->cur_addr++;
        *out = (s16) sample;
        return true;
    }
    case AX_FORMAT_PCM16: {
        const u8* p = &s_aram[v->cur_addr * 2];
        *out = (s16) ((p[0] << 8) | p[1]);
        v->cur_addr++;
        return true;
    }
    case AX_FORMAT_PCM8:
        *out = (s16) ((s8) s_aram[v->cur_addr] << 8);
        v->cur_addr++;
        return true;
    default:
        return false;
    }
}

/* ---- mixing ------------------------------------------------------------ */

static void mix_voice(Voice* v, float* out)
{
    AXPB* pb = &v->vpb.pb;
    u32 ratio = addr32(pb->src.ratioHi, pb->src.ratioLo);
    s32 vol = pb->ve.currentVolume;
    s32 delta = pb->ve.currentDelta;
    float vl = pb->mix.vL / 32767.0f;
    float vr = pb->mix.vR / 32767.0f;

    for (int i = 0; i < AX_FRAME; i++) {
        v->frac += ratio;
        while (v->frac >= 0x10000) {
            s16 s;
            if (!next_sample(v, &s)) {
                pb->state = 0;
                pb->ve.currentVolume = (u16) (vol < 0 ? 0 : vol > 32767 ? 32767 : vol);
                return;
            }
            v->prev = v->cur;
            v->cur = s;
            v->frac -= 0x10000;
        }
        float t = (float) v->frac * (1.0f / 65536.0f);
        float s = ((float) v->prev + t * (float) (v->cur - v->prev)) * (1.0f / 32768.0f);
        float g = (float) vol * (1.0f / 32767.0f);
        out[i * 2] += s * g * vl;
        out[i * 2 + 1] += s * g * vr;
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

static void render_frame(float* out)
{
    memset(out, 0, sizeof(float) * AX_FRAME * 2);
    BOOL intr = OSDisableInterrupts();
    if (s_frame_callback) {
        s_frame_callback();
    }
    int n_used = 0, n_running = 0, n_zero_mix = 0;
    for (int i = 0; i < AX_VOICES; i++) {
        Voice* v = &s_voices[i];
        if (v->used) {
            n_used++;
        }
        if (v->used && v->vpb.pb.state == 1) {
            n_running++;
            if (v->vpb.pb.mix.vL == 0 && v->vpb.pb.mix.vR == 0) {
                n_zero_mix++;
            }
            mix_voice(v, out);
        }
    }
    /* MELEE_AUDIO_STATS=1: voice census against the output clock, so a
     * silent stretch in MELEE_AUDIO_DUMP can be explained -- were there no
     * voices, were they all stopped, or were they running at zero volume? */
    if (pc_dbg_audio_stats()) {
        static unsigned long frames;
        frames++;
        if ((frames % 100) == 0) { /* every 100 * 5ms = 0.5s of output */
            fprintf(stderr, "voices t=%.1fs used=%d running=%d zero_mix=%d\n",
                    frames * (double) AX_FRAME / AX_RATE, n_used, n_running,
                    n_zero_mix);
        }
    }
    OSRestoreInterrupts(intr);
    for (int i = 0; i < AX_FRAME * 2; i++) {
        out[i] *= s_master;
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

void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    p->priority = (int) priority;
}

void AXSetVoiceState(AXVPB* p, u16 state)
{
    p->pb.state = state;
}

void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix)
{
    p->pb.mix = *mix;
}

void AXSetVoiceItdOn(AXVPB* p)
{
    p->pb.itd.flag = 1;
}

void AXSetVoiceItdTarget(AXVPB* p, u16 l, u16 r)
{
    p->pb.itd.targetShiftL = l;
    p->pb.itd.targetShiftR = r;
}

void AXSetVoiceVe(AXVPB* p, AXPBVE* ve)
{
    p->pb.ve = *ve;
}

void AXSetVoiceVeDelta(AXVPB* p, s16 delta)
{
    p->pb.ve.currentDelta = delta;
}

/* Big-endian block from disc. */
void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr)
{
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
}

void AXSetVoiceLoop(AXVPB* p, u16 loop)
{
    p->pb.addr.loopFlag = loop;
}

void AXSetVoiceLoopAddr(AXVPB* p, u32 addr)
{
    Voice* v = (Voice*) p;
    v->loop_addr = addr;
    set_addr(&p->pb.addr.loopAddressHi, &p->pb.addr.loopAddressLo, addr);
}

void AXSetVoiceEndAddr(AXVPB* p, u32 addr)
{
    Voice* v = (Voice*) p;
    v->end_addr = addr;
    set_addr(&p->pb.addr.endAddressHi, &p->pb.addr.endAddressLo, addr);
}

void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr)
{
    Voice* v = (Voice*) p;
    v->cur_addr = addr;
    set_addr(&p->pb.addr.currentAddressHi, &p->pb.addr.currentAddressLo, addr);
}

/* Big-endian block from disc. */
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* a)
{
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
}

/* Big-endian block from disc. */
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* l)
{
    p->pb.adpcmLoop.loop_pred_scale = be16(l->loop_pred_scale);
    p->pb.adpcmLoop.loop_yn1 = be16(l->loop_yn1);
    p->pb.adpcmLoop.loop_yn2 = be16(l->loop_yn2);
}

/* The synth stores the 16.16 ratio as one native u32 over ratioHi/ratioLo. */
void AXSetVoiceSrc(AXVPB* p, AXPBSRC* s)
{
    u32 ratio;
    memcpy(&ratio, &s->ratioHi, sizeof(ratio));
    set_addr(&p->pb.src.ratioHi, &p->pb.src.ratioLo, ratio);
}

void AXSetVoiceSrcRatio(AXVPB* p, float ratio)
{
    set_addr(&p->pb.src.ratioHi, &p->pb.src.ratioLo, (u32) (ratio * 65536.0f));
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context) { (void) callback; (void) context; }
void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context) { (void) callback; (void) context; }

/* AXFX: effects run on the aux busses, which are not mixed. */
int AXFXReverbHiInit(struct AXFX_REVERBHI* rev) { (void) rev; return 1; }
int AXFXReverbHiShutdown(struct AXFX_REVERBHI* rev) { (void) rev; return 1; }
void AXFXReverbHiCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_REVERBHI* r) { (void) b; (void) r; }
int AXFXReverbStdInit(struct AXFX_REVERBSTD* rev) { (void) rev; return 1; }
int AXFXReverbStdShutdown(struct AXFX_REVERBSTD* rev) { (void) rev; return 1; }
void AXFXReverbStdCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_REVERBSTD* r) { (void) b; (void) r; }
int AXFXChorusInit(struct AXFX_CHORUS* c) { (void) c; return 1; }
int AXFXChorusShutdown(struct AXFX_CHORUS* c) { (void) c; return 1; }
void AXFXChorusCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_CHORUS* c) { (void) b; (void) c; }
int AXFXDelayInit(struct AXFX_DELAY* d) { (void) d; return 1; }
int AXFXDelayShutdown(struct AXFX_DELAY* d) { (void) d; return 1; }
void AXFXDelayCallback(struct AXFX_BUFFERUPDATE* b, struct AXFX_DELAY* d) { (void) b; (void) d; }
void AXFXSetHooks(void* (*alloc_hook)(unsigned long), void (*free_hook)(void*)) { (void) alloc_hook; (void) free_hook; }

/* AI: the DTK stream volume doubles as our master volume. */
void AIInit(u8* stack) { (void) stack; }
void AISetDSPSampleRate(u32 rate) { (void) rate; }
void AISetStreamVolLeft(u8 vol)
{
    /* MELEE_AUDIO_STATS=1: this maps the DTK *music stream* volume onto the
     * master gain for the whole mix. If the game fades music out at a scene
     * transition, that silences sound effects too. Log every change so the
     * value can be lined up against silent stretches in MELEE_AUDIO_DUMP. */
    if (pc_dbg_audio_stats() && vol != (u8) (s_master * 255.0f)) {
        static unsigned long n;
        fprintf(stderr, "AISetStreamVolLeft #%lu vol=%u (master %.3f -> %.3f)\n",
                ++n, vol, s_master, vol / 255.0f);
    }
    s_master = vol / 255.0f;
}
void AISetStreamVolRight(u8 vol) { (void) vol; }
