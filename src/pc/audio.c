/*
 * AX / AXFX / AI stubs.
 *
 * ponytail: no audio yet. Voices are handed out and parameters accepted so
 * sysdolphin's synth runs unchanged; nothing is mixed. Replace with a software
 * mixer driven from an SDL audio stream (ADPCM decode from ARAM) later.
 */
#include <dolphin/ai.h>
#include <dolphin/ax.h>
#include <dolphin/axfx.h>

#include <string.h>

#define PC_AX_VOICES 64

static AXVPB s_voices[PC_AX_VOICES];
static u8 s_voice_used[PC_AX_VOICES];

void AXInit(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    memset(s_voice_used, 0, sizeof(s_voice_used));
    for (int i = 0; i < PC_AX_VOICES; i++) {
        s_voices[i].index = i;
    }
}

void AXQuit(void) {}

AXVPB* AXAcquireVoice(u32 priority, void (*callback)(void*), u32 userContext)
{
    for (int i = 0; i < PC_AX_VOICES; i++) {
        if (!s_voice_used[i]) {
            s_voice_used[i] = 1;
            AXVPB* v = &s_voices[i];
            v->priority = priority;
            v->callback = callback;
            v->userContext = userContext;
            return v;
        }
    }
    (void) priority;
    return NULL;
}

void AXFreeVoice(AXVPB* p)
{
    if (p) {
        s_voice_used[p->index] = 0;
    }
}

void AXSetVoicePriority(AXVPB* p, u32 priority)
{
    p->priority = priority;
}

void AXRegisterAuxACallback(void (*callback)(void*, void*), void* context)
{
    (void) callback;
    (void) context;
}

void AXRegisterAuxBCallback(void (*callback)(void*, void*), void* context)
{
    (void) callback;
    (void) context;
}

void AXRegisterCallback(void (*callback)())
{
    (void) callback;
}

void AXSetVoiceState(AXVPB* p, u16 state) { (void) p; (void) state; }
void AXSetVoiceMix(AXVPB* p, AXPBMIX* mix) { (void) p; (void) mix; }
void AXSetVoiceItdOn(AXVPB* p) { (void) p; }
void AXSetVoiceItdTarget(AXVPB* p, u16 l, u16 r) { (void) p; (void) l; (void) r; }
void AXSetVoiceVe(AXVPB* p, AXPBVE* ve) { (void) p; (void) ve; }
void AXSetVoiceVeDelta(AXVPB* p, s16 delta) { (void) p; (void) delta; }
void AXSetVoiceAddr(AXVPB* p, AXPBADDR* addr) { (void) p; (void) addr; }
void AXSetVoiceLoop(AXVPB* p, u16 loop) { (void) p; (void) loop; }
void AXSetVoiceLoopAddr(AXVPB* p, u32 addr) { (void) p; (void) addr; }
void AXSetVoiceEndAddr(AXVPB* p, u32 addr) { (void) p; (void) addr; }
void AXSetVoiceCurrentAddr(AXVPB* p, u32 addr) { (void) p; (void) addr; }
void AXSetVoiceAdpcm(AXVPB* p, AXPBADPCM* a) { (void) p; (void) a; }
void AXSetVoiceSrc(AXVPB* p, AXPBSRC* s) { (void) p; (void) s; }
void AXSetVoiceSrcRatio(AXVPB* p, float ratio) { (void) p; (void) ratio; }
void AXSetVoiceAdpcmLoop(AXVPB* p, AXPBADPCMLOOP* l) { (void) p; (void) l; }

/* AXFX */
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

/* AI */
void AIInit(u8* stack) { (void) stack; }
void AISetDSPSampleRate(u32 rate) { (void) rate; }
void AISetStreamVolLeft(u8 vol) { (void) vol; }
void AISetStreamVolRight(u8 vol) { (void) vol; }
