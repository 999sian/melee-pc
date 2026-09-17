#include <stdlib.h>
#include "synth.h"

#include <math.h> // IWYU pragma: keep
#include <placeholder.h>
#include <string.h>

#include "debug.h"
#include "devcom.h"
#include "synth.static.h"
#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/os.h>

/* Cached once: getenv() scans the whole environment, and these guards sit
 * on per-draw / per-voice paths where that cost is not acceptable even
 * when the diagnostic is switched off. */
static int pc_dbg_sfx_stats(void)
{
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("MELEE_SFX_STATS") != NULL;
    }
    return cached;
}


/* 389334 */ static int HSD_Synth_80389334(int sfx_id, u8 vol, u8 vol2, u8 pan,
                                           int priority, int itd_flag,
                                           float pitch1, float pitch2,
                                           float mix_main, float mix_auxA,
                                           float mix_auxB);

void* HSD_AudioMalloc(size_t size)
{
    void* p = OSAllocFromHeap(HSD_Synth_804D6018, size);
    HSD_ASSERTREPORT(0x29U, p, "audio heap overflow.\n");
    return p;
}

void HSD_AudioFree(void* ptr)
{
    OSFreeToHeap(HSD_Synth_804D6018, ptr);
}

static int HSD_Synth_804D6028[2] = { 0 };
static float HSD_Synth_804D6030 = 1.0f;

static inline s32 SfxLoadStreamDataSize(s32 size)
{
    return size + 8;
}

static void HSD_SynthSFXSampleLoadCallback(int result, uintptr_t length,
                                           void* addr, bool cancelflag)
{
    BOOL intr;
    s32 i;

    if (HSD_Synth_804D7738 == 0) {
        s32 j;
        s32 header_size = hsd_SynthSFXLoadBuf[0].v;
        u32 data_bytes = header_size - 0x10;
        size_t alloc_size;
        u32 total;
        u32 dnw;
        int bankID;
        struct SfxLoadStreamNode** pp;
        struct foo* e;
        s32 count;
        s32 base;

        alloc_size =
            hsd_SynthSFXLoadBuf[2].v * 8 + sizeof(struct SfxLoadStreamNode);
        total = OSRoundUp32B(alloc_size + header_size);
        /* Move the raw entry stream to the end of the allocation and put the
         * 4 words of it that arrived with the header in front of it; all
         * copies are word-for-word so the stream stays big-endian. */
        for (j = (data_bytes >> 2) - 1; j >= 0; j--) {
            ((u32*) HSD_Synth_804D7730)[j + ((total - data_bytes) >> 2)] =
                ((u32*) HSD_Synth_804D7730)[j];
        }
        dnw = total - header_size;
        for (i = 0; i != 4; i++) {
            ((DiscU32*) HSD_Synth_804D7730)[(dnw >> 2) + i] =
                hsd_SynthSFXLoadBuf[4U + i];
        }
        HSD_Synth_804D7734 =
            (DiscU32*) ((u8*) HSD_Synth_804D7730 + (dnw & ~3));

        bankID = HSD_Synth_804C2A60[0].bankID;
        pp = &HSD_Synth_804C2AE0[bankID];
        while (*pp != NULL) {
            pp = &(*pp)->x0;
        }
        *pp = HSD_Synth_804D7730;

        HSD_Synth_804D7730->x0 = NULL;
        HSD_Synth_804D7730->x4 = HSD_Synth_804C2A60[0].entrynum;
        HSD_Synth_804D7730->x10 = hsd_SynthSFXBank[bankID];
        HSD_Synth_804D7730->x14 = hsd_SynthSFXLoadBuf[1].v;
        count = hsd_SynthSFXLoadBuf[2].v;
        base = hsd_SynthSFXLoadBuf[3].v;
        HSD_Synth_804D7730->x8 = base;
        HSD_Synth_804D7730->xC = count;
        e = (struct foo*) (HSD_Synth_804D7730 + 1);
        for (i = 0; i < count; i++) {
            s32 n;
            s32 nbytes;
            s32 k;
            s32 id;

            /* Stream entry: { voice count, sample rate, n * 0x40-byte AX
             * voice blocks }, copied behind the node's next/id words. */
            n = HSD_Synth_804D7734->v;
            nbytes = SfxLoadStreamDataSize(n << 6);
            memcpy((u8*) e + 8, HSD_Synth_804D7734, nbytes);
            for (k = 0; k < n; k++) {
                struct SfxVoiceAddr* a =
                    (struct SfxVoiceAddr*) ((u8*) e + 0x10 + k * 0x40);
                /* Retail always takes this branch (its loopFlag test
                 * degenerated to a non-null check); the else arm would
                 * point non-looping voices at the silence buffer. */
                a->loopAddress += hsd_SynthSFXBank[bankID] * 2;
                a->endAddress += hsd_SynthSFXBank[bankID] * 2;
                a->currentAddress += hsd_SynthSFXBank[bankID] * 2;
            }
            id = base + i;
            e->unk4 = id;
            id &= 0x1F;
            DP_SET(e->next, HSD_Synth_804C29E0[id]);
            HSD_Synth_804C29E0[id] = e;
            HSD_Synth_804D7734 += (u32) nbytes >> 2;
            e = (struct foo*) ((u8*) e + (n << 6) + 0x10);
        }
        if (HSD_Synth_804C2A60[0].x8 != NULL) {
            HSD_Synth_804C2A60[0].x8(HSD_Synth_804C2A60[0].entrynum,
                                     HSD_Synth_804C2A60[0].xC);
        }
        hsd_SynthSFXBank[bankID] += hsd_SynthSFXLoadBuf[1].v;
        HSD_Synth_804D7730 = NULL;
    } else {
        if (HSD_Synth_804D7730 != NULL) {
            HSD_AudioFree(HSD_Synth_804D7730);
            HSD_Synth_804D7730 = NULL;
        }
        HSD_Synth_804D7738 = 0;
    }
    intr = OSDisableInterrupts();
    HSD_Synth_804D772C -= 1;
    for (i = 0; i < HSD_Synth_804D772C; i++) {
        HSD_Synth_804C2A60[i] = HSD_Synth_804C2A60[i + 1];
    }
    HSD_SynthSFXLoadNewProc();
    OSRestoreInterrupts(intr);
}

static void HSD_SynthSFXHeaderLoadCallback(int result, uintptr_t length,
                                           void* addr, bool cancelflag)
{
    s32 header_size;
    size_t alloc_size;

    if (HSD_Synth_804D7738 == 0) {
        int bankID = HSD_Synth_804C2A60[0].bankID;

        HSD_ASSERTREPORT(0xCD,
                         hsd_SynthSFXBankHead[bankID + 1] -
                                 hsd_SynthSFXBank[bankID] >=
                             hsd_SynthSFXLoadBuf[1].v,
                         "Can't load SFX file; bank(id=%d) buffer overflow.\n",
                         HSD_Synth_804C2A60[0].bankID);

        if (hsd_SynthSFXBankHead[bankID + 1] - hsd_SynthSFXBank[bankID] <
            hsd_SynthSFXLoadBuf[1].v)
        {
            BOOL intr;
            int i;
            void (*cb)(int, int) = HSD_Synth_804C2A60[0].x8;
            int entrynum = HSD_Synth_804C2A60[0].entrynum;
            int mode = HSD_Synth_804C2A60[0].xC;

            if (cb != NULL) {
                cb(-1, mode);
            }

            intr = OSDisableInterrupts();
            HSD_Synth_804D772C -= 1;
            for (i = 0; i < HSD_Synth_804D772C; i++) {
                HSD_Synth_804C2A60[i] = HSD_Synth_804C2A60[i + 1];
            }
            HSD_SynthSFXLoadNewProc();
            OSRestoreInterrupts(intr);
            return;
        }

        alloc_size =
            hsd_SynthSFXLoadBuf[2].v * 8 + sizeof(struct SfxLoadStreamNode);
        header_size = hsd_SynthSFXLoadBuf[0].v;
        HSD_Synth_804D7730 =
            HSD_AudioMalloc(OSRoundUp32B(alloc_size + header_size));
        HSD_Synth_804D6028[1] = HSD_DevComRequest(
            HSD_Synth_804C2A60[0].entrynum, 0x20, (uintptr_t) HSD_Synth_804D7730,
            OSRoundUp32B(header_size - 0x10), 0x21, 1, NULL, NULL);
        HSD_Synth_804D6028[0] = HSD_DevComRequest(
            HSD_Synth_804C2A60[0].entrynum, OSRoundUp32B(header_size + 0x10),
            hsd_SynthSFXBank[HSD_Synth_804C2A60[0].bankID],
            hsd_SynthSFXLoadBuf[1].v, 0x23, 1, HSD_SynthSFXSampleLoadCallback,
            NULL);
        return;
    }
    HSD_Synth_804D7730 = NULL;
    HSD_SynthSFXSampleLoadCallback(0, 0, NULL, 0);
}

