/* Exercise the real VS Records name sort: it must fill the name table and
 * leave the MenMain*_Top descriptors that follow the fighter table alone. */
#define __assert libc_assert
#include <assert.h>
#undef __assert
#include <stdio.h>
#include <string.h>
#include "../src/melee/mn/mndiagram.c"

static struct NameTagData names[GM_NAMETAG_COUNT];
static char name_text[] = "NAME";

struct NameTagData* GetPersistentNameData(s32 slot) {
    return &names[slot];
}
/* Even slots hold a name; odd slots are empty. */
char* GetNameText(int slot) {
    return slot % 2 == 0 ? name_text : NULL;
}

static StaticModelDesc* const descs[] = {
    &MenMainCursorB1_Top,
    &MenMainSubB1_Top,
    &MenMainNmB_Top,
    &MenMainFaceB_Top,
    &MenMainConB1_Top,
    &MenMainCursorB3_Top,
    &MenMainConB3_Top,
    &MenMainConB2_Top,
};

int main(void) {
    enum { N = sizeof(descs) / sizeof(*descs) };
    StaticModelDesc saved[N];
    u8 fighters[sizeof(mnDiagram_FighterDisplayOrder)];
    u8 seen[GM_NAMETAG_COUNT] = {0};
    int i;

    for (i = 0; i < N; i++) {
        memset(descs[i], 0xA5, sizeof(*descs[i]));
        saved[i] = *descs[i];
    }
    for (i = 0; i < (int)sizeof(fighters); i++) {
        mnDiagram_FighterDisplayOrder[i] = fighters[i] = (u8)(0xC0 + i);
    }
    /* Named slot n scores n KOs against slot 0, so the highest even slot
     * ranks first. */
    for (i = 0; i < GM_NAMETAG_COUNT; i += 2) {
        names[i].vs_kos[0] = (u16)i;
    }

    mnDiagram_SortNamesByKOs();

    for (i = 0; i < N; i++) {
        assert(memcmp(descs[i], &saved[i], sizeof(saved[i])) == 0);
    }
    assert(memcmp(mnDiagram_FighterDisplayOrder, fighters, sizeof(fighters)) == 0);
    for (i = 0; i < GM_NAMETAG_COUNT; i++) {
        u8 n = mnDiagram_NameDisplayOrder[i];
        assert(n < GM_NAMETAG_COUNT && !seen[n]);
        seen[n] = 1;
    }
    for (i = 0; i < GM_NAMETAG_COUNT / 2; i++) {
        assert(mnDiagram_NameDisplayOrder[i] == GM_NAMETAG_COUNT - 2 - 2 * i);
    }
    puts("PASS: VS Records name sort fills its own table; menu descriptors untouched");
}
