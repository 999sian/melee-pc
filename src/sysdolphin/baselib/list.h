#ifndef _list_h_
#define _list_h_

#include <sysdolphin/baselib/objalloc.h>

typedef struct _HSD_SList {
    struct _HSD_SList* next;
    void* data;
} HSD_SList;

/* On-disc singly linked list (e.g. HSD_Joint.u.ptcl). */
typedef struct DISC_STRUCT _HSD_DiscSList {
    DISC_PTR(struct _HSD_DiscSList) next;
    u32 data;
} HSD_DiscSList;
DISC_ASSERT_SIZE(HSD_DiscSList, 0x8);

typedef struct _HSD_DList {
    struct _HSD_DList* next;
    struct _HSD_DList* prev;
    void* data;
} HSD_DList;

void HSD_ListInitAllocData(void);
HSD_ObjAllocData* HSD_SListGetAllocData(void);
HSD_ObjAllocData* HSD_DListGetAllocData(void);
HSD_SList* HSD_SListAlloc(void);
HSD_SList* HSD_SListAllocAndAppend(HSD_SList* next, void* data);
HSD_SList* HSD_SListAllocAndPrepend(HSD_SList* prev, void* data);
HSD_SList* HSD_SListAppendList(HSD_SList* list, HSD_SList* next);
HSD_SList* HSD_SListPrependList(HSD_SList* list, HSD_SList* prev);
HSD_SList* HSD_SListRemove(HSD_SList* list);

#endif
