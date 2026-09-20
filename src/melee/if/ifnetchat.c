#include "ifnetchat.h"
#ifdef TARGET_PC
#include "forward.h"
#include "pc/net_chat.h"
#include "pc/net.h"
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <string.h>
static HSD_Text* text;
static int entries[2];
static char last[2][192];
/* SIS owns the text's gobj. Preserve its destructor while clearing our
 * handle when scene teardown frees it; never retain a freed scene pointer. */
static void destroy(void* ptr) {
    if (text==ptr) { text=NULL; memset(last,0,sizeof last); }
    HSD_SisLib_803A5A2C(ptr);
}
/* preloadState resets the SIS pool/list without running entity destructors.
 * A VI wait may run us during that reset, and the new pool may reuse the old
 * address. Find the pointer in the current list before dereferencing it and
 * verify ownership rather than mistaking a new scene label for our text. */
static void validate_text(void) {
    if (!text) return;
    for (HSD_Text* current=HSD_SisLib_804D7978; current; current=current->next) {
        if (current==text && current->entity &&
            current->entity->user_data_remove_func==destroy) return;
    }
    text=NULL;
    memset(last,0,sizeof last);
}
void ifNetChat_Free(void) {
    validate_text();
    if (text) HSD_SisLib_803A5CC4(text);
}
void ifNetChat_Update(bool eligible) {
    validate_text();
    /* FontSymbol is cached metadata, not a loaded-font readiness check. */
    if (!HSD_SisLib_804D1124[0]) return;
    eligible=eligible && pc_net_active();
    if (!text && eligible) {
        int canvas=HSD_SisLib_803A611C(0,NULL,HSD_GOBJ_CLASS_UI,15,0,14,0,25);
        text=HSD_SisLib_803A6754(0,canvas);
        if (!text) return;
        text->default_kerning=1;
        if (text->entity) text->entity->user_data_remove_func=destroy;
        for(int i=0;i<2;i++) {
            entries[i]=HSD_SisLib_803A6B98(text,24.0f,430.0f+i*20.0f," ");
            HSD_SisLib_803A7548(text,entries[i],0.36f,0.36f);
        }
    }
    if (!text) return;
    const char* lines[2]={eligible?pc_net_chat_line():"",eligible?pc_net_chat_prompt():""};
    for(int i=0;i<2;i++) if(strcmp(last[i],lines[i])) {
        HSD_SisLib_803A70A0(text,entries[i],"%s",lines[i]);
        strncpy(last[i],lines[i],sizeof last[i]-1); last[i][sizeof last[i]-1]=0;
    }
}
#else
void ifNetChat_Update(bool eligible) { (void)eligible; }
void ifNetChat_Free(void) {}
#endif
