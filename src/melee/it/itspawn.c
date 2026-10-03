#include "itspawn.h"

#include <placeholder.h>

#include "it_26B1.h"
#include "it_2725.h"
#include "it_3F14.h"
#include "item.h"
#include <melee/db/db.h>
#include <melee/ef/efsync.h>
#include <melee/gm/gm_unsplit.h>
#include <melee/gr/ground.h>
#include <melee/gr/stage.h>
#include <melee/mp/mpcoll.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/random.h>
#ifdef TARGET_PC
#include "pc/mods/items.h"
#include "pc/pc.h"
#include <melee/ft/ftlib.h>
#include <melee/ft/inlines.h>
#include "inlines.h"
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/types.h>
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#include <melee/lb/lbarchive.h>
#include <sysdolphin/baselib/archive.h>
#include <string.h>
#endif

ItemPickTable it_804A0E60;
ItemPickTable it_804A0E50;
RandomItemSpawner it_804A0E30;

#ifdef TARGET_PC
/* ---- PC: mod item packs (pc/mods/items.h) ---------------------------------
 *
 * Each match loads the packs' Articles, then adds a draw entry per pack next
 * to its base's in the two random tables (ambient spawns, container
 * contents), weighted relative to the base. The table entry a pick lands on
 * says which pack it is; the spawn that follows takes it. All of this is
 * game memory, so rollback snapshots carry it like the tables themselves. */
#define IT_PC_MAX_PACKS 64
static Article* it_pc_articles[IT_PC_MAX_PACKS];
static s8 it_pc_spawner_packs[256];
static s8 it_pc_drop_packs[256];
static s32 it_pc_pick_kind = -1;
static s32 it_pc_pick_pack = -1;
static u64 it_pc_spawn_logged; /* packs whose first spawn this match was logged */

static s8* it_PcTablePacks(ItemPickTable* table)
{
    if (table == &it_804A0E30.x4) {
        return it_pc_spawner_packs;
    }
    if (table == &it_804A0E50) {
        return it_pc_drop_packs;
    }
    return NULL;
}

u8 it_PcTakePack(ItemKind kind)
{
    u8 pack = 0;
    if (it_pc_pick_kind == (s32) kind && it_pc_pick_pack >= 0) {
        pack = (u8) (it_pc_pick_pack + 1);
    }
    it_pc_pick_kind = it_pc_pick_pack = -1;
    return pack;
}

void it_PcSetPick(ItemKind kind, u8 pack)
{
    it_pc_pick_kind = kind;
    it_pc_pick_pack = (s32) pack - 1;
}

int it_PcPackCount(void)
{
    int n = pc_items_count();
    return n < IT_PC_MAX_PACKS ? n : IT_PC_MAX_PACKS;
}

s32 it_PcPackWeight(int pack, ItemKind kind, s32 base_weight)
{
    if (pack < 0 || pack >= it_PcPackCount() || it_pc_articles[pack] == NULL ||
        pc_items_base(pack) != (int) kind)
    {
        return 0;
    }
    return (s32) (base_weight * pc_items_frequency(pack) + 0.5f);
}

/* The fighter behind a spawn: the parent itself, or the owner of the item
 * that spawned it (a bomb's explosion, a Pokemon's attack). */
static HSD_GObj* it_PcSpawnFighter(HSD_GObj* parent)
{
    int depth;
    for (depth = 0; parent != NULL && depth < 4; depth++) {
        if (ftLib_IsFighter(parent)) {
            return parent;
        }
        if (parent->classifier != HSD_GOBJ_CLASS_ITEM) {
            return NULL;
        }
        parent = GET_ITEM(parent)->owner;
    }
    return NULL;
}

