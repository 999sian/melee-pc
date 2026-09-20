/* Lifecycle regression: SIS scene reset bypasses entity destructors. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/melee/if/ifnetchat.c"
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "failed line %d: %s\n", __LINE__, #x);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
HSD_Text* HSD_SisLib_804D7978;
SIS* HSD_SisLib_804D1124[5];
static HSD_Text slots[4];
static HSD_GObj objects[4];
static unsigned creates, updates, frees;
bool pc_net_active(void) {
    return true;
}
const char* pc_net_chat_line(void) {
    return "Opponent: Hello";
}
const char* pc_net_chat_prompt(void) {
    return "CHAT";
}
int HSD_SisLib_803A611C(int a, HSD_GObj* b, u16 c, u8 d, u8 e, u8 f, u8 g, u32 h) {
    return 0;
}
HSD_Text* HSD_SisLib_803A6754(int a, int b) {
    CHECK(creates < 4);
    HSD_Text* t = &slots[creates];
    memset(t, 0, sizeof *t);
    t->entity = &objects[creates++];
    t->next = HSD_SisLib_804D7978;
    HSD_SisLib_804D7978 = t;
    return t;
}
int HSD_SisLib_803A6B98(HSD_Text* t, float x, float y, const char* fmt, ...) {
    return 0;
}
void HSD_SisLib_803A7548(HSD_Text* t, int e, float x, float y) {}
s32 HSD_SisLib_803A70A0(HSD_Text* t, s32 e, char* fmt, ...) {
    CHECK(t == text && t->entity->user_data_remove_func == destroy);
    updates++;
    return 0;
}
void HSD_SisLib_803A5A2C(void* ptr) {
    frees++;
    HSD_SisLib_804D7978 = NULL;
}
void HSD_SisLib_803A5CC4(HSD_Text* t) {
    t->entity->user_data_remove_func(t);
}
int main(void) {
    ifNetChat_Update(true);
    CHECK(creates == 0); /* pool/font not loaded */
    HSD_SisLib_804D1124[0] = (SIS*)1;
    ifNetChat_Update(true);
    CHECK(creates == 1 && updates == 2);
    /* Pool reset: callback never ran; stale pointer is deliberately invalid. */
    text = (HSD_Text*)1;
    HSD_SisLib_804D7978 = NULL;
    HSD_SisLib_804D1124[0] = NULL;
    ifNetChat_Update(true);
    CHECK(text == NULL && updates == 2);
    HSD_SisLib_804D1124[0] = (SIS*)1;
    ifNetChat_Update(true);
    CHECK(creates == 2 && updates == 4);
    /* New scene reuses the address for an unrelated label. */
    text->entity->user_data_remove_func = HSD_SisLib_803A5A2C;
    ifNetChat_Update(true);
    CHECK(creates == 3 && updates == 6);
    ifNetChat_Free();
    CHECK(text == NULL && frees == 1);
    ifNetChat_Update(true);
    CHECK(creates == 4 && updates == 8);
    destroy(text);
    CHECK(text == NULL && frees == 2);
    puts(
        "PASS: unloaded font, SIS reset without callbacks, reused text address, ordinary teardown");
}