void HSD_SynthSFXLoadNewProc(void)
{
    if (HSD_Synth_804D772C != 0) {
        bool enabled = OSDisableInterrupts();
        HSD_Synth_804D6028[0] = HSD_DevComRequest(
            HSD_Synth_804C2A60[0].entrynum, 0, (size_t) hsd_SynthSFXLoadBuf,
            0x20, 0x21, 1, HSD_SynthSFXHeaderLoadCallback, NULL);
        OSRestoreInterrupts(enabled);
    }
}

int HSD_SynthSFXLoad(const char* filename, int bankID, void (*cb)(int, int),
                     int mode)
{
    int entrynum;
    bool enabled;

    HSD_ASSERTREPORT(0x103, (bankID >= 0 && bankID < hsd_SynthSFXBankNum),
                     "invalid bankID = %d; filename = %s\n", bankID, filename);

    entrynum = DVDConvertPathToEntrynum(filename);

    while (HSD_Synth_804D772C >= 6) {
    }

    enabled = OSDisableInterrupts();
    HSD_Synth_804C2A60[HSD_Synth_804D772C].entrynum = entrynum;
    HSD_Synth_804C2A60[HSD_Synth_804D772C].bankID = bankID;
    HSD_Synth_804C2A60[HSD_Synth_804D772C].x8 = cb;
    HSD_Synth_804C2A60[HSD_Synth_804D772C].xC = mode;
    HSD_Synth_804D772C += 1;

    if (HSD_Synth_804D772C == 1) {
        HSD_SynthSFXLoadNewProc();
    }

    OSRestoreInterrupts(enabled);
    return entrynum;
}

void HSD_SynthSFXWaitForLoadCompletion(void (*callback)(void))
{
    while (HSD_Synth_804D772C != 0) {
        callback();
    }
}

int HSD_SynthSFXGetPendingLoadCount(void)
{
    return HSD_Synth_804D772C - HSD_Synth_804D7738;
}

int HSD_SynthSFXCancelLoad(int entrynum)
{
    int result = 0;
    bool enabled = OSDisableInterrupts();

    if (HSD_Synth_804D772C != 0) {
        if (HSD_Synth_804C2A60[0].entrynum == entrynum &&
            HSD_Synth_804D7738 == 0)
        {
            int i;
            HSD_Synth_804D7738 = 1;
            for (i = 0; i < 2; i++) {
                HSD_DevComCancelEx(HSD_Synth_804D6028[i], 0, 0, 0);
            }
            result = 1;
        } else {
            int idx = 1;
            while (idx < HSD_Synth_804D772C) {
                if (HSD_Synth_804C2A60[idx].entrynum == entrynum) {
                    int j;
                    for (j = idx; j < HSD_Synth_804D772C - 1; j++) {
                        HSD_Synth_804C2A60[j] = HSD_Synth_804C2A60[j + 1];
                    }
                    HSD_Synth_804D772C--;
                    result = 1;
                    break;
                }
                idx++;
            }
        }
    }
    OSRestoreInterrupts(enabled);
    return result;
}

void HSD_SynthSFXAllocateBank(int size)
{
    int base = hsd_SynthSFXBankHead[hsd_SynthSFXBankNum];

    hsd_SynthSFXBank[hsd_SynthSFXBankNum] = base;
    hsd_SynthSFXBankHead[hsd_SynthSFXBankNum + 1] = size + base;

    HSD_ASSERTREPORT(0x158,
                     hsd_SynthSFXBankHead[hsd_SynthSFXBankNum + 1] <=
                         hsd_SynthSFXBankAREnd,
                     "bank overflow\n");
    hsd_SynthSFXBankNum += 1;
}

#ifdef MUST_MATCH
static void order_data_0(void)
{
    (void) "bank stack underflow\n";
    (void) "hsd_SynthSFXBankNum";
}
#endif

static inline void HSD_SynthSFXUnloadBank_inline(struct SfxLoadStreamNode* bank)
{
    int i;
    for (i = 0; i < bank->xC; i++) {
        HSD_Synth_80388DC8(bank->x8 + i);
    }
}

void HSD_SynthSFXUnloadBank(int bank_id)
{
    struct SfxLoadStreamNode** head;
    HSD_SynthSFXStopRange(bank_id);
    head = &HSD_Synth_804C2AE0[bank_id];
    while (*head != NULL) {
        struct SfxLoadStreamNode* cur;
        HSD_SynthSFXUnloadBank_inline(*head);
        cur = *head;
        *head = (*head)->x0;
        HSD_AudioFree(cur);
    }
    hsd_SynthSFXBank[bank_id] = hsd_SynthSFXBankHead[bank_id];
}

void HSD_Synth_80388DC8(int sfx_id)
{
    struct foo* prev = NULL;
    struct foo* cur = HSD_Synth_804C29E0[sfx_id & 0x1F];

    while (cur != NULL) {
        if (cur->unk4 == sfx_id) {
            if (prev == NULL) {
                HSD_Synth_804C29E0[sfx_id & 0x1F] = DP(struct foo, cur->next);
            } else {
                DP_SET(prev->next, DP(struct foo, cur->next));
            }
            return;
        }
        prev = cur;
        cur = DP(struct foo, cur->next);
    }
}

void HSD_Synth_80388E08(int sfx_id)
{
    struct SfxLoadStreamNode* cur;
    struct SfxLoadStreamNode** pcur;
    int i;

    for (i = 0; i < 0x20; i++) {
        pcur = &HSD_Synth_804C2AE0[i];
        while (*pcur != NULL) {
            cur = *pcur;
            if (cur->x4 == sfx_id) {
                HSD_SynthSFXUnloadBank_inline(cur);
                *pcur = cur->x0;
                HSD_AudioFree(cur);
                return;
            }
            pcur = &cur->x0;
        }
    }
}

static void HSD_SynthSFXGroupDataReaddressCallback(void* result, uintptr_t length,
                                                   void* addr, int cancelflag)
{
    HSD_ASSERT(0x182, sfxGroupDataReaddressCounter > 0);
    sfxGroupDataReaddressCounter--;
}

#ifdef MUST_MATCH
static void order_data_1(void)
{
    (void) "Can't relocate SFX group; bank = %d; sfxgroup = %d\n";
    (void) "hsd_SynthSFXBank[bankID] + group->arsize <= "
           "hsd_SynthSFXBankHead[bankID + 1]";
}
#endif

