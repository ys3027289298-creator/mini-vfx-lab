/*
 * particles.h — 10 组爆炸的完整模拟管线。
 *
 * 算法逐字沿用仓库三个模块的原实现，只把容量放大到真实场景：
 *   emitter/src/emitter.c : 槽位线性扫描 spawn、全数组 alive 清点
 *   field/src/field.c     : expf 阻力 + sqrtf 涡流 + 重力积分
 *   trail/src/trail.c     : 满槽 memmove、tr_age 结构体压缩
 *
 * 三个 stage_* 函数分别对应一帧的三个模拟阶段，供外部插桩计时。
 */
#ifndef PARTICLES_H
#define PARTICLES_H

#include <math.h>
#include <string.h>

#define N_EMITTERS   10
#define MAXP         2048                  /* 每组槽位；10*2048 = 20480 */
#define MAXT         32                    /* trail.c 原值 */
#define BURST_FRAME  33                    /* 33*60 ≈ 1980 发/秒/组 */

typedef struct { float x, y, vx, vy; float age, life; int alive; } P;
typedef struct { float x, y, age; } TP;
typedef struct { TP p[MAXT]; int n; } Trail;

typedef struct {
    P p[MAXP];
    Trail tr[MAXP];
    int n;
    float rate_acc;
    int emitted;
    int dropped;
} Emitter;

static void sim_init(Emitter es[N_EMITTERS]) {
    memset(es, 0, sizeof(Emitter) * N_EMITTERS);
}

/* ---- emitter.c 原算法：线性找死槽，否则追加，满了丢弃 ---- */
static int em_spawn(Emitter* e, float x, float y, float vx, float vy, float life) {
    if (life <= 0.0f) return -1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) {
            e->p[i] = (P){ x, y, vx, vy, 0.0f, life, 1 };
            e->emitted++;
            return i;
        }
    }
    if (e->n >= MAXP) { e->dropped++; return -2; }
    e->p[e->n] = (P){ x, y, vx, vy, 0.0f, life, 1 };
    e->emitted++;
    return e->n++;
}

/*
 * 阶段 1：发射/寿命（对应 em_update 的前半与 spawn 部分）。
 * 位置积分由 field 阶段完成（field.c 的 step 末尾已做 x += vx*dt），
 * 这里不再重复移动，其余逻辑与 em_update 一致。
 */
static int stage_emit(Emitter es[N_EMITTERS], float dt) {
    for (int g = 0; g < N_EMITTERS; g++) {
        Emitter* e = &es[g];
        for (int i = 0; i < e->n; i++) {
            if (!e->p[i].alive) continue;
            e->p[i].age += dt;
            if (e->p[i].age >= e->p[i].life) e->p[i].alive = 0;
        }
        for (int b = 0; b < BURST_FRAME; b++) {
            float ang = (float)((b * 7 + g * 13) % 628) * 0.01f;   /* 确定扇形 */
            float spd = 2.0f + 0.05f * (float)(b % 40);
            float ox = -0.9f + 0.2f * (float)g;
            em_spawn(e, ox, 0.0f, cosf(ang) * spd, sinf(ang) * spd, 1.0f);
        }
        e->rate_acc += 0.0f * dt;
        int want = (int)e->rate_acc;
        e->rate_acc -= (float)want;
    }
    int alive = 0;                                          /* em_update 末尾的全量清点 */
    for (int g = 0; g < N_EMITTERS; g++)
        for (int i = 0; i < es[g].n; i++)
            alive += es[g].p[i].alive;
    return alive;
}

/* ---- field.c 原算法：指数阻力、涡流切向力、重力、积分 ---- */
static void field_step(P* p, float dt, float gx, float gy, float drag, float swirl) {
    float k = expf(-drag * dt);
    p->vx *= k; p->vy *= k;
    float r = sqrtf(p->x * p->x + p->y * p->y);
    if (r > 1e-6f && swirl != 0.0f) {
        float tx = -p->y / r, ty = p->x / r;
        p->vx += tx * swirl * dt;
        p->vy += ty * swirl * dt;
    }
    p->vx += gx * dt;
    p->vy += gy * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
}

/* 阶段 2：对所有存活粒子做力场积分。 */
static void stage_field(Emitter es[N_EMITTERS], float dt) {
    for (int g = 0; g < N_EMITTERS; g++) {
        Emitter* e = &es[g];
        for (int i = 0; i < e->n; i++) {
            if (!e->p[i].alive) continue;
            field_step(&e->p[i], dt, 0.0f, -9.8f, 0.5f, 2.0f);
        }
    }
}

/* ---- trail.c 原算法：满槽 memmove 平移，tr_age 压缩 ---- */
static void tr_push(Trail* t, float x, float y) {
    if (t->n >= MAXT) {
        memmove(&t->p[0], &t->p[1], sizeof(TP) * (MAXT - 1));
        t->n = MAXT - 1;
    }
    t->p[t->n].x = x; t->p[t->n].y = y; t->p[t->n].age = 0.0f;
    t->n++;
}

static void tr_age(Trail* t, float dt, float max_age) {
    for (int i = 0; i < t->n; i++) t->p[i].age += dt;
    int w = 0;
    for (int i = 0; i < t->n; i++)
        if (t->p[i].age < max_age) t->p[w++] = t->p[i];
    t->n = w;
}

/* 阶段 3：每个存活粒子推进一条 ribbon trail。 */
static void stage_trail(Emitter es[N_EMITTERS], float dt) {
    for (int g = 0; g < N_EMITTERS; g++) {
        Emitter* e = &es[g];
        for (int i = 0; i < e->n; i++) {
            if (!e->p[i].alive) continue;
            Trail* t = &e->tr[i];
            tr_push(t, e->p[i].x, e->p[i].y);
            tr_age(t, dt, 0.5f);
        }
    }
}

#endif
