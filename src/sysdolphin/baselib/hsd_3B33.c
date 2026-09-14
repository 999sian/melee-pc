#include "hsd_3B33.h"

#include <setjmp.h> // IWYU pragma: keep
#include <string.h>

#include "hsd_3A94.h"
#include "hsd_3B34.h"

extern JpegWork hsd_804D2648;

void hsd_803B3344(u8 byte)
{
    u8* temp_r5;

    temp_r5 = hsd_804D79A0;
    if (temp_r5 < &hsd_804D79A4[hsd_804D79A8]) {
        hsd_804D79A0 = temp_r5 + 1;
        *temp_r5 = byte;
        return;
    }

    longjmp(hsd_804D2648.buf, true);
}

void hsd_803B3398(void* src, size_t size)
{
    void* temp_r3 = hsd_804D79A0;

    if ((u8*) temp_r3 < &hsd_804D79A4[hsd_804D79A8] - size) {
        memcpy(temp_r3, src, size);
        /* hsd_804D79A0 is a pointer: advancing it through a u32 lvalue only
         * writes half of it on LP64 and breaks strict aliasing. */
        hsd_804D79A0 += size;
        return;
    }

    longjmp(hsd_804D2648.buf, true);
}
