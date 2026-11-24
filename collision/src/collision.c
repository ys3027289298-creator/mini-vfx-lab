
/* collision: component-wise floor impact response (restitution on the normal,
   friction on the tangent) plus a sleep cutoff that terminates the bounce
   series. Coordinate convention: y up, floor at y = 0, normal n = (0, 1). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, vx, vy; } CPart;
typedef struct {
    float gx, gy;        /* gravity acceleration */
    float restitution;   /* e: normal velocity multiplier after reversal, 0..1 */
    float friction;      /* mu: tangential velocity retained per impact, 0..1 */
    float floor_y;       /* floor level */
    float sleep_speed;   /* |v| at or below which a contacting particle rests */
    float last_impact_speed; /* diagnostic: pre-impact |v| of the last contact */
    int   bounces;       /* diagnostic: impacts applied since last reset */
} Floor;

static void fl_init(Floor* f, float gx, float gy, float e, float mu,
                    float floor_y, float sleep_speed) {
    memset(f, 0, sizeof(*f));
    f->gx = gx; f->gy = gy;
    f->restitution = e; f->friction = mu;
    f->floor_y = floor_y; f->sleep_speed = sleep_speed;
}

static int fl_validate(const Floor* f) {
    if (f->restitution < 0.0f || f->restitution > 1.0f) return -1;
    if (f->friction < 0.0f || f->friction > 1.0f) return -2;
    if (f->sleep_speed <= 0.0f) return -3;
    return 0;
}

/* Resolve one impact. Returns 1 if the particle is put to sleep, 0 if it
   bounces, negative on bad parameters. */
static int fl_resolve_impact(Floor* f, CPart* p) {
    if (fl_validate(f) != 0) return -1;
    /* only particles moving into the surface collide */
    if (p->y > f->floor_y || p->vy > 0.0f) return 0;

    float vn = p->vy;                                   /* normal component (<=0) */
    float vt = p->vx;                                   /* tangent component */
    float speed = sqrtf(vn * vn + vt * vt);
    f->last_impact_speed = speed;
    if (speed <= f->sleep_speed) {
        p->y = f->floor_y; p->vx = 0.0f; p->vy = 0.0f;  /* rest: ends the series */
        return 1;
    }

    /* split response: normal reversed by restitution, tangent damped by friction */
    p->vy = -f->restitution * vn;
    p->vx =  f->friction * vt;
    p->y = f->floor_y;
    f->bounces++;
    return 0;
}

/* Advance one time step: integrate, then resolve penetration. Returns the
   impact result (1 slept, 0 bounced / no contact), negative on bad input. */
static int fl_step(Floor* f, CPart* p, float dt) {
    if (dt < 0.0f) return -1;
    if (fl_validate(f) != 0) return -2;

    p->vx += f->gx * dt;
    p->vy += f->gy * dt;
    p->x  += p->vx * dt;
    p->y  += p->vy * dt;

    if (p->y <= f->floor_y && p->vy <= 0.0f)
        return fl_resolve_impact(f, p);
    return 0;
}

static float fl_last_ratio(const Floor* f, float v0) {
    return v0 > 0.0f ? f->last_impact_speed / v0 : 0.0f;
}

typedef struct {
    int   converged;     /* 1 if the sleep cutoff was reached */
    int   bounces;       /* impacts applied before sleep */
    float settle;        /* seconds from first landing to sleep */
    float last_ratio;    /* pre-impact speed / v0 at the final contact */
} SeriesResult;

/* Drive the bounce series from the given state until the particle sleeps.
   Returns 0 on convergence, -1 if it does not terminate within max_steps:
   the divergent combinations e -> 1 (Zeno time diverges) or mu -> 1 with a
   tangent component above the sleep threshold (never crosses it). See
   analysis/ANALYSIS.md section 4. */