static bool it_PcOwnedMatches(int p, HSD_GObj* fighter)
{
    if (pc_items_owner_pack(p) >= 0 || pc_items_owner_kind(p) >= 0) {
        Fighter* fp;
        if (fighter == NULL) {
            return false;
        }
        fp = GET_FIGHTER(fighter);
        if (pc_items_owner_pack(p) >= 0 &&
            (int) Player_GetPack(fp->player_idx) - 1 != pc_items_owner_pack(p))
        {
            return false;
        }
        if (pc_items_owner_kind(p) >= 0 &&
            (int) fp->kind != pc_items_owner_kind(p))
        {
            return false;
        }
    }
    if (pc_items_stage_kind(p) >= 0 &&
        (int) Stage_80225194() != pc_items_stage_kind(p))
    {
        return false;
    }
    if (pc_items_stage_pack(p) >= 0 &&
        (int) gm_GetStartMeleeRules()->pc_stage_pack - 1 !=
            pc_items_stage_pack(p))
    {
        return false;
    }
    return true;
}

/* Each matching pack weighs `frequency` against the base's 1: frequency 1 is
 * half the spawns, 100 nearly all. No candidate, no roll, so a match
 * without such packs draws exactly what the game would. */
u8 it_PcOwnedPack(ItemKind kind, HSD_GObj* parent)
{
    s32 weights[IT_PC_MAX_PACKS];
    s32 total = 100;
    HSD_GObj* fighter = it_PcSpawnFighter(parent);
    int p;
    s32 r;
    for (p = 0; p < it_PcPackCount(); p++) {
        weights[p] = 0;
        if (it_pc_articles[p] == NULL || pc_items_base(p) != (int) kind ||
            pc_item_base_class(kind) != PC_ITEM_OWNED ||
            !it_PcOwnedMatches(p, fighter))
        {
            continue;
        }
        weights[p] = (s32) (pc_items_frequency(p) * 100.0f + 0.5f);
        total += weights[p];
    }
    if (total == 100) {
        return 0;
    }
    r = HSD_Randi(total) - 100;
    for (p = 0; p < it_PcPackCount(); p++) {
        if (r < weights[p]) {
            return r < 0 ? 0 : (u8) (p + 1);
        }
        r -= weights[p];
    }
    return 0;
}

void it_PcNoteSpawn(u8 pack)
{
    if (pack == 0 || pack > IT_PC_MAX_PACKS ||
        (it_pc_spawn_logged & (1ULL << (pack - 1))))
    {
        return;
    }
    it_pc_spawn_logged |= 1ULL << (pack - 1);
    pc_log_line("mods: item %s spawned (first this match)", pc_items_id(pack - 1));
}

Article* it_PcPackArticle(u8 pack)
{
    if (pack == 0 || pack > IT_PC_MAX_PACKS) {
        return NULL;
    }
    return it_pc_articles[pack - 1];
}

/* The packs' files load into the scene heap with the rest of the match. */
static void it_PcLoadPacks(void)
{
    static bool reported[IT_PC_MAX_PACKS];
    int n = pc_items_count();
    int i;
    memset(it_pc_articles, 0, sizeof(it_pc_articles));
    memset(it_pc_spawner_packs, 0xFF, sizeof(it_pc_spawner_packs));
    memset(it_pc_drop_packs, 0xFF, sizeof(it_pc_drop_packs));
    it_pc_pick_kind = it_pc_pick_pack = -1;
    it_pc_spawn_logged = 0;
    for (i = 0; i < n && i < IT_PC_MAX_PACKS; i++) {
        HSD_Archive* archive = NULL;
        lbArchive_80016F80(&archive, pc_items_file(i));
        if (archive != NULL) {
            it_pc_articles[i] =
                HSD_ArchiveGetPublicAddress(archive, pc_items_symbol(i));
        }
        if (!reported[i]) {
            reported[i] = true;
            if (it_pc_articles[i] == NULL) {
                pc_log_line("mods: item %s: no symbol \"%s\" in %s; it will not spawn",
                            pc_items_id(i), pc_items_symbol(i), pc_items_file(i));
            }
        }
    }
}

/* Rebuilds @p table with an entry for every loaded pack right after its
 * base's (so a Poke Ball entry stays last, which it_8026C75C relies on). */