void HSD_SynthSFXGroupDataReaddress(struct SfxLoadStreamNode* bank,
                                    u32 aram_offset)
{
    struct foo* e;
    int i;
    int count;
    int delta;
    int j;

    e = (struct foo*) (bank + 1);
    sfxGroupDataReaddressCounter += 1;
    HSD_DevComRequest(
        0, bank->x10, aram_offset, bank->x14, 0x1B, 0,
        (HSD_DevComCallback) (Event) HSD_SynthSFXGroupDataReaddressCallback,
        NULL);
    delta = (aram_offset - bank->x10) * 2;
    for (i = 0; i < bank->xC; i++) {
        count = e->unk8;
        for (j = 0; j < count; j++) {
            struct SfxVoiceAddr* a =
                (struct SfxVoiceAddr*) ((u8*) e + 0x10 + j * 0x40);
            if (a->loopFlag != 0) {
                a->loopAddress += delta;
            }
            a->endAddress += delta;
            a->currentAddress += delta;
        }
        e = (struct foo*) ((u8*) e + (count << 6) + 0x10);
    }
    bank->x10 = aram_offset;
}

void HSD_SynthSFXBankDeflag(int bank_id)
{
    struct SfxLoadStreamNode* bank;
    u32 offset;

    HSD_SynthSFXStopRange(bank_id);
    bank = HSD_Synth_804C2AE0[bank_id];
    offset = hsd_SynthSFXBankHead[bank_id];
    while (bank != NULL) {
        if ((u32) bank->x10 != offset) {
            HSD_SynthSFXGroupDataReaddress(bank, offset);
        }
        offset += bank->x14;
        bank = bank->x0;
    }
    /* Retail wrote HSD_Synth_804C2AE0[bank_id + 0x20], which is this slot. */
    hsd_SynthSFXBank[bank_id] = offset;
}

void HSD_SynthSFXBankDeflagSync(void)
{
    while (sfxGroupDataReaddressCounter) {
        continue;
    }
}

u32 HSD_SynthGetSoundMode(void)
{
    return OSGetSoundMode();
}

void HSD_SynthSetSoundMode(int mode)
{
    int i;
    HSD_Synth_804D7754 = mode;
    for (i = 0; i < (int) ARRAY_SIZE(hsd_SynthSFXNodes); i++) {
        if (hsd_SynthSFXNodes[i].x0 > 0) {
            HSD_SynthSFXUpdateMix(&hsd_SynthSFXNodes[i], 1);
        }
    }
    OSSetSoundMode(mode);
}

void HSD_SynthSFXStopNode(struct HSD_SynthSFXNode* node)
{
    int i;
    PAD_STACK(0x10);

    if (!(node->flags & 1) && node->x27 == 1 &&
        driverInactivatedCallback != NULL)
    {
        driverInactivatedCallback(node->x0);
    }
    for (i = 0; i < node->voice_count; i++) {
        AXFreeVoice(node->voice[i]);
        hsd_SynthSFXNodes[node->voice[i]->index].x0 = 0;
    }
}

void dropcallback(void* dropped)
{
    AXVPB* voice = dropped;
    struct HSD_SynthSFXNode* node;
    bool enabled;
    int i;

    PAD_STACK(0x10);

    enabled = OSDisableInterrupts();

    node = &hsd_SynthSFXNodes[voice->index];

    /// Search HSD_Synth_804C28E0 queue for this voice and remove it
    for (i = 0; i < HSD_Synth_804D7720; i++) {
        if (HSD_Synth_804C28E0[i] == voice) {
            HSD_Synth_804C28E0[i] = NULL;
            break;
        }
    }

    if (node->x0 == 0) {
        OSRestoreInterrupts(enabled);
        return;
    }

    if (node->x0 == -1) {
        /// Secondary voice - follow to primary node
        node = &hsd_SynthSFXNodes[node->voice[0]->index];
    }

    if (!(node->flags & 1) && node->x27 == 1 &&
        driverInactivatedCallback != NULL)
    {
        driverInactivatedCallback(node->x0);
    }

    for (i = 0; i < node->voice_count; i++) {
        AXVPB* v = node->voice[i];
        if (v != voice) {
            HSD_Synth_804C28E0[HSD_Synth_804D7720++] = v;
        }
        hsd_SynthSFXNodes[node->voice[i]->index].x0 = 0;
    }

    OSRestoreInterrupts(enabled);
}

/** @remarks The per-voice blocks of an SFX entry are 0x40 apart, which is
 *  less than the AX structures they carry. They are big-endian in memory
 *  (see struct foo in synth.static.h) and are handed to AXSetVoiceAddr /
 *  AXSetVoiceAdpcm / AXSetVoiceAdpcmLoop raw; those three byte-swap their
 *  block (src/pc/audio.c), unlike every other AXSetVoice* which takes
 *  host-native values.
 */
#define SFX_VOICE(i) ((struct foo*) ((u8*) sfx_entry + (i) * 0x40))

static AXPBMIX lbl_80407FB4 = { 0 };

static AXPBSRC HSD_Synth_80407FD8 = { 1, 0, 0, { 0, 0, 0, 0 } };

/* Retail wrote the 16.16 ratio with `*(u32*) &src->ratioHi`, which only lands
 * in the right halves on a big-endian target. */
static void setSrcRatio(AXPBSRC* src, u32 ratio)
{
    src->ratioHi = ratio >> 16;
    src->ratioLo = ratio;
}

