
/* emitter: rate + burst emission with lifetime expiry and capacity.
 *
 * 槽位字段（含 color）的唯一清单与“槽位完整性判据”见 particle_slots.h。
 * 复用槽位必须经 em_spawn_ex()->particle_spawn_reset() 整槽覆写。
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "particle_slots.h"

#define MAXP 64

typedef Particle P;
typedef struct { P p[MAXP]; int n; float rate_acc; int emitted; int dropped; } Emitter;

static void em_init(Emitter* e) { memset(e, 0, sizeof(*e)); }

/* 合法出生调色板：全部在 0xFF...... 段；测试用的“已死颜色毒值”在
 * 0xDEAD.... 段，两个区间互不相交，保证相同只可能是残留、不会巧合相等。 */
static uint32_t em_palette_color(int index) {
    static const uint32_t palette[] = {
        0xFF3B82F6u, /* blue   */
        0xFFF59E0Bu, /* amber  */
        0xFF10B981u, /* green  */
        0xFFEF4444u  /* red    */
    };
    return palette[(unsigned)index % (sizeof(palette) / sizeof(palette[0]))];
}

/* 槽位复用的唯一完整入口：清单驱动的整槽覆写。 */
static int em_spawn_ex(Emitter* e, const ParticleSpawn* sp) {
    if (sp->life <= 0.0f) return -1;
    int slot = -1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) { slot = i; break; }
    }
    if (slot < 0) {
        if (e->n >= MAXP) { e->dropped++; return -2; }
        slot = e->n++;
    }
    e->p[slot] = particle_spawn_reset(sp);
    e->emitted++;
    return slot;
}

/* 旧签名包装：自动分配一个调色板出生色。 */
static int em_spawn(Emitter* e, float x, float y, float vx, float vy, float life) {
    ParticleSpawn sp = { x, y, vx, vy, life, em_palette_color(e->emitted) };
    return em_spawn_ex(e, &sp);
}

/* 寿命/运动推进（一帧的前半段）。
 * 返回本帧最后一个死亡粒子的槽位（即“同帧内上一颗已死粒子”），无死亡返回 -1。
 * DEATH 步骤只把 alive 置 0；color 等表现字段保留临终值，供截帧比对，
 * 它们会在下一次 SPAWN 时被整槽覆写。 */
static int em_age(Emitter* e, float dt) {
    int last_dead = -1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        e->p[i].age += dt;
        if (e->p[i].age >= e->p[i].life) {
            e->p[i].alive = 0; /* PRS_DEATH */
            last_dead = i;
            continue;
        }
        e->p[i].x += e->p[i].vx * dt;
        e->p[i].y += e->p[i].vy * dt;
    }
    return last_dead;
}