static void it_PcAddPacks(ItemPickTable* table, s8* packs)
{
    u8 kinds[256];
    u16 weights[256];
    s8 owner[256];
    int n = 0;
    int i, p;
    u32 total = 0;
    int base_count = table->size;
    if (base_count == 0 || table->x8 == 0) {
        return;
    }
    for (i = 0; i < base_count && n < 255; i++) {
        u32 end = i + 1 < base_count ? table->xC[i + 1] : table->x8;
        u32 w = end - table->xC[i];
        kinds[n] = table->x4[i];
        weights[n] = (u16) w;
        owner[n++] = -1;
        for (p = 0; p < pc_items_count() && p < IT_PC_MAX_PACKS && n < 255;
             p++)
        {
            u32 pw;
            if (it_pc_articles[p] == NULL || pc_items_base(p) != table->x4[i]) {
                continue;
            }
            pw = (u32) (w * pc_items_frequency(p) + 0.5f);
            if (pw == 0) {
                continue;
            }
            kinds[n] = table->x4[i];
            weights[n] = (u16) pw;
            owner[n++] = (s8) p;
        }
    }
    if (n == base_count) {
        return; /* no pack on any of this table's items */
    }
    table->x4 = HSD_MemAlloc(n * 4);
    table->xC = HSD_MemAlloc(n * 4);
    for (i = 0; i < n; i++) {
        table->x4[i] = kinds[i];
        table->xC[i] = (u16) total;
        packs[i] = owner[i];
        total += weights[i];
    }
    table->size = (u8) n;
    table->x8 = (u16) (total > 0xFFFF ? 0xFFFF : total);
}
#endif

/// @todo .sdata2 order hack
#ifdef MUST_MATCH
static void sdata2_order(void)
{
    (void) S32_TO_F32;
    (void) 0.0F;
    (void) 0.99F;
    (void) it_804A0E30;
    (void) it_804A0E50;
}
#endif

void it_8026C47C(struct it_8026C47C_arg0_t* arg_struct)
{
    u32 it_kind;
    u32 unused;
    s32 bit_idx;
    s32* word;
    PAD_STACK(8);

    it_kind = (unused = It_Kind_Capsule);
    bit_idx = 0;
    word = &arg_struct->unk0;
    arg_struct->unk0 = 0;
    arg_struct->unk4 = 0;
    arg_struct->unk8 = 0;
    arg_struct->unkC = 0;
    arg_struct->unk10 = 0;
    arg_struct->unk14 = 0;
    arg_struct->unk18 = 0;
    arg_struct->unk1C = 0;
    while (it_kind < 238) {
        if (it_80272828(it_kind)) {
            *word |= 1 << bit_idx;
        }
        it_kind++;
        bit_idx++;
        if (!(it_kind & It_Kind_RabbitC)) {
            bit_idx = 0;
            word++;
        }
    }
}

/// bisection search in the item table for a given value
static int bisectValue(int val, ItemPickTable* table, int lo, int hi)
{
    int mid;

    // base case, done bisecting
    if (lo == hi - 1) {
        return lo;
    }

    mid = (lo + hi) / 2;
    if (table->xC[mid] > val) {
        // recurse into lower half
        return bisectValue(val, table, lo, mid);
    } else {
        if (table->xC[mid + 1] > val) {
            return mid;
        }
        // recurse into upper half
        return bisectValue(val, table, mid, hi);
    }
}

ItemKind it_8026C65C(ItemPickTable* table)
{
    int temp_r6 = table->x8;
#ifdef TARGET_PC
    int i = bisectValue(HSD_Randi(temp_r6), table, 0, table->size);
    s8* packs = it_PcTablePacks(table);
    it_pc_pick_kind = table->x4[i];
    it_pc_pick_pack = packs != NULL ? packs[i] : -1;
    return table->x4[i];
#else
    return table->x4[bisectValue(HSD_Randi(temp_r6), table, 0, table->size)];
#endif
}

bool it_8026C704(void)
{
    bool result = false;
    if (Item_804A0C64.x1C >= Item_804A0C64.x20 || !it_8026D324(It_Kind_M_Ball))
    {
        result = true;
    }
    return result;
}