int HSD_Synth_80389334(int sfx_id, u8 vol, u8 vol2, u8 pan, int priority,
                       int itd_flag, float pitch1, float pitch2,
                       float mix_main, float mix_auxA, float mix_auxB)
{
    AXVPB* voices[2] = { NULL, NULL };
    UNUSED u8 stack_pad[8];
    AXPBVE ve;
    float vol_norm;
    float vol2_norm;
    int voice_idx;
    u32 node_idx;
    struct foo* sfx_entry;
    struct HSD_SynthSFXNode* sfx_node;
    int saved_interrupts;

    PAD_STACK(0x14);

    saved_interrupts = OSDisableInterrupts();
    sfx_entry = HSD_Synth_804C29E0[sfx_id & 0x1F];

    while (sfx_entry != NULL) {
        if (sfx_entry->unk4 == sfx_id) {
            voice_idx = 0;
            while (voice_idx < sfx_entry->unk8) {
                voices[voice_idx] =
                    AXAcquireVoice(priority + 1, dropcallback, 0U);
                if (voices[voice_idx] == NULL) {
                    if (voices[0] != NULL) {
                        AXFreeVoice(voices[0]);
                    }
                    OSRestoreInterrupts(saved_interrupts);
                    return -1;
                }
                voice_idx += 1;
            }
            if ((sfx_entry->unk8 == 2) && (voices[0] == voices[1])) {
                AXFreeVoice(voices[0]);
                OSRestoreInterrupts(saved_interrupts);
                return -1;
            }
            if (sfx_entry->unk8 == 2) {
                hsd_SynthSFXNodes[voices[1]->index].x0 = -1;
                hsd_SynthSFXNodes[voices[1]->index].voice[0] = voices[0];
            }

            node_idx = voices[0]->index;

            sfx_node = &hsd_SynthSFXNodes[node_idx];
            sfx_node->x27 = 1;
            sfx_node->sfx_id = sfx_id;
            sfx_node->flags = 0;
            sfx_node->voice_count = sfx_entry->unk8;
            sfx_node->xB = itd_flag;
            sfx_node->voice[0] = voices[0];
            sfx_node->voice[1] = voices[1];
            sfx_node->x14 = 0.00003125F * sfx_entry->unkC;
            sfx_node->x18[0] = pitch1;
            sfx_node->x18[1] = pitch2;
            vol_norm = 1 / 255.0F * vol;
            vol2_norm = 1 / 255.0F * vol2;
            sfx_node->unk28 = vol_norm;
            sfx_node->user_vol[0].volume = vol_norm;
            sfx_node->user_vol[0].x4 = 0;
            sfx_node->user_vol[0].x8_float = vol2_norm;
            sfx_node->user_vol[1].volume = vol2_norm;
            sfx_node->user_vol[1].x4 = 0;
            sfx_node->user_vol[1].x8 = pan;
            sfx_node->x44 = mix_main;
            sfx_node->x48 = mix_auxA;
            sfx_node->x4C = mix_auxB;
            HSD_SynthSFXUpdateVolume(sfx_node);
            HSD_SynthSFXUpdateMix(sfx_node, 0);
            ve.currentVolume =
                (32767.0F * (sfx_node->user_vol[0].x8_float *
                             (sfx_node->unk28 *
                              (HSD_Synth_804D6030 *
                               HSD_Synth_804C28E0_1784[sfx_node->xB].x1784))));
            ve.currentDelta = 0;
            sfx_node->x24 = ve.currentVolume;

            voice_idx = 0;
            while (voice_idx < sfx_entry->unk8) {
                AXSetVoicePriority(voices[voice_idx], priority);
                AXSetVoiceVe(voices[voice_idx], &ve);
                setSrcRatio(
                    &HSD_Synth_80407FD8,
                    (u32) (65536.0F * (sfx_node->x18[1] *
                                       (sfx_node->x14 * sfx_node->x18[0]))));
                AXSetVoiceSrc(voices[voice_idx], &HSD_Synth_80407FD8);
                AXSetVoiceAddr(voices[voice_idx],
                               (AXPBADDR*) &SFX_VOICE(voice_idx)->x10);
                AXSetVoiceAdpcm(voices[voice_idx], &SFX_VOICE(voice_idx)->x20);
                AXSetVoiceAdpcmLoop(voices[voice_idx],
                                    &SFX_VOICE(voice_idx)->x48);
                AXSetVoiceState(voices[voice_idx], 1U);
                voice_idx += 1;
            }
            HSD_Synth_804D7750 += 0x40;
            if (HSD_Synth_804D7750 < 0) {
                HSD_Synth_804D7750 = 0x40;
            }
            sfx_node->x0 = HSD_Synth_804D7750 + node_idx;
            if (pc_dbg_sfx_stats()) {
                static unsigned long started;
                started++;
                if (started <= 3 || (started % 200) == 0) {
                    OSReport("sfx started=%lu (latest id=0x%X)\n", started,
                             sfx_id);
                }
            }
            OSRestoreInterrupts(saved_interrupts);
            return sfx_node->x0;
        }
        sfx_entry = DP(struct foo, sfx_entry->next);
    }

    /* MELEE_SFX_STATS=1: a play request whose sfx_id is not in its hash
     * bucket falls through to here and returns -1 silently -- the sound
     * simply never plays. Count those against successful starts, and name
     * the first few missing ids. */
    if (pc_dbg_sfx_stats()) {
        static unsigned long misses;
        misses++;
        if (misses <= 12 || (misses % 200) == 0) {
            OSReport("sfx MISS #%lu id=0x%X (bucket %d)\n", misses, sfx_id,
                     sfx_id & 0x1F);
        }
    }

    OSRestoreInterrupts(saved_interrupts);
    return -1;
}

static inline struct HSD_SynthSFXNode* getNode(int sfx_id)
{
    struct HSD_SynthSFXNode* node = &hsd_SynthSFXNodes[sfx_id & 0x3F];
    if (sfx_id > 0 && node->x0 == sfx_id) {
        return node;
    } else {
        return NULL;
    }
}

/* Returns HSD_Synth_80389334's synth node id (0x40 * n + voice index), or -1.
 * Upstream declares this `bool`; under C11 that is 1-byte `_Bool`, so every id
 * clamps to 1 and the -1 sentinel clamps to 1 as well. The only caller,
 * axdriver.c:217, stores it in `int HSD_SM::vID` and needs both the id (to
 * register in AXDriver_804C5920 and to key the sound off later) and the -1. */
int HSD_SynthSFXPlayWithGroup(int sfx_id, u8 vol, u8 vol2, u8 pan,
                              int priority, int itd_flag, int group,
                              f32 pitch1, f32 pitch2, f32 mix_main,
                              f32 mix_auxA, f32 mix_auxB)
{
    int result;
    int nodeID;
    struct HSD_SynthSFXNode* node;

    PAD_STACK(0x8);

    HSD_ASSERTREPORT(0x30B, group >= 0 && group < HSD_SYNTHSFXGROUP_MAX,
                     "sfx group ID %d out of range.", group);

    if (group != 0) {
        nodeID = HSD_Synth_804C28E0_1844[group];
        if (nodeID > 0) {
            node = getNode(nodeID);
            if (node != NULL && node->flags != 1) {
                if (node->x27 == 1 && driverInactivatedCallback != NULL) {
                    driverInactivatedCallback(node->x0);
                }
                HSD_SynthSFXKeyOff(HSD_Synth_804C28E0_1844[group]);
            }
        }
    }

    result = HSD_Synth_80389334(sfx_id, vol, vol2, pan, priority, itd_flag,
                                pitch1, pitch2, mix_main, mix_auxA, mix_auxB);
    HSD_Synth_804C28E0_1844[group] = result;
    return result;
}

static inline void freeVoices(struct HSD_SynthSFXNode* node)
{
    int j;
    for (j = 0; j < node->voice_count; j++) {
        AXFreeVoice(node->voice[j]);
        hsd_SynthSFXNodes[node->voice[j]->index].x0 = 0;
    }
}

void HSD_SynthSFXKeyOff(int id)
{
    struct HSD_SynthSFXNode* node;
    int i;

    PAD_STACK(0x10);

    if ((node = getNode(id))) {
        if (node->flags & 8) {
            if (!(node->flags & 1) && node->x27 == 1 &&
                driverInactivatedCallback != NULL)
            {
                driverInactivatedCallback(node->x0);
            }
            freeVoices(node);
        } else if (!(node->flags & 1)) {
            node->flags |= 1;
            HSD_SynthSFXUpdateVolume(node);
            for (i = 0; i < node->voice_count; i++) {
                AXSetVoicePriority(node->voice[i], 1U);
            }
        }
    }
}

static inline void stopRange(size_t lo, size_t hi)
{
    size_t addr;
    int i;
    for (i = 0; i < 0x40; i++) {
        struct HSD_SynthSFXNode* node = &hsd_SynthSFXNodes[i];
        if (hsd_SynthSFXNodes[i].x0 > 0) {
            /* The current play position is an address PAIR in field order,
             * not one word: reading it with *(size_t*) takes 8 bytes on LP64
             * (all of `addr` past the pair plus the first two ADPCM coeffs)
             * and the range test never matched, so unloading or relocating a
             * bank left its voices playing out of reused ARAM. */
            addr = ((u32) node->voice[0]->pb.addr.currentAddressHi << 16) |
                   node->voice[0]->pb.addr.currentAddressLo;
            if (addr >= lo && addr < hi) {
                HSD_SynthSFXStopNode(&hsd_SynthSFXNodes[i]);
            }
        }
    }
}

void HSD_SynthSFXStopRange(int bank_id)
{
    stopRange(hsd_SynthSFXBankHead[bank_id + 0] * 2,
              hsd_SynthSFXBankHead[bank_id + 1] * 2);
}

