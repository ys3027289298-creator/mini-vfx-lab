/*
 * particle_slots.h —— 粒子池「槽位字段清单」（Particle 存储字段的唯一事实来源）
 *
 * ============================================================================
 * 槽位完整性判据（Slot Completeness Criterion, SCC）
 * ----------------------------------------------------------------------------
 * 一个被复用的粒子槽位是“完整”的，当且仅当以下三条同时成立：
 *
 *   C1 存储完备：Particle 的每一个存储字段都必须登记在下方 PARTICLE_FIELDS。
 *        - Particle 结构体本身由 PARTICLE_FIELDS 经 X 宏展开生成，新增字段的
 *          唯一合法入口就是这张清单 —— 清单之外不存在（也不允许存在）字段；
 *        - 若有人绕过清单、直接在 struct 里私加字段，两道编译期卡口会让编译
 *          直接失败，实现“新增字段忘了登记/重置就自动发现”：
 *            (a) particle_spawn_reset() 对结构做“每清单字段一个初值”的全位置
 *                初始化，并用头文件内 pragma 把 -Wmissing-field-initializers
 *                升级为 error（gcc/clang 均验证，报错会直接点出私加字段名）；
 *            (b) 每个字段的 offsetof 与“清单影子结构”逐一 _Static_assert，
 *                另有 sizeof(Particle)==sizeof(Shadow) 总卡口。
 *
 *   C2 重置完备：每个字段行尾注释都写明它在哪一步被重置（INIT / SPAWN /
 *        DEATH / TICK）。槽位复用只允许走 particle_spawn_reset() 的整槽覆写，
 *        清单中的每个字段都在该函数里拿到新值，因此不会再“漏擦一个字段”。
 *
 *   C3 运行时无残留：emitter.c 连续截 20 帧，每帧比较新生粒子颜色与同帧内
 *        上一颗已死粒子的最终颜色；任一对完全相同即判定为颜色残留，测试失败。
 *
 * 新增字段的正确动作只有一步：在 PARTICLE_FIELDS 里加一行 X(...)，写清重置
 * 步骤与行尾注释。结构、重置函数、描述符表、编译卡口都会自动带上它。
 * ============================================================================
 */
#ifndef PARTICLE_SLOTS_H
#define PARTICLE_SLOTS_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#  define PARTICLE_UNUSED __attribute__((unused))
#else
#  define PARTICLE_UNUSED
#endif

/* 颜色打包格式 RGBA8888：高 8 位 R，依次 G/B/A。打包成单个 uint32_t
 * 才能用一次整数相等比较精确判断“两颗粒子颜色是否完全相同”。 */
#define PARTICLE_RGBA(r, g, b, a) \
    ((uint32_t)(r) << 24 | (uint32_t)(g) << 16 | (uint32_t)(b) << 8 | (uint32_t)(a))

/*
 * 重置步骤：每个清单字段必须归属其中之一。
 */
typedef enum ParticleResetStep {
    PRS_INIT = 0,  /* em_init()：memset 整池为 0，每个槽位生命周期只做一次 */
    PRS_SPAWN,     /* em_spawn_ex()->particle_spawn_reset()：复用瞬间整槽覆写，防残留的关键步骤 */
    PRS_DEATH,     /* em_kill()/寿命过期：只把 alive 置 0 释放槽位，表现字段不擦，统一留给 SPAWN */
    PRS_TICK       /* em_age()/em_update()：存活期间每帧推进，是更新不是重置 */
} ParticleResetStep;

/*
 * SPAWN 步骤的新值来源（发射参数）。
 */
typedef struct ParticleSpawn {
    float    x, y;   /* 出生坐标 */
    float    vx, vy; /* 初速度 */
    float    life;   /* 寿命（秒），必须 > 0 */
    uint32_t color;  /* 出生颜色 RGBA8888 */
} ParticleSpawn;

/*
 * ----------------------------------------------------------------------------
 * 字段清单 PARTICLE_FIELDS —— 以后给 Particle 加存储字段，只能在这里加行。
 *
 * 列含义：X(类型, 字段名, 重置步骤, SPAWN 初值表达式)
 * 每一行末尾的注释逐一写明：这个字段到底在哪一步、由哪个函数重置。
 * 注意：行尾续行符 '\' 必须保持为该行最后一个字符。
 * ----------------------------------------------------------------------------
 */
