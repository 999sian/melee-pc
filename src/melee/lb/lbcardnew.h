#ifndef MELEE_LB_CARDNEW_H
#define MELEE_LB_CARDNEW_H

#include <Runtime/platform.h>

#include <melee/lb/forward.h>

#include <placeholder.h>

typedef enum {
    LbCardResult_Ready,
    LbCardResult_2 = 2,
    LbCardResult_NoFile = 4,
    LbCardResult_NullFilename = 7,
    LbCardResult_Malformed = 9,
    LbCardResult_10,
    LbCardResult_Busy,
    LbCardResult_BadSectorSize,
    LbCardResult_Invalid,
    LbCardResult_FatalError,
    LbCardResult_DeviceError,
    LbCardResult_16,
} lbCardResult;

typedef struct {
    int file_size;
    int file_flags;
    void* data;
} LbCardEntry;

typedef LbCardEntry CardEntry;

typedef void (*LbCardOnFinishedCallback)(int);

/* 01B6E0 */ s32 lb_8001B6E0(s32 file_idx);
/* 01B6F8 */ enum_t lbCardNew_CompleteNextTask(void);
/* 01B760 */ int lbCardNew_CompleteAllTasks(int result);
/* 01B7E0 */ u32 lb_8001B7E0(int chan, char* filename, void* file_entries,
                             void* save_data, UNK_T status_out);
/* 01B8C8 */ int lb_8001B8C8(int chan);
/* 01B99C */ int lbCardNew_DeleteSnap(int chan, const char* filename,
                                      UNK_T status_out);
/* 01BA44 */ int lb_8001BA44(int chan, const char* filename,
                             UNK_T status_out);
/* 01BB48 */ int lb_8001BB48(int chan, char* filename, void* file_entries,
                             void* save_data, char* comment, void* banner,
                             void* icons, UNK_T status_out);
/* 01BC18 */ int lb_8001BC18(int chan, char* filename, void** file_entries,
                             void* save_data, char* comment, void* banner,
                             void* icons, UNK_T status_out);
/* 01BD34 */ enum_t lb_8001BD34(int chan, const char* filename,
                                UNK_T file_entries, UNK_T status_out);
/* 01BE30 */ int lb_8001BE30(int chan, const char* filename,
                             UNK_T file_entries, char* comment, void* banner,
                             void* icons, UNK_T status_out, UNK_T callback);
/* 01BF04 */ int lb_8001BF04(int chan, char* filename, void* file_entries,
                             char* comment, void* banner, void* icons,
                             UNK_T status_out);
/* 01BFD8 */ int lb_8001BFD8(int chan,
                             lbCardNew_SnapshotEntry* snapshot_entries,
                             int* free_blocks, int* free_files);
/* 01C0F4 */ int lb_8001C0F4(int chan, const char* name_a, const char* name_b,
                             const char* name_c, UNK_T status_out);
/* 01C2D8 */ int lb_8001C2D8(int chan, const char* company,
                             const char* game_name, const char* filename);
/* 01C404 */ int lbCardNew_ProbeEx(int chan);
/* 01C4A8 */ int lb_8001C4A8(void* file_entries, void* icon_data);
/* 01C550 */ void lbCardNew_AllocWorkArea(void);
/* 01C5A4 */ void lbCardNew_ForgetMemory(void);
/* 01C5BC */ void lbCardNew_Init(void);

static inline enum_t lb_8001B6F8(void)
{
    return lbCardNew_CompleteNextTask();
}
static inline int lb_8001B760(int result)
{
    return lbCardNew_CompleteAllTasks(result);
}
static inline int lb_8001B99C(int chan, const char* filename, UNK_T status_out)
{
    return lbCardNew_DeleteSnap(chan, filename, status_out);
}
static inline int lb_8001C404(int chan)
{
    return lbCardNew_ProbeEx(chan);
}
static inline void lb_8001C5A4(void)
{
    lbCardNew_ForgetMemory();
}
static inline void lb_8001C5BC(void)
{
    lbCardNew_Init();
}

#endif
