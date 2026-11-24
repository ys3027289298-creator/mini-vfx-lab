
/* trail: age-sorted ribbon points with width taper. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAXT 32

typedef struct { float x, y, age; } TP;
typedef struct { TP p[MAXT]; int n; } Trail;

static int tr_push(Trail* t, float x, float y) {
    if (t->n >= MAXT) {
        memmove(&t->p[0], &t->p[1], sizeof(TP) * (MAXT - 1));
        t->n = MAXT - 1;
    }
    t->p[t->n].x = x; t->p[t->n].y = y; t->p[t->n].age = 0.0f;
    return t->n++;
}

static int tr_age(Trail* t, float dt, float max_age) {
    if (dt < 0.0f || max_age <= 0.0f) return -1;
    for (int i = 0; i < t->n; i++) t->p[i].age += dt;
    int w = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->p[i].age < max_age) t->p[w++] = t->p[i];
    }
    t->n = w;
    return w;
}

/* width falls linearly from head to tail, normalized by max_age */
static float tr_width(const Trail* t, int i, float base, float max_age) {
    if (t->n <= 0 || i < 0 || i >= t->n) return -1.0f;
    float k = t->p[i].age / max_age;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    return base * (1.0f - k);
}

/* ------------------------------------------------------------------ */
/* displacement-based sampler: spacing by arc length, not by time.    */
/* derivation: docs/sampling.md                                       */
/* ------------------------------------------------------------------ */

#define SMP_MAXOUT 512          /* output points retained per trail    */
#define SMP_RING   256          /* raw frames cached inside a q<1 zone */
#define SMP_CMIN   1.04719755f  /* pi/3: non-overlap lower bound, q>=1 */
#define SMP_CMAX   2.0f         /* connected-trail upper bound         */
#define SMP_QEXIT  1.15f        /* hysteresis: leave zone only above it */
#define SMP_RHOMAX 3.402823466e+38f

typedef struct { float x, y; } SMPt;

typedef struct {
    float radius;       /* particle radius r                      */
    float c;            /* spacing coefficient, c*r = arc spacing */
    int   fixed_k;      /* point count forced inside q<1 zones    */
} SMPConfig;

typedef struct {
    SMPConfig cfg;
    /* finite-difference history */
    int   frames;
    SMPt  px0;              /* previous position                 */
    float pvx, pvy;         /* previous velocity                 */
    /* q<1 zone ring buffer */
    int   in_zone;
    SMPt  ring[SMP_RING];
    int   rn;
    /* output */
    SMPt  out[SMP_MAXOUT];
    int   n;
    /* telemetry */
    int   breaks;           /* adjacent output pairs farther than 2r */
    int   overlap_viol;     /* normal-mode pairs closer than r       */
    int   zone_entries;
    int   zone_emitted;     /* points emitted by fixed-count mode    */
    float last_spacing;
} Sampler;

static void smp_init(Sampler* s, float radius, float c, int fixed_k) {
    memset(s, 0, sizeof(*s));
    if (c < SMP_CMIN) c = SMP_CMIN;
    if (c > SMP_CMAX) c = SMP_CMAX;
    if (fixed_k < 2) fixed_k = 2;
    s->cfg.radius = radius;
    s->cfg.c = c;
    s->cfg.fixed_k = fixed_k;
}

/* rho = |v|^3 / |v x a|; SMP_RHOMAX when straight / not enough history */
static float smp_curvature_radius(float vx, float vy, float ax, float ay) {
    float v2 = vx * vx + vy * vy;
    if (v2 < 1e-12f) return SMP_RHOMAX;
    float cross = fabsf(vx * ay - vy * ax);
    if (cross < 1e-12f) return SMP_RHOMAX;
    float v = sqrtf(v2);
    return v2 * v / cross;
}