/// Decides item kind for spawned items - not sure in which context (i.e from
/// pokeballs, from capsules, thin air, etc.)
ItemKind it_8026C75C(ItemPickTable* table)
{
    bool chk1;
    int saved;
    bool chk2;
    ItemKind ret;
    ItemKind kind;
    ItemPickTable* tbl = table;
    PAD_STACK(16);

    chk1 = false;
    if (Item_804A0C64.x1C >= Item_804A0C64.x20 || !it_8026D324(It_Kind_M_Ball))
    {
        chk1 = true;
    }
    chk2 = false;
    if (tbl->x8 == 0) {
        return -1;
    }
    if (chk1) {
        if (tbl->x4[tbl->size - 1] == It_Kind_M_Ball) {
            int i_last = tbl->size - 1;
            if (i_last < 1) {
                return -1;
            }
            saved = tbl->x8;
            chk2 = true;
            tbl->x8 = tbl->xC[i_last];
            tbl->size--;
        }
    }
    kind = it_8026C65C(tbl);

    ret = kind;
    if (chk1 && chk2) {
        tbl->x8 = saved;
        tbl->size++;
        if (kind == It_Kind_M_Ball) {
            ret = -1;
        }
    }
    return ret;
}

static inline void it_8026C88C_inline(RandomItemSpawner* alloc)
{
    Vec3* pos;
    s32 chk;
    Item_GObj* spawn_gobj;
    SpawnItem spawn;
    if (db_AreItemSpawnsEnabled() != 0U) {
        alloc->x0--;
        if (alloc->x0 == 0) {
            spawn.kind = it_8026C75C(&alloc->x4);
            if ((s32) spawn.kind != -1) {
                pos = &spawn.prev_pos;
                if (it_8026CB3C(pos)) {
                    spawn.pos = *pos;
                    spawn.facing_dir = it_8026B684(pos);
                    chk = 1;
                    spawn.x3C_damage = 0;
                    spawn.vel.x = spawn.vel.y = spawn.vel.z = 0.0F;
                    spawn.x0_parent_gobj = NULL;
                    spawn.x4_parent_gobj2 = spawn.x0_parent_gobj;
                    spawn.x44_flag.b0 = chk;
                    spawn.x40 = 0;
                } else {
                    chk = false;
                }
                if (chk) {
                    spawn_gobj = Item_80268B18(&spawn);
                    if (spawn_gobj != NULL) {
                        efSync_Spawn(0x420, spawn_gobj, pos);
                        it_80274ED8();
                    }
                }
            } ///< @todo Make a FLT_RAND(min, max) define or inline
            {
                s32 range[2] = { it_804D6D28->xFC[gm_8016AE80() * 2],
                                 it_804D6D28->xFC[gm_8016AE80() * 2 + 1] };
                f32 randf = HSD_Randf();
                f32 diff = range[1] - range[0];
                alloc->x0 = diff * randf + range[0];
                alloc->x0 *= Ground_801C2AE8(Stage_80225194());
            }
        }
    }
}

void fn_8026C88C(HSD_GObj* gobj)
{
    RandomItemSpawner* alloc = &it_804A0E30;
    it_8026C88C_inline(alloc);
}

void it_8026CA4C(ItemPickTable* alloc, s32* arg1, u64 arg2, s32 arg3, f32 arg4)
{
    u64 mask = arg2;
    s32* p = arg1 + arg3;
    s32 i = arg3;
    s32 sum = 0;

    while (i < It_Kind_L_Gun_Ray) {
        if (mask & 1) {
            sum += arg4 * *p + 0.99f;
        }
        p++;
        i++;
        mask >>= 1;
    }
    alloc->x8 = sum;
}

bool it_8026CB3C(Vec3* vec)
{
    if (!Stage_80224FDC(vec)) {
        return false;
    }
    vec->z = 0.0f;
    if (mpColl_8004D024(vec)) {
        return false;
    }
    return true;
}

