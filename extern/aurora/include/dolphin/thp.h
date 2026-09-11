#ifndef _DOLPHIN_THP_H_
#define _DOLPHIN_THP_H_

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL THPInit(void);
s32 THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work /* unused */);
u32 THPAudioDecode(s16* audioBuffer, const u8* audioFrame, s32 flag);

#ifdef __cplusplus
}
#endif

#endif // _DOLPHIN_THP_H_
