#ifndef SYSDOLPHIN_BASELIB_SYNTH_STATIC_H
#define SYSDOLPHIN_BASELIB_SYNTH_STATIC_H

#include <dolphin/ax.h>
#include <dolphin/os.h>
#include <sysdolphin/baselib/synth.h> // IWYU pragma: export

OSHeapHandle HSD_Synth_804D6018 = -1; // audio heap

/* A loaded SSM bank: one head node (native) followed in the same allocation
 * by the entry nodes. Entry nodes keep the on-disc 0x10-byte header layout
 * (32-bit next slot) because the AX voice blocks that follow are addressed
 * at fixed 0x40 strides from it. */
struct SfxLoadStreamNode {
    /* 0x00 */ struct SfxLoadStreamNode* x0; ///< next bank in the same bankID
    /* 0x04 */ s32 x4;                       ///< DVD entrynum
    /* 0x08 */ s32 x8;                       ///< first sfx id
    /* 0x0C */ s32 xC;                       ///< entry count
    /* 0x10 */ s32 x10;                      ///< ARAM offset of sample data
    /* 0x14 */ s32 x14;                      ///< sample data bytes
};

/* AXPBADDR with the hi/lo address halves merged; the entry stream stores
 * them big-endian and the readdress code patches them as u32. */
struct DISC_STRUCT SfxVoiceAddr {
    /* 0x00 */ u16 loopFlag;
    /* 0x02 */ u16 format;
    /* 0x04 */ u32 loopAddress;
    /* 0x08 */ u32 endAddress;
    /* 0x0C */ u32 currentAddress;
};
DISC_ASSERT_SIZE(struct SfxVoiceAddr, 0x10);

/* One SSM entry, copied verbatim (big-endian) from the file's entry stream.
 * The per-voice AX parameter blocks at 0x10 + i*0x40 stay big-endian and are
 * handed to AXSetVoice* as-is: a real mixer must byte-swap them. */
struct DISC_STRUCT foo {
    /* 0x00 */ DISC_PTR(struct foo) next; ///< bucket chain
    /* 0x04 */ int unk4;                  ///< sound ID
    /* 0x08 */ int unk8;                  ///< voice count
    /* 0x0C */ int unkC;                  ///< sample rate
    /* 0x10 */ struct SfxVoiceAddr x10;
    /* 0x20 */ AXPBADPCM x20;
    /* 0x48 */ AXPBADPCMLOOP x48;
};
DISC_ASSERT_SIZE(struct foo, 0x50);

/// Named after the assertion text pooled in this TU's `.data`.
struct HSD_SynthSFXGroup {
    int arsize;
};

#define USERVOL_NUM 2

struct HSD_SynthSFXNode {
    /* 00 */ int x0;
    /* 04 */ int sfx_id;
    /* 08 */ u8 pad8;
    /* 09 */ u8 flags;
    /* 0A */ u8 voice_count;
    /* 0B */ u8 xB;
    /* 0C */ AXVPB* voice[2];
    /* 14 */ float x14;
    /* 18 */ float x18[2];
    /* 20 */ struct HSD_SynthSFXNode* x20;
    /* 24 */ u16 x24;
    /* 26 */ u8 volume_update_pending;
    /* 27 */ u8 x27;
    /* 28 */ float unk28;
    /* 2C */ struct {
        /* 2C */ float volume;
        /* 30 */ int x4;
        union {
            /* 34 */ u8 x8;
            /* 34 */ float x8_float;
        };
    } user_vol[USERVOL_NUM];
    /* 44 */ float x44;
    /* 48 */ float x48;
    /* 4C */ float x4C;
};

static AXVPB* HSD_Synth_804C28E0[0x100 / 4];
static struct foo* HSD_Synth_804C29E0[0x80 / 4]; ///< entries by (id & 0x1F)
static struct {
    /* 00 */ int entrynum;
    /* 04 */ int bankID;
    /* 08 */ void (*x8)(int, int);
    /* 0C */ int xC;
} HSD_Synth_804C2A60[6];
static DiscU32 hsd_SynthSFXLoadBuf[0x20 / 4] __attribute__((aligned(32))); /* raw SSM header, big-endian */
static struct SfxLoadStreamNode* HSD_Synth_804C2AE0[0x80 / 4];
static int hsd_SynthSFXBank[0x80 / 4];
static int hsd_SynthSFXBankHead[0x84 / 4];
static struct HSD_SynthSFXNode hsd_SynthSFXNodes[0x40];

static struct {
    float x1784;
    float x1788;
    int x178C;
} HSD_Synth_804C28E0_1784[0xC0 / 0xC];

#define HSD_SYNTHSFXGROUP_MAX 0x100

static int HSD_Synth_804C28E0_1844[HSD_SYNTHSFXGROUP_MAX];

static u8 lbl_804C4524[0x1C];

/* HPS block headers, DVD-loaded verbatim (big-endian). Words 3.. hold the
 * per-channel AXPBADPCMLOOP blocks. */
static struct DISC_STRUCT {
    /* 00 */ s32 x0;
    /* 04 */ s32 x4;
    /* 08 */ s32 x8;
    /* 0C */ char pad[0x14];
} lbl_804C4540[3] __attribute__((aligned(32)));

/* 4D7720 */ static int HSD_Synth_804D7720;
/* 4D7724 */ static int hsd_SynthSFXBankNum;
/* 4D7728 */ static u32 hsd_SynthSFXBankAREnd;
/* 4D772C */ static volatile int HSD_Synth_804D772C;
/* 4D7730 */ static struct SfxLoadStreamNode* HSD_Synth_804D7730;
/* 4D7734 */ static DiscU32* HSD_Synth_804D7734;
/* 4D7738 */ static int HSD_Synth_804D7738;
/* 4D773C */ static volatile int sfxGroupDataReaddressCounter;
/* 4D7740 */ static void (*driverInactivatedCallback)(int);
/* 4D7744 */ static void (*driverMasterClockCallback)(int);
/* 4D7748 */ static void (*driverPauseCallback)(s32);
/* 4D774C */ static struct HSD_SynthSFXNode* HSD_Synth_804D774C;
/* 4D7750 */ static int HSD_Synth_804D7750;
/* 4D7754 */ static u32 HSD_Synth_804D7754; // sound mode
/* 4D7758 */ static u32 HSD_Synth_804D7758;
/* 4D7754 */ static int HSD_Synth_804D775C;
/* 4D7760 */ static int HSD_Synth_804D7760;
/* 4D7764 */ static s32 HSD_Synth_804D7764;
/* 4D7768 */ static u32 HSD_Synth_804D7768;
/* 4D776C */ static u32 HSD_Synth_804D776C;
/* 4D7770 */ static u32 HSD_Synth_804D7770;
/* 4D7774 */ static u32 HSD_Synth_804D7774;
/* 4D7778 */ static volatile u8 HSD_Synth_804D7778;
/* 4D777C */ static s32 HSD_Synth_804D777C;
/* 4D7780 */ static u32 HSD_Synth_804D7780;
/* 4D7784 */ static u32 HSD_Synth_804D7784;

#endif