static void smp_emit(Sampler* s, float x, float y, int fixed_mode) {
    if (s->n > 0) {
        float dx = x - s->out[s->n - 1].x;
        float dy = y - s->out[s->n - 1].y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > 2.0f * s->cfg.radius) s->breaks++;
        /* q<1 zones overlap geometrically by construction (docs 3.3):
           only normal-mode closeness counts as a sampling violation */
        if (!fixed_mode && d < s->cfg.radius) s->overlap_viol++;
        s->last_spacing = d;
    }
    if (s->n < SMP_MAXOUT) {
        s->out[s->n].x = x;
        s->out[s->n].y = y;
        s->n++;
    }
    if (fixed_mode) s->zone_emitted++;
}

/* equal-arc-length resample of a cached polyline into fixed_k points.
   ring[0] is the zone entry and was already emitted, so flush emits k-1. */
static void smp_flush_zone(Sampler* s) {
    int k = s->cfg.fixed_k;
    if (s->rn < 2 || k < 2) { s->rn = 0; s->in_zone = 0; return; }

    float total = 0.0f;
    for (int i = 1; i < s->rn; i++)
        total += hypotf(s->ring[i].x - s->ring[i - 1].x,
                        s->ring[i].y - s->ring[i - 1].y);
    if (total < 1e-9f) { s->rn = 0; s->in_zone = 0; return; }

    float step = total / (float)(k - 1);
    int seg = 1;
    float seg_start = 0.0f;
    for (int j = 1; j < k; j++) {
        float target = step * (float)j;
        for (;;) {
            float slen = hypotf(s->ring[seg].x - s->ring[seg - 1].x,
                                s->ring[seg].y - s->ring[seg - 1].y);
            if (seg >= s->rn - 1 || seg_start + slen >= target) break;
            seg_start += slen;
            seg++;
        }
        float sx = s->ring[seg - 1].x, sy = s->ring[seg - 1].y;
        float ex = s->ring[seg].x,     ey = s->ring[seg].y;
        float slen = hypotf(ex - sx, ey - sy);
        float u = slen > 1e-9f ? (target - seg_start) / slen : 0.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        smp_emit(s, sx + u * (ex - sx), sy + u * (ey - sy), 1);
    }
    s->rn = 0;
    s->in_zone = 0;
}

/* ring full: decimate in place so no part of a long zone is dropped */
static void smp_compact_ring(Sampler* s) {
    int keep = SMP_RING / 2;
    for (int i = 1; i < keep; i++) s->ring[i] = s->ring[2 * i];
    s->rn = keep;
}

/* normal-mode emission along one frame segment p0 -> p1.
   New points are circle intersections: distance exactly c*r from the
   last output, so an oscillating segment that swings back can never
   stack a point on top of an earlier one. */
static void smp_emit_along(Sampler* s, SMPt p0, SMPt p1) {
    if (s->n == 0) { smp_emit(s, p0.x, p0.y, 0); return; }
    float spacing = s->cfg.c * s->cfg.radius;
    SMPt anchor = s->out[s->n - 1];
    for (int guard = 0; guard < 8; guard++) {
        float ex = p1.x - p0.x, ey = p1.y - p0.y;
        float fx = p0.x - anchor.x, fy = p0.y - anchor.y;
        float ee = ex * ex + ey * ey;
        if (ee < 1e-12f) return;
        float b = 2.0f * (fx * ex + fy * ey);
        float cc = fx * fx + fy * fy - spacing * spacing;
        float disc = b * b - 4.0f * ee * cc;
        if (disc < 0.0f) return;              /* segment misses the circle */
        float u = (-b + sqrtf(disc)) / (2.0f * ee);
        if (u > 1.0f) return;                 /* crossing lies beyond p1 */
        if (u < 0.0f) u = 0.0f;
        float qx = p0.x + u * ex, qy = p0.y + u * ey;
        smp_emit(s, qx, qy, 0);
        anchor = s->out[s->n - 1];
        p0 = (SMPt){ qx, qy };                /* continue toward p1 */
    }
}