void HSD_SynthSFXPause(int sfx_id)
{
    struct HSD_SynthSFXNode* node;

    if ((node = getNode(sfx_id))) {
        node->flags |= 2;
        HSD_SynthSFXUpdateVolume(node);
        if (node->flags & 8) {
            node->flags |= 6;
        }
    }
}

void HSD_SynthSFXResume(int sfx_id)
{
    struct HSD_SynthSFXNode* node;

    if ((node = getNode(sfx_id))) {
        if (node->flags & 2) {
            node->flags &= ~6;
            HSD_SynthSFXUpdatePitch(node);
            HSD_SynthSFXUpdateVolume(node);
        }
    }
}

int HSD_SynthSFXCheck(int sfx_id)
{
    struct HSD_SynthSFXNode* node = getNode(sfx_id);
    int i;

    if (node != NULL) {
        if (node->flags & 1) {
            return -1;
        }
        for (i = 0; i < node->voice_count; i++) {
            if (node->voice[i]->pb.state == 0) {
                return -1;
            }
        }
        return sfx_id;
    }
    return -1;
}

void HSD_SynthSFXSetVolumeFade(int sfx_id, u8 vol, int flag)
{
    struct HSD_SynthSFXNode* node;

    if ((node = getNode(sfx_id))) {
        HSD_ASSERT(0x376, flag >= 0 && flag < USERVOL_NUM);
        node->user_vol[flag].volume = 1 / 255.0 * vol;
        node->user_vol[flag].x4 = 1;
        HSD_SynthSFXUpdateVolume(node);
    }
}

void HSD_SynthSFXSetUserVol(int sfx_id, u8 vol)
{
    struct HSD_SynthSFXNode* node;

    if ((node = getNode(sfx_id))) {
        node->user_vol[1].x8 = vol;
        HSD_SynthSFXUpdateMix(node, 1);
    }
}

void HSD_SynthSFXSetMix(int sfx_id, float mix_main, float mix_auxA,
                        float mix_auxB)
{
    int* unused;
    struct HSD_SynthSFXNode* node;

    if ((node = getNode(sfx_id))) {
        node->x44 = mix_main;
        node->x48 = mix_auxA;
        node->x4C = mix_auxB;
        HSD_SynthSFXUpdateMix(node, 1);
    }
}

void HSD_SynthSFXUpdatePitch(struct HSD_SynthSFXNode* node)
{
    float ratio;

    if (node->flags & 4) {
        ratio = 0.0F;
    } else {
        ratio = node->x14 * node->x18[0] * node->x18[1];
    }
    if (!(node->flags & 8)) {
        int i;
        for (i = 0; i < node->voice_count; i++) {
            AXSetVoiceSrcRatio(node->voice[i], ratio);
        }
    }
}

void HSD_SynthSFXSetPitchRatio(int sfx_id, int flag, float ratio)
{
    struct HSD_SynthSFXNode* node = getNode(sfx_id);

    if (node != NULL && !(node->flags & 1)) {
        HSD_ASSERT(0x3A7, flag == 0 || flag == 1);

        node->x18[flag] = ratio;

        HSD_SynthSFXUpdatePitch(node);
    }
}

void HSD_SynthSFXSetPriority(int id, int prio)
{
    struct HSD_SynthSFXNode* node = getNode(id);
    int i;

    if (node != NULL && !(node->flags & 1)) {
        for (i = 0; i < node->voice_count; i++) {
            AXSetVoicePriority(node->voice[i], prio);
        }
    }
}

static inline int user_vol_dst_offset(int k)
{
    return k * 3;
}

static inline int user_vol_src_offset(int k)
{
    return k * 3;
}

s32 HSD_Synth_8038A000(void)
{
    struct HSD_SynthSFXNode* node;
    BOOL intr;
    int i;
    struct HSD_SynthSFXNode** pnode;
    AXPBVE ve;

    intr = OSDisableInterrupts();
    if (HSD_Synth_804D7758 != 0) {
        int ch;
        for (ch = 0; ch < 16; ch++) {
            if (HSD_Synth_804D7758 & (1 << ch)) {
                int cnt = HSD_Synth_804C28E0_1784[ch].x178C;
                if (cnt != 0) {
                    f32 cur = HSD_Synth_804C28E0_1784[ch].x1784;
                    HSD_Synth_804C28E0_1784[ch].x1784 =
                        (cur * ((f32) cnt - 1.0f)) / (f32) cnt +
                        HSD_Synth_804C28E0_1784[ch].x1788 / (f32) cnt;
                    HSD_Synth_804C28E0_1784[ch].x178C -= 1;
                }
                if (HSD_Synth_804C28E0_1784[ch].x178C == 0) {
                    HSD_Synth_804C28E0_1784[ch].x1784 =
                        HSD_Synth_804C28E0_1784[ch].x1788;
                    HSD_Synth_804D7758 &= ~(1 << ch);
                }
            }
        }
    }
    pnode = &HSD_Synth_804D774C;
    while (*pnode != NULL) {
        int active = 0;
        s32 vol;
        s32 delta;
        s32 delta_tmp;
        node = *pnode;

        if (node->x0 <= 0) {
            node->volume_update_pending = 0;
            *pnode = node->x20;
            continue;
        }
        {
            int k;
            for (k = 0; k < USERVOL_NUM; k++) {
                if (*(volatile int*) &node->user_vol[k].x4 != 0) {
                    int c = node->user_vol[k].x4;
                    (&node->user_vol[k]
                          .volume)[user_vol_dst_offset(k) - k * 3 - 1] =
                        ((&node->user_vol[k]
                               .volume)[user_vol_src_offset(k) - k * 3 - 1] *
                         ((f32) c - 1.0f)) /
                            (f32) c +
                        node->user_vol[k].volume / (f32) c;
                    node->user_vol[k].x4 -= 1;
                    if (node->user_vol[k].x4 != 0) {
                        active = 1;
                    }
                }
            }
        }
        if (HSD_Synth_804C28E0_1784[node->xB].x178C != 0) {
            active = 1;
        }
        if (!(node->flags & 3)) {
            vol = 32767.0f *
                  (node->user_vol[0].x8_float *
                   (node->unk28 * (HSD_Synth_804D6030 *
                                   HSD_Synth_804C28E0_1784[node->xB].x1784)));
        } else {
            vol = 0;
            active = 0;
        }
        delta_tmp = (vol - node->x24) / 160;
        delta = delta_tmp;
        if (delta_tmp > 0x14) {
            delta = 0x14;
        } else if (delta < -0x14) {
            delta = -0x14;
        }
        for (i = 0; i < node->voice_count; i++) {
            AXSetVoiceVeDelta(node->voice[i], (s16) delta);
        }
        node->x24 += delta * 0xA0;
        if (delta == 0 && (active == 0 || (f32) vol == 0.0f)) {
            node->x24 = (u16) vol;
            ve.currentVolume = vol;
            ve.currentDelta = 0;
            for (i = 0; i < node->voice_count; i++) {
                node->voice[i]->sync &= 0xFFFFFBFF;
                AXSetVoiceVe(node->voice[i], &ve);
            }
            if (active == 0) {
                u8 flags;
                node->volume_update_pending = 0;
                *pnode = node->x20;
                flags = node->flags;
                if (flags & 1) {
                    for (i = 0; i < node->voice_count; i++) {
                        AXFreeVoice(node->voice[i]);
                        hsd_SynthSFXNodes[node->voice[i]->index].x0 = 0;
                    }
                } else if ((flags & 6) == 2) {
                    node->flags = flags | 6;
                    for (i = 0; i < node->voice_count; i++) {
                        HSD_SynthSFXUpdatePitch(node);
                    }
                    if (driverPauseCallback != NULL && node->x27 == 1) {
                        driverPauseCallback(node->x0);
                    }
                }
                continue;
            }
        }
        pnode = &node->x20;
    }
    return OSRestoreInterrupts(intr);
}