static int em_update(Emitter* e, float dt, float rate, int burst) {
    if (dt < 0.0f) return -1;
    /* 先推进已有粒子，再发射新粒子，避免新粒子在本帧就被计满寿命 */
    em_age(e, dt);
    for (int b = 0; b < burst; b++) em_spawn(e, 0, 0, 0, 1.0f, 1.0f);
    e->rate_acc += rate * dt;
    int want = (int)e->rate_acc;
    e->rate_acc -= (float)want;
    for (int i = 0; i < want; i++) em_spawn(e, 0, 0, 0, 1.0f, 1.0f);
    int alive = 0;
    for (int i = 0; i < e->n; i++) if (e->p[i].alive) alive++;
    return alive;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* ===========================================================================
 * 槽位完整性：运行时核对描述符表（编译期卡口在 particle_slots.h 内）
 * =========================================================================== */
static int test_slot_manifest_complete(void) {
    int ok = 1;
    size_t last_end = 0;
    for (int i = 0; i < PARTICLE_FIELD_COUNT; i++) {
        const ParticleFieldDescriptor* d = &particle_field_descriptors[i];
        if (d->size == 0) ok = 0;
        if (d->offset + d->size > sizeof(Particle)) ok = 0;       /* 字段越界 */
        if (i > 0 && d->offset < particle_field_descriptors[i - 1].offset) ok = 0;
        if (d->reset_step < PRS_INIT || d->reset_step > PRS_TICK) ok = 0;
        /* 清单字段按声明顺序排布，区间不得与前一字段重叠（允许填充空隙） */
        if (i > 0 && d->offset < last_end) ok = 0;
        last_end = d->offset + d->size;
    }
    if (last_end > sizeof(Particle)) ok = 0;
    return ok;
}

/* ===========================================================================
 * 连续 20 帧颜色残留检测
 *
 * 每帧编排（确定性地制造一次“同帧死亡 -> 同槽新生”）：
 *   1. 给当前槽位粒子染上该帧专属“临终颜色” 0xDEAD0001..0xDEAD0014；
 *   2. em_age() 让它寿命到期，记录返回槽位的临终颜色 —— 同帧内上一颗已死粒子；
 *   3. 发射一颗新生粒子，扫描死槽时必定复用同一槽位；
 *   4. 比较新生颜色与步骤 2 记录的已死颜色，完全相等即一对残留。
 *
 * respawn 可替换：正式入口（清单整槽重置）应得到 0 对残留；
 * 传入“旧版逐字段重置、漏掉 color”的标本时应得到整整 20 对，证明检测有牙。
 * =========================================================================== */
typedef int (*RespawnFn)(Emitter* e, const ParticleSpawn* sp);

static int respawn_manifest(Emitter* e, const ParticleSpawn* sp) {
    return em_spawn_ex(e, sp);
}

/* 反面标本：复现当年“逐个清字段”的手写重置 —— 唯独漏了 color。 */
static int respawn_legacy_field_by_field(Emitter* e, const ParticleSpawn* sp) {
    int slot = -1;
    for (int i = 0; i < e->n; i++) if (!e->p[i].alive) { slot = i; break; }
    if (slot < 0) {
        if (e->n >= MAXP) return -2;
        slot = e->n++;
    }
    P* p = &e->p[slot];
    p->x = sp->x;
    p->y = sp->y;
    p->vx = sp->vx;
    p->vy = sp->vy;
    p->age = 0.0f;
    p->life = sp->life;
    /* 当年这里没有 p->color = sp->color; —— 颜色于是保留上一颗的值 */
    p->alive = 1;
    e->emitted++;
    return slot;
}

static uint32_t frame_dead_color(int frame) {
    return 0xDEAD0000u | (uint32_t)(frame + 1);
}

/* 返回检测到的残留对数；*scene_ok 报告“每帧确实同槽死亡+新生”的编排前提。
 * verbose 时按仓库 check 命名格式逐帧打印 residue_frame_NN=1/0。 */
static int run_color_residue_frames(RespawnFn respawn, int verbose, int* scene_ok) {
    enum { FRAMES = 20 };
    Emitter e;
    em_init(&e);
    *scene_ok = 1;

    ParticleSpawn first = { 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, em_palette_color(0) };
    int slot = respawn(&e, &first);
    if (slot != 0) *scene_ok = 0;

    int residue_pairs = 0;
    for (int f = 0; f < FRAMES; f++) {
        /* 1) 上一代的临终颜色（每帧唯一，且与出生调色板不相交） */
        uint32_t dead_color = frame_dead_color(f);
        e.p[slot].color = dead_color;

        /* 2) 本帧老化：它死亡，拿到“同帧内上一颗已死粒子”的槽位与颜色 */
        int last_dead = em_age(&e, 1.0f);
        int dead_slot = (last_dead >= 0) ? last_dead : slot;
        uint32_t last_dead_color = e.p[dead_slot].color;
        int died_as_expected = (last_dead == slot) && !e.p[slot].alive;

        /* 3) 同帧发射新生粒子（出生色走合法调色板） */
        ParticleSpawn ns = {
            3.0f, 4.0f, 0.5f, 1.0f, 1.0f,
            em_palette_color(e.emitted)
        };
        int newborn_slot = respawn(&e, &ns);
        uint32_t newborn_color = (newborn_slot >= 0) ? e.p[newborn_slot].color : 0xFFFFFFFFu;

        /* 4) 判据：新生色 == 同帧上一颗已死粒子颜色 => 残留 */
        int same_slot_pair = (newborn_slot == last_dead);
        int is_residue = died_as_expected && same_slot_pair &&
                         (newborn_color == last_dead_color);
        if (is_residue) residue_pairs++;
        if (!died_as_expected || !same_slot_pair) *scene_ok = 0;

        if (verbose) {
            char name[32];
            snprintf(name, sizeof(name), "residue_frame_%02d", f);
            /* 干净帧打印 =1；若抓到残留打印 =0 并附双方颜色取证 */
            printf("%s=%d\n", name, is_residue ? 0 : 1);
            if (is_residue) {
                printf("  residue evidence frame=%d slot=%d newborn=0x%08X dead=0x%08X\n",
                       f, newborn_slot, newborn_color, last_dead_color);
            }
        }
    }
    return residue_pairs;
}

int main(void) {
    Emitter e; em_init(&e);
    check("spawn_ok", em_spawn(&e, 1, 2, 0, 0, 1.0f) == 0);
    check("zero_life_rejected", em_spawn(&e, 0, 0, 0, 0, 0.0f) == -1);

    Emitter r; em_init(&r);
    int alive = em_update(&r, 1.0f, 10.0f, 0);
    check("rate_count", alive == 10 && r.emitted == 10);

    Emitter b; em_init(&b);
    alive = em_update(&b, 0.0f, 0.0f, 5);
    check("burst_count", alive == 5);

    Emitter x; em_init(&x);
    em_spawn(&x, 0, 0, 0, 0, 0.5f);
    em_update(&x, 1.0f, 0.0f, 0);
    int still = 0;
    for (int i = 0; i < x.n; i++) if (x.p[i].alive) still++;
    check("expired", still == 0);

    Emitter cap; em_init(&cap);
    for (int i = 0; i < MAXP + 5; i++) em_spawn(&cap, 0, 0, 0, 0, 10.0f);
    check("capacity_dropped", cap.dropped == 5);

    Emitter mv; em_init(&mv);
    em_spawn(&mv, 0, 0, 2.0f, 0, 1.0f);
    em_update(&mv, 0.5f, 0, 0);
    check("moved", fabsf(mv.p[0].x - 1.0f) < 1e-5f);

    check("bad_dt", em_update(&mv, -1.0f, 0, 0) == -1);

    /* ---- 槽位清单与编译期/运行时完整性 ---- */
    printf("slot_manifest_fields=%d\n", PARTICLE_FIELD_COUNT);
    check("slot_manifest_complete", test_slot_manifest_complete());

    /* 清单重置必须连 0xCD 毒字节填充过的槽位一起覆盖（颜色精确等于出生色） */
    Particle poisoned;
    memset(&poisoned, 0xCD, sizeof(poisoned));
    ParticleSpawn fresh = { 1.0f, 2.0f, 3.0f, 4.0f, 1.0f, 0xFF112233u };
    poisoned = particle_spawn_reset(&fresh);
    check("manifest_reset_overwrites_color", poisoned.color == 0xFF112233u &&
           poisoned.age == 0.0f && poisoned.alive == 1);

    /* ---- 主验收：连续 20 帧，新生色 vs 同帧上一颗已死色，0 对相同 ---- */
    int scene_ok = 0;
    int residue_pairs = run_color_residue_frames(respawn_manifest, 1, &scene_ok);
    check("color_residue_20_frames", scene_ok && residue_pairs == 0);

    /* ---- 阳性对照：旧的漏 color 写法必须被同一判据抓出 20/20 对 ---- */
    int legacy_scene_ok = 0;
    int legacy_pairs = run_color_residue_frames(respawn_legacy_field_by_field, 0,
                                                &legacy_scene_ok);
    check("residue_check_catches_legacy_bug",
          legacy_scene_ok && legacy_pairs == 20);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