/* shared feed: v/a may be analytic (experiment) or finite-differenced */
static void smp_feed(Sampler* s, float x, float y, float dt,
                     float vx, float vy, float ax, float ay, int have_va) {
    if (!have_va && s->frames >= 1) {
        vx = (x - s->px0.x) / dt;
        vy = (y - s->px0.y) / dt;
        if (s->frames >= 2) {
            ax = (vx - s->pvx) / dt;
            ay = (vy - s->pvy) / dt;
        }
        have_va = 1;
    }
    float rho = smp_curvature_radius(vx, vy, ax, ay);
    float q = rho / s->cfg.radius;
    /* hysteresis: enter at q<1, only leave above SMP_QEXIT */
    int tight = s->in_zone ? (q < SMP_QEXIT) : (q < 1.0f);

    if (tight) {
        if (!s->in_zone) {
            s->in_zone = 1;
            s->zone_entries++;
            s->rn = 0;
            if (s->n > 0) {
                /* anchor the zone on the actual last output, emitting it
                   once more only if it is far enough away to avoid a dup */
                s->ring[s->rn++] = s->out[s->n - 1];
            } else if (s->frames) {
                s->ring[s->rn++] = s->px0;
                smp_emit(s, s->px0.x, s->px0.y, 1);
            } else {
                s->ring[s->rn++] = (SMPt){ x, y };
                smp_emit(s, x, y, 1);
            }
        }
        if (s->rn >= SMP_RING) smp_compact_ring(s);
        s->ring[s->rn++] = (SMPt){ x, y };
    } else {
        if (s->in_zone) smp_flush_zone(s);
        if (s->frames) smp_emit_along(s, s->px0, (SMPt){ x, y });
    }

    s->px0 = (SMPt){ x, y };
    s->pvx = vx; s->pvy = vy;
    s->frames++;
}

/* production entry: derivatives reconstructed from positions */
static void smp_update(Sampler* s, float x, float y, float dt) {
    smp_feed(s, x, y, dt, 0.0f, 0.0f, 0.0f, 0.0f, 0);
}

/* call once when the particle/trail ends: flush an open q<1 zone */
static void smp_finish(Sampler* s) {
    if (s->in_zone) smp_flush_zone(s);
}

/* analytic entry: exact velocity/acceleration known (test rigs) */
static void smp_update_va(Sampler* s, float x, float y,
                          float vx, float vy, float ax, float ay) {
    smp_feed(s, x, y, 1.0f, vx, vy, ax, ay, 1);
}

