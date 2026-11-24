
/* emitter: rate + burst emission with lifetime expiry and capacity. */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAXP 64

typedef struct { float x, y, vx, vy; float age, life; int alive; } P;
typedef struct { P p[MAXP]; int n; float rate_acc; int emitted; int dropped; } Emitter;

static void em_init(Emitter* e) { memset(e, 0, sizeof(*e)); }

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

static int em_update(Emitter* e, float dt, float rate, int burst) {
    if (dt < 0.0f) return -1;
    /* 先推进已有粒子，再发射新粒子，避免新粒子在本帧就被计满寿命 */
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        e->p[i].age += dt;
        if (e->p[i].age >= e->p[i].life) { e->p[i].alive = 0; continue; }
        e->p[i].x += e->p[i].vx * dt;
        e->p[i].y += e->p[i].vy * dt;
    }
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

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
