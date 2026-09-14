#ifndef GALE01_3A949C
#define GALE01_3A949C

#include <Runtime/platform.h>

#include <placeholder.h>

#include <dolphin/card.h>

#ifdef TARGET_PC
#include "pc/disc.h"
#define PTR_TO_U32(p) pc_encode_dp((const void*) (uintptr_t) (p))
#define U32_TO_PTR(T, v) ((T) (uintptr_t) pc_resolve_dp((uint32_t) (v)))
#else
#define PTR_TO_U32(p) ((u32) (uintptr_t) (p))
#define U32_TO_PTR(T, v) ((T) (uintptr_t) (u32) (v))
#endif

/* Layout must match GameCube exactly (0x464 bytes): the driver addresses it
 * with word offsets and embeds it in game state. Pointer members are 32-bit
 * host-address slots. */
typedef struct CardFileData {
    u32 ptr; /* u8* */
} CardFileData;

typedef struct CardState {
    /* 0x00 */ u32 x0; /* u8* work buffer; see CARD_BUF */
    /* 0x04 */ s32 x4;
    /* 0x08 */ u32 x8;
    /* 0x0C */ CARDFileInfo file_info;
    /* 0x20 */ s32 x20;
    /* 0x24 */ u32 x24;
    /* 0x28 */ int x28[9];
    /* 0x4C */ int x4C[9];
    /* 0x70 */ CardFileData x70[9];
    /* 0x94 */ u8 pad_94[0xDC];
    /* 0x170 */ s32 x170[64];
    /* 0x270 */ s32 x270[64];
    /* 0x370 */ u8 x370[0x40];
    /// The banner format, icon formats and icon speeds are copied in and
    /// hashed as one 18-byte block.
    /* 0x3B0 */ union {
        u8 file_header[18];
        struct {
            u8 x3B0;
            u8 pad_3B1[1];
            u8 icon_format[8];
            u8 icon_speed[8];
        };
    };
    /* 0x3C2 */ u8 pad_3C2[2];
    /* 0x3C4 */ CARDStat stat;
    /* 0x430 */ u8 digest[0x30];
    /* 0x460 */ s32 x460;
} CardState;
STATIC_ASSERT(sizeof(CardState) == 0x464);
#define CARD_BUF(state) U32_TO_PTR(u8*, (state)->x0)

/* 3AA790 */ s32 fn_803AA790(void);
/* 3AAA48 */ void hsd_803AAA48(void);
/* 3AC168 */ s32 fn_803AC168(s32* cmd_buf);
/* 3AC258 */ s32 fn_803AC258(CardState* card_state, s32 block_idx);
/* 3AC2A4 */ s32 fn_803AC2A4(CardState* card_state);
/* 3AC2D4 */ UNK_RET fn_803AC2D4(UNK_PARAMS);
/* 3AC2E0 */ void fn_803AC2E0(void);
/* 3AC334 */ UNK_RET fn_803AC334(UNK_PARAMS);
/* 3AC340 */ int hsd_803AC340(void* header);
/* 3AC3E0 */ void hsd_803AC3E0(struct CardState* file_desc, int file_idx,
                               int file_size, int file_flags, u8* data);
/* 3AC3F8 */ void fn_803AC3F8(void*, u8*, s32);
/* 3AC558 */ void hsd_803AC558(struct CardState*, u8*);
/* 3AC634 */ u32 fn_803AC634(struct CardState* file_desc, s32 file_idx);
/* 3AC6B8 */ s32 fn_803AC6B8(struct CardState* file_desc, s32 file_count);
/* 3AC7DC */ s32 fn_803AC7DC(CardState*);
/* 3ACBE8 */ s32 fn_803ACBE8(CardState* state, s32 block_idx);
/* 3ACC0C */ s32 fn_803ACC0C(CardState* state, s32 block_idx, s32 file_id,
                             s32 seq_num, void* expected_data, s32 data_size);
/* 3ACD58 */ s32 fn_803ACD58(CardState* state, void* icon_data,
                             void* file_data);
/* 3ACF30 */ s32 fn_803ACF30(CardState* state, s32 file_id, s32 seq_num,
                             s32 version);
/* 3ACFC0 */ s32 fn_803ACFC0(CardState* state, s32 block_idx, s32 file_id,
                             s32 seq_num, void* payload, s32 payload_size,
                             s32 version);
/* 3AD16C */ s32 fn_803AD16C(CardState* state);
/* 3ADE4C */ s32 fn_803ADE4C(s32 card_state, s32 channel, s32 callback);
/* 3ADF90 */ s32 fn_803ADF90(struct CardState*, s32, u8*, s32,
                             void (*)(s32, s32));
/* 3AE7F8 */ s32 fn_803AE7F8(struct CardState*, s32, s32, s32, s32);
/* 3AF3F0 */ s32 fn_803AF3F0(CardState* state, s32, s32, s32, s32);
/* 3B0120 */ s32 fn_803B0120(CardState* state, s32, s32, s32, s32);
/* 3B0E9C */ s32 fn_803B0E9C(struct CardState*, s32, s32, s32, s32);
/* 3B1338 */ s32 fn_803B1338(CardState* state, s32);
/* 3B1F78 */ s32 fn_803B1F78(CardState* state, s32 channel, s32 file_id,
                             s32 seq_num, s32 callback);
/* 3B21E8 */ s32 fn_803B21E8(s32 card_state, s32 file_id, s32 seq_num,
                             s32 callback);
/* 3B2374 */ void hsd_803B2374(void);
/* 3B24E4 */ void hsd_803B24E4(s32* ctx, int channel, int file_no,
                               void* work_buf);
/* 3B2550 */ int hsd_803B2550(s32*, const char*, void (*)(int, int));
/* 3B2674 */ s32 hsd_803B2674(CardState* state);
/* 3B26CC */ s32 fn_803B26CC(CardState* state, s32 file_id, s32 seq_num,
                             s32 version, void (*callback)(s32, s32));
/// One contiguous 0x1510-byte card work area: CardContext head (0x10), 128
/// CardCmd slots (0x1200) and 32 HsdCmdEntry (0x300). The other two names
/// are views into it.
/* 4D1138 */ extern u8 hsd_804D1138[0x1510];
/* 4D1148 */ #define hsd_804D1148 ((u32(*)[9]) (hsd_804D1138 + 0x10))
/* 4D2348 */ #define hsd_804D2348 (*(u8(*)[0x300]) (hsd_804D1138 + 0x1210))
/* 4D2E70 */ extern u8 hsd_804D2E70[2084];
/* 4D7990 */ extern s32 hsd_804D7990;
/* 4D7994 */ extern s32 hsd_804D7994;
/* 4D79A0 */ extern u8* hsd_804D79A0;
/* 4D79A4 */ extern u8* hsd_804D79A4;
/* 4D79A8 */ extern s32 hsd_804D79A8;
/* 4D79AC */ extern s32 hsd_804D79AC;
/* 4D79B0 */ extern u8 hsd_804D79B0[8];
/* 4D79B8 */ extern u8* hsd_804D79B8;
/* 4D79BC */ extern u8* hsd_804D79BC;
/* 4D79C0 */ extern s32 hsd_804D79C0;
/* 4D79C4 */ extern s32 hsd_804D79C4;
/* 4D79C8 */ extern u8 hsd_804D79C8;

#endif