/* choose c from a trail-window length budget; see docs/sampling.md §4 */
static float smp_pick_coef(float window_len, float radius, int n_cap) {
    float need = window_len / (radius * (float)n_cap);
    if (need < SMP_CMIN) need = SMP_CMIN;
    if (need > SMP_CMAX) need = SMP_CMAX;
    return need;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

#ifndef TRAIL_SAMPLER_LIB
int main(void) {
    Trail t; memset(&t, 0, sizeof(t));
    for (int i = 0; i < 5; i++) tr_push(&t, (float)i, 0);
    check("push_count", t.n == 5);

    check("age_ok", tr_age(&t, 0.1f, 1.0f) > 0);
    check("age_accumulated", fabsf(t.p[0].age - 0.1f) < 1e-5f);

    check("expire", tr_age(&t, 2.0f, 1.0f) == 0 && t.n == 0);

    Trail w; memset(&w, 0, sizeof(w));
    tr_push(&w, 0, 0);
    tr_age(&w, 0.5f, 1.0f);
    check("width_head", fabsf(tr_width(&w, 0, 4.0f, 1.0f) - 2.0f) < 1e-5f);

    Trail o; memset(&o, 0, sizeof(o));
    for (int i = 0; i < MAXT + 3; i++) tr_push(&o, (float)i, 0);
    check("ring_capped", o.n == MAXT);

    check("bad_dt", tr_age(&o, -1.0f, 1.0f) == -1);
    check("bad_age", tr_age(&o, 1.0f, 0.0f) == -1);
    check("bad_index", tr_width(&o, -1, 1.0f, 1.0f) < 0.0f);

    /* ---- displacement sampler ---- */

    /* straight line, high vs low speed over EQUAL displacement:
       spacing and point count must be speed-invariant */
    Sampler hi, lo;
    smp_init(&hi, 1.0f, SMP_CMIN, 4);
    smp_init(&lo, 1.0f, SMP_CMIN, 4);
    for (int i = 0; i < 100; i++) smp_update(&hi, 0.05f * 20.0f * (float)i, 0.0f, 0.05f);
    for (int i = 0; i < 4000; i++) smp_update(&lo, 0.05f * 0.5f * (float)i, 0.0f, 0.05f);
    check("straight_no_break", hi.breaks == 0 && lo.breaks == 0);
    check("straight_no_overlap", hi.overlap_viol == 0 && lo.overlap_viol == 0);
    check("speed_invariant_count", abs(hi.n - lo.n) <= 1);
    check("spacing_matches_coef",
          fabsf(hi.last_spacing - SMP_CMIN) < 0.05f &&
          fabsf(lo.last_spacing - SMP_CMIN) < 0.05f);

    /* low-speed jitter: slow drift with oscillation must not pile points up.
       amplitude/frequency chosen so rho stays > r (q>=1) the whole way;
       old time-based sampling emitted every frame (400 pts in ~20r) */
    Sampler j;
    smp_init(&j, 1.0f, SMP_CMIN, 4);
    for (int i = 0; i < 400; i++) {
        float x = 0.05f * (float)i + 1.5f * sinf(0.2f * (float)i);
        smp_update(&j, x, 0.0f, 0.05f);
    }
    check("jitter_emits_sparsely", j.n < 80);
    check("jitter_no_overlap", j.overlap_viol == 0);

    /* analytic circle R=5: curvature estimate must recover R */
    float R = 5.0f, omega = 2.0f;
    Sampler cv;
    smp_init(&cv, 1.0f, SMP_CMIN, 4);
    float est = 0.0f;
    for (int i = 0; i <= 400; i++) {
        float ang = omega * 0.005f * (float)i;
        float x = R * cosf(ang), y = R * sinf(ang);
        float vx = -R * omega * sinf(ang), vy = R * omega * cosf(ang);
        float ax = -R * omega * omega * cosf(ang), ay = -R * omega * omega * sinf(ang);
        if (i >= 2) est = smp_curvature_radius(vx, vy, ax, ay);
        smp_update_va(&cv, x, y, vx, vy, ax, ay);
    }
    check("curvature_estimate", fabsf(est - R) < 0.05f);

    /* tight circle R=0.5r: q<1 everywhere -> fixed-count mode, no normal points */
    Sampler tz;
    smp_init(&tz, 1.0f, SMP_CMIN, 4);
    float Rt = 0.5f;
    for (int i = 0; i <= 600; i++) {
        float ang = 4.0f * 0.005f * (float)i;
        float x = Rt * cosf(ang), y = Rt * sinf(ang);
        float vx = -Rt * 4.0f * sinf(ang), vy = Rt * 4.0f * cosf(ang);
        float ax = -Rt * 16.0f * cosf(ang), ay = -Rt * 16.0f * sinf(ang);
        smp_update_va(&tz, x, y, vx, vy, ax, ay);
    }
    smp_finish(&tz);
    check("zone_detected", tz.zone_entries >= 1);
    check("zone_fixed_count", tz.zone_emitted == tz.zone_entries * 4);
    check("zone_no_break", tz.breaks == 0);

    /* coefficient budget rule */
    check("pick_clamps_low", fabsf(smp_pick_coef(10.0f, 1.0f, 1000) - SMP_CMIN) < 1e-6f);
    check("pick_clamps_high", fabsf(smp_pick_coef(1000.0f, 1.0f, 10) - SMP_CMAX) < 1e-6f);
    check("pick_budget", fabsf(smp_pick_coef(20.0f, 1.0f, 10) - 2.0f) < 1e-6f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
#endif