static int run_series(Floor* f, CPart seed, float v0, float dt,
                      int max_steps, SeriesResult* out) {
    CPart p = seed;
    float t = 0.0f, t0 = -1.0f;
    for (int i = 0; i < max_steps; i++) {
        int was_air = p.y > 0.0f;
        int res = fl_step(f, &p, dt);
        t += dt;
        if (res > 0) {
            out->converged = 1;
            out->bounces = f->bounces;
            out->settle = t - t0;
            out->last_ratio = fl_last_ratio(f, v0);
            return 0;
        }
        if (t0 < 0.0f && was_air && fabsf(p.y - f->floor_y) < 1e-6f) t0 = t;
    }
    out->converged = 0;
    out->bounces = f->bounces;
    out->settle = t - (t0 < 0.0f ? 0.0f : t0);
    out->last_ratio = fl_last_ratio(f, v0);
    return -1;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    /* normal component reverses with restitution; tangent decays with friction */
    Floor f; fl_init(&f, 0, -9.8f, 0.6f, 0.7f, 0.0f, 0.05f);
    CPart a = { 0, 0, 10.0f, -8.0f };
    int r = fl_resolve_impact(&f, &a);
    check("normal_reversed", r == 0 && fabsf(a.vy - 4.8f) < 1e-5f);
    check("tangent_damped", fabsf(a.vx - 7.0f) < 1e-5f);
    check("rest_on_floor", fabsf(a.y) < 1e-6f);

    /* upward-moving or above-floor particle is not a collision */
    CPart up = { 0, 0, 0, 1.0f };
    check("no_collision_up", fl_resolve_impact(&f, &up) == 0 && up.vy == 1.0f);

    /* e = 0 kills the normal component in one hit instead of mirroring it */
    Floor dead; fl_init(&dead, 0, -9.8f, 0.0f, 1.0f, 0.0f, 0.05f);
    CPart d = { 0, 0, 5.0f, -6.0f };
    fl_resolve_impact(&dead, &d);
    check("zero_restitution", d.vy == 0.0f && fabsf(d.vx - 5.0f) < 1e-6f);

    /* sleep cutoff: slow contact stops dead instead of bouncing forever */
    CPart s = { 0, 0, 0.02f, -0.03f };
    r = fl_resolve_impact(&f, &s);
    check("sleep_cutoff", r == 1 && s.vx == 0.0f && s.vy == 0.0f && s.y == 0.0f);

    /* full bounce series: three incidence angles, same coefficients the
       analysis selects (e = 0.33, mu = 0.8, sleep at 1% of the 10 m/s impact).
       Launch height h = v_n^2 / 2g makes the first landing hit at V0. */
    const float E_PICK = 0.33f, MU_PICK = 0.8f, V0 = 10.0f;
    const float VSLEEP = 0.01f * V0, H = 0.0001f;
    const float angles[3] = { 15.0f, 45.0f, 75.0f };
    const int   expect[3] = { 15, 20, 21 };   /* closed-form bounce counts */
    int all_deadline = 1, all_ratio = 1, counts_ok = 1;
    for (int a = 0; a < 3; a++) {
        float rad = angles[a] * 3.14159265f / 180.0f;
        float v1n = V0 * cosf(rad), v1t = V0 * sinf(rad);
        Floor sim; fl_init(&sim, 0, -9.8f, E_PICK, MU_PICK, 0.0f, VSLEEP);
        CPart seed = { 0, v1n * v1n / (2.0f * 9.8f), v1t, 0.0f };
        SeriesResult sr;
        int rc = run_series(&sim, seed, V0, H, 5000000, &sr);
        int deadline_ok = rc == 0 && sr.settle <= 1.0f;
        int ratio_ok = rc == 0 && sr.last_ratio <= 0.01f;
        int count_ok = rc == 0 && abs(sr.bounces - expect[a]) <= 1;
        printf("angle_%gdeg: bounces=%d settle=%.4fs last_impact_ratio=%.5f "
               "deadline=%d ratio1pct=%d\n",
               (double)angles[a], sr.bounces, sr.settle, sr.last_ratio,
               deadline_ok, ratio_ok);
        all_deadline &= deadline_ok; all_ratio &= ratio_ok; counts_ok &= count_ok;
    }
    check("all_angles_settle_in_1s", all_deadline);
    check("all_angles_below_1pct", all_ratio);
    check("bounce_counts_match_closed_form", counts_ok);

    /* infeasibility demonstration: e = 0.95 still terminates thanks to the
       sleep cutoff, but takes far longer than 1 s — the divergent Zeno series
       in miniature. Reports the bounce that finally stops it. */
    {
        float v1n = V0 * cosf(15.0f * 3.14159265f / 180.0f);
        float v1t = V0 * sinf(15.0f * 3.14159265f / 180.0f);
        Floor bouncy; fl_init(&bouncy, 0, -9.8f, 0.95f, 0.95f, 0.0f, VSLEEP);
        CPart seed = { 0, v1n * v1n / (2.0f * 9.8f), v1t, 0.0f };
        SeriesResult sr;
        run_series(&bouncy, seed, V0, H, 5000000, &sr);
        printf("e0.95: sleeps_after_bounce=%d settle=%.4fs (deadline_violated=%d)\n",
               sr.bounces, sr.settle, sr.settle > 1.0f);
        check("high_e_violates_deadline", sr.settle > 1.0f);
    }

    /* divergence endpoints from the infeasibility proof: neither series
       terminates, and the step cap must catch it instead of looping forever */
    {
        float v1n = V0 * cosf(15.0f * 3.14159265f / 180.0f);
        float v1t = V0 * sinf(15.0f * 3.14159265f / 180.0f);

        Floor e1; fl_init(&e1, 0, -9.8f, 1.0f, 0.8f, 0.0f, VSLEEP);
        SeriesResult sr1;
        int rc1 = run_series(&e1, (CPart){ 0, v1n * v1n / 19.6f, v1t, 0.0f },
                             V0, H, 200000, &sr1);
        /* never converges and impact speed stays near V0 instead of decaying */
        check("e_equals_1_diverges", rc1 == -1 && sr1.bounces >= 5
                                      && sr1.last_ratio > 0.5f);

        Floor mu1; fl_init(&mu1, 0, -9.8f, 0.33f, 1.0f, 0.0f, VSLEEP);
        SeriesResult sr2;
        int rc2 = run_series(&mu1, (CPart){ 0, v1n * v1n / 19.6f, v1t, 0.0f },
                             V0, H, 200000, &sr2);
        /* normal dies out but the tangent stays at v1t = 2.59 > v_s forever */
        check("mu_equals_1_never_sleeps", rc2 == -1 && sr2.last_ratio > 0.01f);
    }

    check("bad_e", fl_validate(&(Floor){ .restitution = 1.2f }) == -1);
    check("bad_mu", fl_validate(&(Floor){ .restitution = 0.5f, .friction = -0.1f }) == -2);
    check("bad_sleep", fl_validate(&(Floor){ .restitution = 0.5f, .sleep_speed = 0.0f }) == -3);
    check("bad_dt", fl_step(&f, &a, -1.0f) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