#define PARTICLE_FIELDS(X) \
    X(float,    x,     PRS_SPAWN, sp->x)     /* 重置点 SPAWN：particle_spawn_reset() 用发射坐标整槽覆写；INIT 时随 memset 为 0 */ \
    X(float,    y,     PRS_SPAWN, sp->y)     /* 重置点 SPAWN：同上，覆写为本次发射 Y，不继承上一代位置；INIT 为 0 */ \
    X(float,    vx,    PRS_SPAWN, sp->vx)    /* 重置点 SPAWN：覆写为本次初速度 X；TICK 阶段才允许被积分修改；INIT 为 0 */ \
    X(float,    vy,    PRS_SPAWN, sp->vy)    /* 重置点 SPAWN：覆写为本次初速度 Y，杜绝速度残留；INIT 为 0 */ \
    X(float,    age,   PRS_SPAWN, 0.0f)      /* 重置点 SPAWN：复用重生时强制归零；之后 TICK(em_age) 每帧 += dt；INIT 为 0 */ \
    X(float,    life,  PRS_SPAWN, sp->life)  /* 重置点 SPAWN：覆写为本次寿命；DEATH 不改它；INIT 为 0（会被 spawn 校验拦截） */ \
    X(uint32_t, color, PRS_SPAWN, sp->color) /* 重置点 SPAWN：覆写为本次出生色 —— 修复“新粒子带上一颗颜色闪一下”的主字段；DEATH 故意不清，靠 SPAWN 整槽覆盖 */ \
    X(int,      alive, PRS_SPAWN, 1)         /* 重置点 SPAWN：置 1 占用槽位；DEATH（em_kill/过期）置 0；INIT 的 memset 使其初值为 0 */

/* ---- 展开 1：Particle 结构体本身（清单即结构，清单之外无字段） ---- */
#define PARTICLE_STRUCT_DECL(type, name, step, spawn_value) type name;
typedef struct Particle {
    PARTICLE_FIELDS(PARTICLE_STRUCT_DECL)
} Particle;

/* ---- 展开 2：清单影子结构，专供编译期布局核对 ---- */
#define PARTICLE_SHADOW_DECL(type, name, step, spawn_value) type name;
typedef struct ParticleManifestShadow {
    PARTICLE_FIELDS(PARTICLE_SHADOW_DECL)
} ParticleManifestShadow;

/* ---- C1(b) 编译期卡口：任何字段偏移相对影子漂移，说明清单外被塞了字段 ---- */
#define PARTICLE_OFFSET_ASSERT(type, name, step, spawn_value) \
    _Static_assert(offsetof(Particle, name) == offsetof(ParticleManifestShadow, name), \
                   "Particle." #name " offset drift: field inserted outside PARTICLE_FIELDS?");
PARTICLE_FIELDS(PARTICLE_OFFSET_ASSERT)
#undef PARTICLE_OFFSET_ASSERT

_Static_assert(sizeof(Particle) == sizeof(ParticleManifestShadow),
               "sizeof(Particle) != manifest shadow: a storage field is missing from PARTICLE_FIELDS");

/*
 * ---- C2 重置入口：清单驱动的整槽覆写 ----
 * 位置初始化器恰好每个清单字段一个值。若 Particle 被清单外加字段，
 * 初始化器数量少于结构字段，下面的 -Wmissing-field-initializers 会被
 * pragma 升级成编译错误（C1(a) 主卡口，能点出未登记字段名）。
 */
PARTICLE_UNUSED static inline Particle particle_spawn_reset(const ParticleSpawn *sp) {
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic error "-Wmissing-field-initializers"
#endif
#define PARTICLE_SPAWN_VALUE(type, name, step, spawn_value) spawn_value,
    Particle slot = {
        PARTICLE_FIELDS(PARTICLE_SPAWN_VALUE)
    };
#undef PARTICLE_SPAWN_VALUE
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    return slot;
}

/* ---- 展开 3：字段描述符表（运行时也能遍历清单，核对槽位完整性） ---- */
typedef struct ParticleFieldDescriptor {
    const char        *name;       /* 字段名 */
    size_t             offset;     /* offsetof(Particle, name) */
    size_t             size;       /* sizeof(字段类型) */
    ParticleResetStep  reset_step; /* 该字段声明的重置步骤 */
} ParticleFieldDescriptor;

#define PARTICLE_DESC_ROW(type, name, step, spawn_value) \
    { #name, offsetof(Particle, name), sizeof(type), step },

static PARTICLE_UNUSED const ParticleFieldDescriptor particle_field_descriptors[] = {
    PARTICLE_FIELDS(PARTICLE_DESC_ROW)
};

#define PARTICLE_FIELD_COUNT \
    ((int)(sizeof(particle_field_descriptors) / sizeof((particle_field_descriptors)[0])))

PARTICLE_UNUSED static inline const char *particle_reset_step_name(ParticleResetStep step) {
    switch (step) {
        case PRS_INIT:  return "INIT";
        case PRS_SPAWN: return "SPAWN";
        case PRS_DEATH: return "DEATH";
        case PRS_TICK:  return "TICK";
    }
    return "?";
}

#endif /* PARTICLE_SLOTS_H */