/// Builds some structs for items
void it_8026CB9C(s32* counts, u64 mask, f32 weight)
{
    RandomItemSpawner* spawner = &it_804A0E30;
    u8** item_kinds;
    u16** weights;
    s32* p;
    s32 cnt;
    ItemKind it_kind;
    s32 cnt2;
    ItemKind it_kind2;
    s32* p2;
    s32 cumulative;
    s32 idx;
    u64 backup;

    backup = mask;
    p = counts;
    it_kind = 0;
    cnt = 0;
    while (it_kind < It_Kind_L_Gun_Ray) {
        if ((mask & 1) && *p != 0) {
            cnt++;
        }
        p++;
        it_kind++;
        mask >>= 1;
    }
    spawner->x4.size = cnt;
    *(item_kinds = &spawner->x4.x4) = HSD_MemAlloc(cnt * 4);
    *(weights = &spawner->x4.xC) = HSD_MemAlloc(cnt * 4);

    idx = (cnt2 = 0);
    mask = backup;
    p2 = counts;
    it_kind2 = 0;
    cumulative = 0;
    while (it_kind2 < It_Kind_L_Gun_Ray) {
        if ((mask & 1) && *p2 != 0) {
            (*item_kinds)[cnt2] = it_kind2;
            (*weights)[idx] = cumulative;
            cnt2++;
            idx++;
            cumulative = (cumulative + ((weight * *p2) + 0.99f));
        }
        p2++;
        it_kind2++;
        mask >>= 1;
    }
}

void it_8026CD50(s32* counts, u64 mask, f32 weight)
{
    /* The original reached it_804A0E50 by walking past it_804A0E30. */
    ItemPickTable* tbl = &it_804A0E50;
    s32* p;
    s32 cnt;
    ItemKind it_kind;
    ItemKind it_kind2;
    s32 cnt2;
    s32* p2;
    u8** item_kinds;
    s32 idx;
    u16** weights;
    s32 cumulative;
    u64 backup;

    backup = mask;
    p = counts + It_Kind_BombHei;
    cnt = 0;
    it_kind = It_Kind_BombHei;
    while (it_kind < It_Kind_L_Gun_Ray) {
        if ((mask & 1) && *p != 0) {
            cnt++;
        }
        p++;
        it_kind++;
        mask >>= 1;
    }
    tbl->size = cnt;
    *(item_kinds = &tbl->x4) = HSD_MemAlloc(cnt * 4);
    *(weights = &tbl->xC) = HSD_MemAlloc(cnt * 4);

    idx = (cnt2 = 0);
    mask = backup;
    p2 = counts + It_Kind_BombHei;
    cumulative = 0;
    it_kind2 = It_Kind_BombHei;
    while (it_kind2 < It_Kind_L_Gun_Ray) {
        if ((mask & 1) && *p2 != 0) {
            (*item_kinds)[cnt2] = it_kind2;
            (*weights)[idx] = cumulative;
            cnt2++;
            idx++;
            cumulative = (cumulative + ((weight * *p2) + 0.99f));
        }
        p2++;
        it_kind2++;
        mask >>= 1;
    }
}

/// Builds the monster-item weighted-pick table (it_804A0E60)
void it_8026CF04(void)
{
    ItemCommonData* item_common;
    s32 sum;
    int i;
    u32 cumulative;
    u32 idx;

    sum = it_804D6D28->x128[0];
    sum += it_804D6D28->x128[1];
    sum += it_804D6D28->x128[2];
    sum += it_804D6D28->x128[3];
    if (sum != 0) {
        it_804A0E60.x8 = sum;
        it_804A0E60.size = 4;
        it_804A0E60.x4 = HSD_MemAlloc(it_804A0E60.size * 4);
        it_804A0E60.xC = HSD_MemAlloc(it_804A0E60.size * 4);
        idx = i = 0;
        item_common = it_804D6D28;
        cumulative = 0;
        for (; i < 4; i++, idx++) {
            it_804A0E60.x4[i] = It_Kind_Kuriboh + i;
            it_804A0E60.xC[idx] = cumulative;
            cumulative += item_common->x128[idx];
        }
    }
}