void HSD_SynthSFXUpdateVolume(struct HSD_SynthSFXNode* node)
{
    bool enabled = OSDisableInterrupts();
    if (!node->volume_update_pending && !(node->flags & 8)) {
        node->volume_update_pending = true;
        node->x20 = HSD_Synth_804D774C;
        HSD_Synth_804D774C = node;
    }
    OSRestoreInterrupts(enabled);
}

static u8 lbl_8040806C[] = {
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04,
    0x05, 0x05, 0x06, 0x06, 0x07, 0x07, 0x08, 0x08, 0x09, 0x09, 0x0A, 0x0A,
    0x0B, 0x0B, 0x0C, 0x0C, 0x0D, 0x0D, 0x0E, 0x0E, 0x0F, 0x0F, 0x10, 0x10,
    0x11, 0x11, 0x12, 0x12, 0x13, 0x13, 0x14, 0x14, 0x15, 0x15, 0x16, 0x16,
    0x16, 0x17, 0x17, 0x17, 0x17, 0x18, 0x18, 0x18, 0x18, 0x19, 0x19, 0x19,
    0x19, 0x19, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1B,
    0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1D,
    0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1E, 0x1E, 0x1E, 0x1E,
    0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
    0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
    0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
};

static inline void my_memzero(void* dst_raw, size_t size)
{
    int i;
    int* dst = dst_raw;
    for (i = 0; i < size; i += 4) {
        *dst++ = 0;
    }
}

void HSD_SynthSFXUpdateMix(struct HSD_SynthSFXNode* node, int interpolate)
{
    int i;
    int shiftL;
    int shiftR;

    f32 l;
    f32 r;

    if (HSD_Synth_804D7754 != 0) {
        l = 32767.0F *
            sqrtf_accurate(0.003921569F * (0xFF - node->user_vol[1].x8));
        r = 32767.0F * sqrtf_accurate(0.003921569F * node->user_vol[1].x8);
    } else {
        l = 23169.768F;
        r = 23169.768F;
    }
    if (node->voice_count == 1) {
        lbl_80407FB4.vL = l * node->x44;
        lbl_80407FB4.vR = r * node->x44;
        lbl_80407FB4.vAuxAL = l * node->x48;
        lbl_80407FB4.vAuxAR = r * node->x48;
        lbl_80407FB4.vAuxBL = l * node->x4C;
        lbl_80407FB4.vAuxBR = r * node->x4C;
        AXSetVoiceMix(node->voice[0], &lbl_80407FB4);
    } else if (HSD_Synth_804D7754 != 0) {
        lbl_80407FB4.vL = l * node->x44;
        lbl_80407FB4.vAuxAL = l * node->x48;
        lbl_80407FB4.vAuxBL = l * node->x4C;
        AXSetVoiceMix(node->voice[0], &lbl_80407FB4);
        lbl_80407FB4.vAuxBL = 0;
        lbl_80407FB4.vAuxAL = 0;
        lbl_80407FB4.vL = 0;
        lbl_80407FB4.vR = r * node->x44;
        lbl_80407FB4.vAuxAR = r * node->x48;
        lbl_80407FB4.vAuxBR = r * node->x4C;
        AXSetVoiceMix(node->voice[1], &lbl_80407FB4);
    } else {
        l /= 2;
        lbl_80407FB4.vL = l * node->x44;
        lbl_80407FB4.vR = l * node->x44;
        lbl_80407FB4.vAuxAL = l * node->x48;
        lbl_80407FB4.vAuxAR = l * node->x48;
        lbl_80407FB4.vAuxBL = l * node->x4C;
        lbl_80407FB4.vAuxBR = l * node->x4C;
        AXSetVoiceMix(node->voice[0], &lbl_80407FB4);
        AXSetVoiceMix(node->voice[1], &lbl_80407FB4);
    }

    my_memzero(&lbl_80407FB4, sizeof(lbl_80407FB4));

    shiftL = 0;
    shiftR = 0;
    if (HSD_Synth_804D7754 != 0) {
        if (node->user_vol[1].x8 < 0x80) {
            shiftL = lbl_8040806C[0x7F - node->user_vol[1].x8];
        } else {
            shiftR = lbl_8040806C[node->user_vol[1].x8 - 0x80];
        }
    }
    for (i = 0; i < node->voice_count; i++) {
        if (shiftL != 0 || shiftR != 0 || node->voice[0]->pb.itd.flag == 1) {
            if (node->voice[i]->pb.itd.flag == 0 ||
                (node->voice[i]->sync & 0x20))
            {
                AXSetVoiceItdOn(node->voice[i]);
                if (interpolate == 0) {
                    node->voice[i]->pb.itd.shiftR = shiftR;
                    node->voice[i]->pb.itd.shiftL = shiftL;
                }
                node->voice[i]->pb.itd.targetShiftR = shiftR;
                node->voice[i]->pb.itd.targetShiftL = shiftL;
            } else {
                AXSetVoiceItdTarget(node->voice[i], shiftL, shiftR);
            }
        }
    }
}

static inline void updateAllVolume(u32 mask)
{
    int i;
    HSD_Synth_804D7758 |= mask;
    for (i = 0; i < 0x40; i++) {
        struct HSD_SynthSFXNode* node = &hsd_SynthSFXNodes[i];
        if (node->x0 > 0 && ((1 << node->xB) & mask)) {
            HSD_SynthSFXUpdateVolume(node);
        }
    }
}

void HSD_SynthSFXUpdateAllVolume(int vol, u16 fade_frames, int channel)
{
    HSD_Synth_804C28E0_1784[channel].x1788 = 1 / 255.0F * vol;
    if (HSD_Synth_804C28E0_1784[channel].x1784 !=
        HSD_Synth_804C28E0_1784[channel].x1788)
    {
        HSD_Synth_804C28E0_1784[channel].x178C = fade_frames;
        updateAllVolume(1 << channel);
    }
}

void HSD_SynthSFXSetDriverInactivatedCallback(UNK_T callback)
{
    driverInactivatedCallback = callback;
}

void HSD_SynthSFXSetDriverMasterClockCallback(UNK_T callback)
{
    driverMasterClockCallback = callback;
}

void HSD_SynthSFXSetDriverPauseCallback(void (*callback)(s32))
{
    driverPauseCallback = callback;
}

void HSD_SynthCallback(void)
{
    int i;
    int j;
    bool enabled;
    int base;
    PAD_STACK(0x8);

    base = HSD_Synth_804D775C % 8 * 8;

    enabled = OSDisableInterrupts();

    while (HSD_Synth_804D7720-- > 0) {
        if (HSD_Synth_804C28E0[HSD_Synth_804D7720] != 0) {
            AXFreeVoice(HSD_Synth_804C28E0[HSD_Synth_804D7720]);
        }
    }
    HSD_Synth_804D7720 = 0;

    for (i = 0; i < 8; i++) {
        struct HSD_SynthSFXNode* node = &hsd_SynthSFXNodes[base + i];
        if (node->x0 > 0) {
            if (!(node->flags & 8) && node->voice[0]->pb.state == 0 &&
                (node->voice_count == 1 || node->voice[1]->pb.state == 0))
            {
                HSD_SynthSFXStopNode(node);
            }
        }
    }

    HSD_Synth_8038A000();

    if (driverMasterClockCallback != NULL) {
        driverMasterClockCallback(HSD_Synth_804D775C);
    }

    HSD_Synth_8038ADD0();
    HSD_Synth_804D775C++;
    OSRestoreInterrupts(enabled);
}

