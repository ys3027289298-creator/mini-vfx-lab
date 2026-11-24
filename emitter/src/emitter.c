
/* emitter: rate + burst emission with lifetime expiry and capacity. */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAXP 64

typedef struct { float x, y, vx, vy; float age, life; int alive; } P;
typedef struct { P p[MAXP]; int n;
                 float rate_acc;
                 int debt;        /* whole particles owed by paused wall time */
                 float debt_frac; /* fractional owed amount, never truncated away */
                 int emitted; int dropped; } Emitter;

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

/* Pause-aware update.
 *
 * dt         : simulated seconds for THIS frame. The frame loop must rebase
 *              its clock on resume so dt is one normal frame, NOT the raw
 *              timestamp gap across the pause (that gap would teleport
 *              existing particles and spike rate_acc at once).
 * pause_span : wall seconds spent paused, booked ONLY into the emission debt
 *              ledger. It never reaches particle positions/ages.
 * pay_cap    : max debt particles allowed to spawn this frame; bounds the
 *              resume "catch-up flood". A debt particle rejected by capacity
 *              is resolved through the usual dropped counter.
 */
static int em_update_ex(Emitter* e, float dt, float rate, int burst,
                        float pause_span, int pay_cap) {
    if (dt < 0.0f || pause_span < 0.0f || pay_cap < 0) return -1;
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

    /* Pause interval -> count debt only; fractional remainder is retained. */
    e->debt_frac += rate * pause_span;
    int owed = (int)e->debt_frac;
    e->debt_frac -= (float)owed;
    e->debt += owed;

    /* Repay at most pay_cap per frame so the backlog cannot pour out. */
    int pay = e->debt < pay_cap ? e->debt : pay_cap;
    for (int i = 0; i < pay; i++) {
        /* em_spawn already counts a full pool as dropped; either way the
         * obligation is discharged here (birth or accounted drop). */
        em_spawn(e, 0, 0, 0, 1.0f, 1.0f);
        e->debt--;
    }

    int alive = 0;
    for (int i = 0; i < e->n; i++) if (e->p[i].alive) alive++;
    return alive;
}

static int em_update(Emitter* e, float dt, float rate, int burst) {
    return em_update_ex(e, dt, rate, burst, 0.0f, 0);
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

    /* pause debt: rate=10/s paused 1s -> 10 owed, repaid <=2 per frame */
    Emitter pd; em_init(&pd);
    em_update_ex(&pd, 0.0f, 10.0f, 0, 1.0f, 2); /* resume frame */
    check("pause_debt_booked_capped", pd.debt == 8 && pd.emitted == 2);
    int max_frame = pd.emitted; /* 2 on the booking frame */
    for (int f = 0; f < 4; f++) {
        int before = pd.emitted;
        em_update_ex(&pd, 0.0f, 10.0f, 0, 0.0f, 2);
        int got = pd.emitted - before;
        if (got > max_frame) max_frame = got;
    }
    check("pause_debt_no_frame_flood", max_frame <= 2);
    check("pause_debt_eventually_paid", pd.debt == 0 && pd.emitted == 10);
    check("bad_pause_span", em_update_ex(&pd, 0.0f, 10.0f, 0, -1.0f, 2) == -1);
    check("bad_pay_cap", em_update_ex(&pd, 0.0f, 10.0f, 0, 0.0f, -1) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