static inline bool it_8026D018_inline(void)
{
    s32* stage_info;
    u64 stage_mask;
    int chk;
    f32 weight;

    stage_mask = it_804A0E30.x18;
    stage_info = Ground_801C2AD8();
    chk = gm_8016AE80();
    weight = gm_8016AE94();

    if (stage_mask == 0 || stage_info == NULL || chk == -1) {
        return false;
    }
    it_8026CA4C(&it_804A0E30.x4, stage_info, stage_mask, 0, weight);
    if (it_804A0E30.x4.x8 == 0) {
        return false;
    }
    it_8026CB9C(stage_info, stage_mask, weight);
    return true;
}

static inline void it_8026D018_inline2(void)
{
    s32* stage_info;
    u64 stage_mask;
    f32 weight;

    stage_mask = it_804A0E30.x18;
    stage_info = Ground_801C2AD8();
    weight = gm_8016AE94();

    if ((stage_mask != 0) && (stage_info != NULL)) {
        stage_mask >>= 6;
        it_8026CA4C(&it_804A0E50, stage_info, stage_mask, 6, weight);
        if (it_804A0E50.x8 != 0) {
            it_8026CD50(stage_info, stage_mask, weight);
        }
    }
}

static inline void it_8026D018_inline3(f32 randf, const s32* range)
{
    s32 diff = range[1] - range[0];

    (void) diff;
    it_804A0E30.x0 = diff * randf + range[0];
    it_804A0E30.x0 *= Ground_801C2AE8(Stage_80225194());
}

void it_8026D018(void)
{
#ifdef TARGET_PC
    /* Every match, items on or not: Poke Balls and Training's item menu
     * spawn packs outside the random draw too. */
    it_PcLoadPacks();
#endif
    if (!gm_8016B238() && (gm_8016AE80() != -1)) {
        it_804A0E30.x18 = gm_8016AEA4();
        if (it_8026D018_inline()) {
            it_8026D018_inline2();
#ifdef TARGET_PC
            it_PcAddPacks(&it_804A0E30.x4, it_pc_spawner_packs);
            it_PcAddPacks(&it_804A0E50, it_pc_drop_packs);
#endif
            it_8026CF04();
            HSD_GObj_SetupProc(GObj_Create(5, 7, 0), fn_8026C88C, 0);
            {
                s32 range[2] = { it_804D6D28->xFC[gm_8016AE80() * 2],
                                 it_804D6D28->xFC[gm_8016AE80() * 2 + 1] };
                it_8026D018_inline3(HSD_Randf(), range);
            }
        }
    }
}

/// Spawn item of specified kind at specified position (but no z-offset)
bool it_8026D258(Vec3* pos, ItemKind kind)
{
    SpawnItem spawn;
    bool item_spawn_chk;

    item_spawn_chk = false;
    if (Item_804A0C64.x60 < Item_804A0C64.x64) {
        spawn.kind = kind;
        spawn.prev_pos = *pos;
        spawn.prev_pos.z = 0.0f;
        spawn.pos = spawn.prev_pos;
        spawn.facing_dir = it_8026B684(&spawn.prev_pos);
        spawn.x3C_damage = 0;
        spawn.vel.z = 0.0f;
        spawn.vel.y = 0.0f;
        spawn.vel.x = 0.0f;
        spawn.x0_parent_gobj = NULL;
        spawn.x4_parent_gobj2 = spawn.x0_parent_gobj;
        spawn.x44_flag.b0 = 1;
        spawn.x40 = 0;
        Item_80268B9C(&spawn);
        item_spawn_chk = true;
    }
    return item_spawn_chk;
}

bool it_8026D324(ItemKind kind)
{
    u64 temp_r29 = it_804A0E30.x18;
    s32* temp_r30 = Ground_801C2AD8();
    s32 temp_r3 = gm_8016AE80();
    if (temp_r29 == 0 || temp_r30 == NULL || temp_r3 == -1) {
        return false;
    }
    if (!(temp_r29 >> kind & 1)) {
        return false;
    }
    return true;
}

bool it_8026D3CC(void)
{
    bool result = it_8026D324(It_Kind_Heart);

    result |= it_8026D324(It_Kind_Tomato);
    result |= it_8026D324(It_Kind_Foods);
    return result;
}