void HSD_SynthResetStreamCounters(int result, uintptr_t length, void* buf, bool b)
{
    HSD_Synth_804D776C = HSD_Synth_804D7768;
    HSD_Synth_804D7778 = 0;
}

void HSD_Synth_8038AD74(u32 offset, uintptr_t src)
{
    HSD_DevComRequest(HSD_Synth_804D7764, src,
                      HSD_Synth_804D7780 + (HSD_Synth_804D7768 << 16),
                      lbl_804C4540[HSD_Synth_804D7768].x0, 0x23, 0,
                      HSD_SynthResetStreamCounters, 0);
}

static inline void HSD_Synth_8038ADD0_inline(u32 pos)
{
    BOOL intr;
    s32 src;

    if ((s32) ((HSD_Synth_804D776C + 1) % 3) != pos) {
        intr = OSDisableInterrupts();
        if (HSD_Synth_804D7778 != 0 ||
            HSD_Synth_804D7768 != HSD_Synth_804D776C)
        {
            OSRestoreInterrupts(intr);
            return;
        }
        if (getNode(HSD_Synth_804D7760) != NULL) {
            src = lbl_804C4540[HSD_Synth_804D776C].x8;
            if (src == -1) {
                HSD_Synth_804D776C = (HSD_Synth_804D776C + 1) % 3;
            } else {
                HSD_Synth_804D7768 = (HSD_Synth_804D776C + 1) % 3;
                HSD_Synth_804D7778 = 1;
                HSD_DevComRequest(
                    HSD_Synth_804D7764, src,
                    (uintptr_t) &lbl_804C4540[HSD_Synth_804D7768], 0x20, 0x21,
                    0, (HSD_DevComCallback) (Event) HSD_Synth_8038AD74,
                    (void*) (uintptr_t) (src + 0x20));
            }
        }
        OSRestoreInterrupts(intr);
    }
}

void HSD_Synth_8038ADD0(void)
{
    struct HSD_SynthSFXNode* node = getNode(HSD_Synth_804D7760);
    u32 pos;
    s32 i;

    if (node == NULL) {
        return;
    }
    if (node->flags & 8) {
        return;
    }
    /* pb.addr.currentAddress{Hi,Lo} (GC offset 0x1B2 in AXVPB). */
    pos = (((u32) node->voice[0]->pb.addr.currentAddressHi << 16 |
            node->voice[0]->pb.addr.currentAddressLo) -
           HSD_Synth_804D7780 * 2) >>
          0x11;
    if (pos != HSD_Synth_804D7774) {
        HSD_Synth_804D7774 = pos;
        for (i = 0; i < node->voice_count; i++) {
            AXSetVoiceEndAddr(
                node->voice[i],
                (HSD_Synth_804D7780 + (HSD_Synth_804D7774 << 0x10)) * 2 +
                    i * lbl_804C4540[HSD_Synth_804D7774].x0 +
                    lbl_804C4540[HSD_Synth_804D7774].x4);
        }
    }
    if (pos == HSD_Synth_804D7770 && pos != HSD_Synth_804D776C) {
        if ((u32) lbl_804C4540[HSD_Synth_804D7770].x8 == -1U) {
            HSD_Synth_804D7770 = (HSD_Synth_804D7770 + 1) % 3;
            for (i = 0; i < node->voice_count; i++) {
                AXSetVoiceLoop(node->voice[i], 0);
                AXSetVoiceLoopAddr(node->voice[i], HSD_Synth_804D7784);
            }
        } else {
            HSD_Synth_804D7770 = (HSD_Synth_804D7770 + 1) % 3;
            for (i = 0; i < node->voice_count; i++) {
                AXSetVoiceLoopAddr(
                    node->voice[i],
                    (HSD_Synth_804D7780 + (HSD_Synth_804D7770 << 0x10)) * 2 +
                        i * lbl_804C4540[HSD_Synth_804D7770].x0 + 2);
                AXSetVoiceAdpcmLoop(
                    node->voice[i],
                    (AXPBADPCMLOOP*) ((u32*) &lbl_804C4540
                                          [HSD_Synth_804D7770] +
                                      (i * 2 + 3)));
            }
        }
    }
    HSD_Synth_8038ADD0_inline(pos);
}

void HSD_Synth_8038B120(void)
{
    AXPBVE ve;
    int i;
    bool enabled;
    struct HSD_SynthSFXNode* node;

    PAD_STACK(0x10);

    node = getNode(HSD_Synth_804D7760);
    if (node != NULL) {
        if (!(node->flags & 2)) {
            ve.currentVolume =
                (32767.0F *
                 (node->user_vol[0].x8_float *
                  (node->unk28 * (HSD_Synth_804D6030 *
                                  HSD_Synth_804C28E0_1784[node->xB].x1784))));
        } else {
            ve.currentVolume = 0;
        }
        ve.currentDelta = 0;
        node->x24 = ve.currentVolume;
        for (i = 0; i < node->voice_count; i++) {
            AXSetVoiceVe(node->voice[i], &ve);
            if (node->flags & 4) {
                setSrcRatio(&HSD_Synth_80407FD8, 0);
            } else {
                setSrcRatio(&HSD_Synth_80407FD8,
                            (u32) (65536.0F * (node->x14 * node->x18[0] *
                                               node->x18[1])));
            }
            AXSetVoiceSrc(node->voice[i], &HSD_Synth_80407FD8);
            AXSetVoiceCurrentAddr(
                node->voice[i],
                (HSD_Synth_804D7780 + (HSD_Synth_804D7768 << 16)) * 2 +
                    i * lbl_804C4540[HSD_Synth_804D7768].x0 + 2);
            AXSetVoiceEndAddr(
                node->voice[i],
                (HSD_Synth_804D7780 + (HSD_Synth_804D7768 << 16)) * 2 +
                    i * lbl_804C4540[HSD_Synth_804D7768].x0 +
                    lbl_804C4540[HSD_Synth_804D7768].x4);
            AXSetVoiceLoopAddr(
                node->voice[i],
                (HSD_Synth_804D7780 + (HSD_Synth_804D7768 << 16)) * 2 +
                    i * lbl_804C4540[HSD_Synth_804D7768].x0 + 2);
            AXSetVoiceState(node->voice[i], 1);
        }
        node->flags &= ~8;
        enabled = OSDisableInterrupts();
        if (!node->volume_update_pending && !(node->flags & 8)) {
            node->volume_update_pending = 1;
            node->x20 = HSD_Synth_804D774C;
            HSD_Synth_804D774C = node;
        }
        OSRestoreInterrupts(enabled);
        HSD_SynthSFXUpdateMix(node, 0);
        HSD_Synth_804D7778 = 0;
    } else {
        HSD_Synth_804D7778 = 0;
    }
}

void HSD_SynthPStreamFirstHakoHeaderCallback(void)
{
    HSD_DevComRequest(HSD_Synth_804D7764, 0xA0,
                      HSD_Synth_804D7780 + (HSD_Synth_804D7768 << 16),
                      lbl_804C4540[HSD_Synth_804D7768].x0, 0x23, 0,
                      (HSD_DevComCallback) HSD_Synth_8038B120, 0);
}

void HSD_SynthPStreamHeaderCallback(int arg0, uintptr_t arg1, void* arg2,
                                    bool cancelflag)
{
    DiscU32* entry = arg2; /* HPS header, big-endian */
    struct HSD_SynthSFXNode* node;
    int i;

    node = getNode(HSD_Synth_804D7760);
    if (node != NULL) {
        node->voice_count = entry[3].v;
        if (node->voice_count == 2) {
            node->voice[1] = AXAcquireVoice(0x1D, dropcallback, 0);
            HSD_ASSERTMSG(0x5CF, node->voice[1], "entry->voice[1]");
        }
        node->x14 = 0.00003125f * (f32) entry[2].v;
        for (i = 0; i < node->voice_count; i++) {
            setSrcRatio(&HSD_Synth_80407FD8,
                        (u32) (65536.0f * node->x14));
            /* Raw big-endian block; AXSetVoiceAddr/Adpcm byte-swap it. */
            AXSetVoiceAddr(node->voice[i], (AXPBADDR*) &entry[i * 14 + 4]);
            AXSetVoiceAdpcm(node->voice[i], (AXPBADPCM*) &entry[i * 14 + 8]);
        }
        HSD_Synth_804D7774 = (HSD_Synth_804D7774 + 2) % 3;
        HSD_Synth_804D776C = HSD_Synth_804D7770 = HSD_Synth_804D7768 =
            HSD_Synth_804D7774;
        HSD_DevComRequest(
            HSD_Synth_804D7764, 0x80,
            (uintptr_t) &lbl_804C4540[HSD_Synth_804D7768], 0x20, 0x21, 0,
            (HSD_DevComCallback) HSD_SynthPStreamFirstHakoHeaderCallback,
            NULL);
    } else {
        HSD_Synth_804D7778 = 0;
    }
}

static inline void HSD_Synth_8038B5AC_inline(void)
{
    struct HSD_SynthSFXNode* node = getNode(HSD_Synth_804D7760);
    int i;
    int saved_interrupts;

    if (!node) {
        return;
    }

    if (node->flags & 8) {
        HSD_SynthSFXStopNode(node);
    } else if (!(node->flags & 1)) {
        node->flags |= 1;
        saved_interrupts = OSDisableInterrupts();
        if (node->volume_update_pending == 0 && !(node->flags & 8)) {
            node->volume_update_pending = 1;
            node->x20 = HSD_Synth_804D774C;
            HSD_Synth_804D774C = node;
        }
        OSRestoreInterrupts(saved_interrupts);
        for (i = 0; i < node->voice_count; i++) {
            AXSetVoicePriority(node->voice[i], 1);
        }
    }
}

int HSD_Synth_8038B5AC(int entrynum, u8 vol, u8 vol2, int channel)
{
    bool enabled;

    struct HSD_SynthSFXNode* voice_node;
    int idx;
    AXVPB* voice;

    PAD_STACK(8);

    do {
    } while (HSD_Synth_804D7778 != 0);

    HSD_Synth_804D7778 = 1;
    enabled = OSDisableInterrupts();

    if (getNode(HSD_Synth_804D7760) != NULL) {
        HSD_Synth_8038B5AC_inline();
    }
    HSD_Synth_804D7764 = entrynum;
    voice = AXAcquireVoice(0x1D, dropcallback, 0);
    idx = voice->index;
    voice_node = &hsd_SynthSFXNodes[idx];
    HSD_Synth_804D7750 += 0x40;
    if (HSD_Synth_804D7750 < 0) {
        HSD_Synth_804D7750 = 0x40;
    }
    voice_node->x27 = 2;
    voice_node->x0 = HSD_Synth_804D7760 = HSD_Synth_804D7750 + idx;
    voice_node->sfx_id = 0;
    voice_node->flags = 8;
    voice_node->voice_count = 1;
    voice_node->xB = (u8) channel;
    voice_node->voice[0] = voice;
    voice_node->x18[0] = 1.0F;
    voice_node->x18[1] = 1.0F;
    voice_node->unk28 = 0.003921569F * vol;
    voice_node->user_vol[0].volume = 0.003921569F * vol;
    voice_node->user_vol[0].x4 = 0;
    voice_node->user_vol[0].x8_float = 0.003921569F * vol2;
    voice_node->user_vol[1].volume = 0.003921569F * vol2;
    voice_node->user_vol[1].x4 = 0;
    voice_node->user_vol[1].x8 = 0x80;
    voice_node->x44 = 1.0F;
    voice_node->x48 = 0.0F;
    voice_node->x4C = 0.0F;
    HSD_DevComRequest(entrynum, 0, 0, 0x80, 0x22, 1,
                      HSD_SynthPStreamHeaderCallback, NULL);
    OSRestoreInterrupts(enabled);
    return HSD_Synth_804D7760;
}

void HSD_SynthStreamSetVolume(f32 volume)
{
    HSD_Synth_804D6030 = volume;
    AISetStreamVolLeft(HSD_Synth_804D6030 * (f32) HSD_Synth_804D777C);
    AISetStreamVolRight(HSD_Synth_804D6030 * (f32) HSD_Synth_804D777C);
    updateAllVolume(0xFFFF);
}

void HSD_SynthInit(int dsp_size, int voices, int stream_size, int bank_size)
{
    AXInit();
    AISetDSPSampleRate(0);
    HSD_Synth_804D7784 = ARAlloc(0x500);
    HSD_DevComRequest(0, 0, HSD_Synth_804D7784, 0x500, 3, 0, 0, 0);
    HSD_Synth_804D7784 *= 2;
    hsd_SynthSFXBankHead[0] = ARAlloc(bank_size);
    AXRegisterCallback(HSD_SynthCallback);
    HSD_Synth_804D777C = 0xFF;
    AISetStreamVolLeft(HSD_Synth_804D6030 * (f32) HSD_Synth_804D777C);
    AISetStreamVolRight(HSD_Synth_804D6030 * (f32) HSD_Synth_804D777C);
    /// Loop unrolled to match original binary (loop version adds 8 bytes
    /// to stack frame due to compiler allocating space for the counter)
    HSD_Synth_804C28E0_1784[0].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[0].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[0].x178C = 0;
    HSD_Synth_804C28E0_1784[1].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[1].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[1].x178C = 0;
    HSD_Synth_804C28E0_1784[2].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[2].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[2].x178C = 0;
    HSD_Synth_804C28E0_1784[3].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[3].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[3].x178C = 0;
    HSD_Synth_804C28E0_1784[4].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[4].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[4].x178C = 0;
    HSD_Synth_804C28E0_1784[5].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[5].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[5].x178C = 0;
    HSD_Synth_804C28E0_1784[6].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[6].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[6].x178C = 0;
    HSD_Synth_804C28E0_1784[7].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[7].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[7].x178C = 0;
    HSD_Synth_804C28E0_1784[8].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[8].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[8].x178C = 0;
    HSD_Synth_804C28E0_1784[9].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[9].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[9].x178C = 0;
    HSD_Synth_804C28E0_1784[10].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[10].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[10].x178C = 0;
    HSD_Synth_804C28E0_1784[11].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[11].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[11].x178C = 0;
    HSD_Synth_804C28E0_1784[12].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[12].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[12].x178C = 0;
    HSD_Synth_804C28E0_1784[13].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[13].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[13].x178C = 0;
    HSD_Synth_804C28E0_1784[14].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[14].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[14].x178C = 0;
    HSD_Synth_804C28E0_1784[15].x1784 = 1.0F;
    HSD_Synth_804C28E0_1784[15].x1788 = 1.0F;
    HSD_Synth_804C28E0_1784[15].x178C = 0;
    hsd_SynthSFXBankAREnd = hsd_SynthSFXBankHead[0] + bank_size;
    HSD_Synth_804D7780 = ARAlloc(0x30000);
    HSD_Synth_804D7754 = OSGetSoundMode();
}

/// @remarks Nothing in the DOL references this; it is recovered from the tail
/// of this TU's `.data`.
static u8 HSD_Synth_804080FC[0x44] = { 0 };
