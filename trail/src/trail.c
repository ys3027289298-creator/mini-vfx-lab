
/* trail: sword-tip ribbon.
 *
 * Points are recorded by TIP POSITION (uniform arc-length spacing), not by
 * fixed time interval: equal-time sampling puts points close together in the
 * slow parts of a cut and yards apart through the fast middle, so the polyline
 * chords straight across the curve and detaches from a curved blade.
 *
 * Width decays with age through a speed-derived envelope:
 *   q    = clamp(v / v_ref, qmin, qmax)       local tip-speed ratio
 *   w0   = base * min(1, q^-gamma)             fast part -> thinner
 *   life = max_age * clamp(q^beta, lmin, lmax)  fast part -> decays slower
 * The arc length covered while a point decays is ~= v * life, which grows
 * strongly with v, so fast parts also read as longer tails on screen.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAXT 48

typedef struct { float x, y, age, speed; } TP;
typedef struct { TP p[MAXT]; int n; } Trail;

static int tr_push_v(Trail* t, float x, float y, float speed) {
    if (t->n >= MAXT) {
        memmove(&t->p[0], &t->p[1], sizeof(TP) * (MAXT - 1));
        t->n = MAXT - 1;
    }
    t->p[t->n].x = x; t->p[t->n].y = y;
    t->p[t->n].age = 0.0f; t->p[t->n].speed = speed;
    return t->n++;
}

static int tr_push(Trail* t, float x, float y) {
    return tr_push_v(t, x, y, 0.0f);
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

/* ---- position-based (arc-length gated) sampling ----------------------- */

typedef struct {
    int have;
    float cur_x, cur_y;     /* latest raw tip observation = live head */
    float cur_speed;
} TrSampler;

static void tr_sampler_init(TrSampler* s) {
    memset(s, 0, sizeof(*s));
}

/* Feed one raw tip observation. Fixed trail points are emitted so that
 * consecutive trail points are exactly `spacing` world units apart along the
 * traveled polyline (interpolated inside long segments). The raw observation
 * itself is kept as the live head in s->cur_* and is NOT pushed until a full
 * spacing has accumulated. Returns trail count, or -1 on bad spacing. */
static int tr_sample(Trail* t, TrSampler* s,
                     float x, float y, float speed, float spacing) {
    if (!(spacing > 0.0f)) return -1;
    if (!s->have || t->n == 0) {
        tr_push_v(t, x, y, speed);
        s->have = 1;
    }
    for (int guard = 0; guard < MAXT + 2; guard++) {
        TP* last = &t->p[t->n - 1];
        float dx = x - last->x, dy = y - last->y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < spacing) break;
        float nx = last->x + dx / d * spacing;
        float ny = last->y + dy / d * spacing;
        tr_push_v(t, nx, ny, speed);
    }
    s->cur_x = x; s->cur_y = y; s->cur_speed = speed;
    return t->n;
}

/* ---- speed-derived age width envelope -------------------------------- */

typedef struct {
    float max_age;  /* reference life at q == 1 (seconds) */
    float v_ref;    /* reference tip speed (units/s) */
    float gamma;    /* birth width scales as q^-gamma (fast -> thin) */
    float beta;     /* lifetime scales as q^beta (fast -> age slower) */
    float power;    /* age taper exponent, 1 = linear */
    float qmin, qmax;
    float life_min, life_max; /* lifetime clamp factors on max_age */
} WidthModel;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Lifetime in seconds for point i under the speed-derived model. */
static float tr_life(const Trail* t, int i, const WidthModel* m) {
    if (t->n <= 0 || i < 0 || i >= t->n || !m || m->v_ref <= 0.0f) return -1.0f;
    float q = clampf(t->p[i].speed / m->v_ref, m->qmin, m->qmax);
    float lf = clampf(powf(q, m->beta), m->life_min, m->life_max);
    return m->max_age * lf;
}

/* Full ribbon width at point i's current age. base is the full width at the
 * reference speed and age 0. */
static float tr_width_speed(const Trail* t, int i, float base,
                            const WidthModel* m, float* life_out) {
    if (t->n <= 0 || i < 0 || i >= t->n || !m || m->v_ref <= 0.0f) return -1.0f;
    float life = tr_life(t, i, m);
    if (life_out) *life_out = life;
    float q = clampf(t->p[i].speed / m->v_ref, m->qmin, m->qmax);
    float birth = base * fminf(1.0f, powf(q, -m->gamma)); /* fast -> thin */
    float k = clampf(1.0f - t->p[i].age / life, 0.0f, 1.0f);
    return birth * powf(k, m->power);
}

/* Ribbon vertex row: center, side offsets and diagnostic channels. */
typedef struct {
    float cx, cy, lx, ly, rx, ry;
    float width, age, speed, life;
} RV;

/* Build the renderable ribbon (oldest -> newest, live raw tip last).
 * Returns number of rows written (<= cap). */
static int tr_ribbon(const Trail* t, const TrSampler* s, float base,
                     const WidthModel* m, RV* out, int cap) {
    int rows = t->n + 1; /* + live head */
    if (rows > cap) rows = cap;
    for (int i = 0; i < t->n && i < cap; i++) {
        float life = 0.0f;
        float w = tr_width_speed(t, i, base, m, &life);
        float tx = 1.0f, ty = 0.0f;
        if (t->n == 1) {
            tx = 1.0f; ty = 0.0f;
        } else if (i == 0) {
            tx = t->p[1].x - t->p[0].x; ty = t->p[1].y - t->p[0].y;
        } else if (i == t->n - 1) {
            tx = t->p[i].x - t->p[i - 1].x; ty = t->p[i].y - t->p[i - 1].y;
        } else {
            tx = t->p[i + 1].x - t->p[i - 1].x;
            ty = t->p[i + 1].y - t->p[i - 1].y;
        }
        float tl = sqrtf(tx * tx + ty * ty);
        if (tl < 1e-7f) { tx = 1.0f; ty = 0.0f; tl = 1.0f; }
        float nx = -ty / tl, ny = tx / tl, h = 0.5f * w;
        out[i].cx = t->p[i].x; out[i].cy = t->p[i].y;
        out[i].lx = t->p[i].x + nx * h; out[i].ly = t->p[i].y + ny * h;
        out[i].rx = t->p[i].x - nx * h; out[i].ry = t->p[i].y - ny * h;
        out[i].width = w; out[i].age = t->p[i].age;
        out[i].speed = t->p[i].speed; out[i].life = life;
    }
    /* live head row */
    if (cap >= t->n + 1 && s->have) {
        int i = t->n;
        float q = clampf(s->cur_speed / m->v_ref, m->qmin, m->qmax);
        float life = m->max_age * clampf(powf(q, m->beta), m->life_min, m->life_max);
        float w = base * fminf(1.0f, powf(q, -m->gamma));
        float tx = 1.0f, ty = 0.0f;
        if (t->n > 0) {
            tx = s->cur_x - t->p[t->n - 1].x;
            ty = s->cur_y - t->p[t->n - 1].y;
            float tl = sqrtf(tx * tx + ty * ty);
            if (tl > 1e-7f) { tx /= tl; ty /= tl; } else { tx = 1.0f; ty = 0.0f; }
        }
        float nx = -ty, ny = tx, h = 0.5f * w;
        out[i].cx = s->cur_x; out[i].cy = s->cur_y;
        out[i].lx = s->cur_x + nx * h; out[i].ly = s->cur_y + ny * h;
        out[i].rx = s->cur_x - nx * h; out[i].ry = s->cur_y - ny * h;
        out[i].width = w; out[i].age = 0.0f;
        out[i].speed = s->cur_speed; out[i].life = life;
    }
    return rows;
}

/* ====================================================================== */
/* Curved sword + slash simulation (single source of geometry for dumps). */
/* ====================================================================== */

#define SIM_DT     (1.0f/240.0f)
#define SLASH_DUR  0.95f
#define PIVOT_X    0.0f
#define PIVOT_Y    0.0f
#define GRIP_LEN   0.30f   /* pommel/pivot -> guard */
#define BLADE_LEN  1.00f   /* guard -> tip along the arc */
#define CURVE_R    2.05f   /* blade arc radius (curved dao) */
#define GUARD_HW   0.030f  /* blade half width at guard */
#define PHI0      -0.82f   /* start pose angle */
#define DPHI       1.38f   /* total rotation CCW */
#define LUNGE_X    0.34f   /* hand translation over the cut */
#define DIP_Y     -0.16f   /* middle hand dip */
#define RECORD_SPACING 0.115f  /* coarse log: ~17 tip points over one cut */
#define RIBBON_SPACING 0.045f  /* dense position gate for a smooth ribbon */
#define HARD_AGE   0.35f   /* memory bound for tr_age */
#define OLD_DT     0.105f  /* legacy equal-time sampling interval */

#define BLADE_SEG  30
#define BLADE_NV   (2 * (BLADE_SEG + 1))
#define MAX_FRAMES 8

typedef struct { float x, y; } V2;

static float ease3(float u) { return u * u * (3.0f - 2.0f * u); }

/* Hand/pivot translation during the cut. Both components have zero endpoint
 * velocity so the tip path has no startup hook/cusp. */
static V2 hand_at(float u) {
    V2 h;
    h.x = PIVOT_X + LUNGE_X * ease3(u);
    float s = sinf((float)M_PI * u);
    h.y = PIVOT_Y + DIP_Y * s * s;
    return h;
}

static float phi_at(float u) { return PHI0 + DPHI * ease3(u); }

/* Curved blade centerline, local frame, s in [0, BLADE_LEN] from guard. */
static V2 blade_cl(float s) {
    float k = 1.0f / CURVE_R;
    float a = k * s;
    V2 p;
    p.x = GRIP_LEN + sinf(a) / k;
    p.y = (1.0f - cosf(a)) / k;
    return p;
}

static float blade_hw(float s) {
    float u = s / BLADE_LEN;
    /* broad belly, quick tapered point */
    return GUARD_HW * powf(fmaxf(0.0f, 1.0f - u), 0.55f) + 0.0012f;
}

static V2 rotate2(V2 p, float phi) {
    float c = cosf(phi), si = sinf(phi);
    V2 r = { p.x * c - p.y * si, p.x * si + p.y * c };
    return r;
}

static V2 tip_at(float t) {
    float u = clampf(t / SLASH_DUR, 0.0f, 1.0f);
    V2 h = hand_at(u);
    V2 loc = blade_cl(BLADE_LEN);
    V2 r = rotate2(loc, phi_at(u));
    V2 w = { h.x + r.x, h.y + r.y };
    return w;
}

/* Tip speed by central finite difference on the analytic tip path. */
static float speed_at(float t) {
    float e = SIM_DT;
    float t0 = clampf(t - e, 0.0f, SLASH_DUR);
    float t1 = clampf(t + e, 0.0f, SLASH_DUR);
    V2 a = tip_at(t0), b = tip_at(t1);
    float dt = t1 - t0;
    if (dt <= 0.0f) return 0.0f;
    return hypotf(b.x - a.x, b.y - a.y) / dt;
}

/* Full blade outline polygon (BLADE_NV verts) at time t. */
static int blade_poly(float t, V2* poly, int cap) {
    float u = clampf(t / SLASH_DUR, 0.0f, 1.0f);
    V2 h = hand_at(u);
    float phi = phi_at(u), k = 1.0f / CURVE_R;
    int m = BLADE_SEG + 1;
    if (cap < 2 * m) return -1;
    for (int j = 0; j < m; j++) {
        float s = BLADE_LEN * (float)j / BLADE_SEG;
        float a = k * s;
        V2 c = blade_cl(s);
        float hw = blade_hw(s);
        V2 n = { -sinf(a), cosf(a) };
        V2 up = { c.x + n.x * hw, c.y + n.y * hw };
        V2 wu = rotate2(up, phi);
        poly[j].x = h.x + wu.x; poly[j].y = h.y + wu.y;
    }
    for (int j = 0; j < m; j++) {
        float s = BLADE_LEN * (1.0f - (float)j / BLADE_SEG);
        float a = k * s;
        V2 c = blade_cl(s);
        float hw = blade_hw(s);
        V2 n = { -sinf(a), cosf(a) };
        V2 dn = { c.x - n.x * hw, c.y - n.y * hw };
        V2 wd = rotate2(dn, phi);
        poly[m + j].x = h.x + wd.x; poly[m + j].y = h.y + wd.y;
    }
    return 2 * m;
}

/* Grip rectangle + guard segment at time t (for rendering). */
static void fittings_at(float t, V2 grip[4], V2 guard[2]) {
    float u = clampf(t / SLASH_DUR, 0.0f, 1.0f);
    V2 h = hand_at(u);
    float phi = phi_at(u);
    V2 loc[4] = {
        { 0.00f, -0.018f }, { GRIP_LEN + 0.02f, -0.020f },
        { GRIP_LEN + 0.02f,  0.020f }, { 0.00f,  0.018f }
    };
    for (int i = 0; i < 4; i++) {
        V2 r = rotate2(loc[i], phi);
        grip[i].x = h.x + r.x; grip[i].y = h.y + r.y;
    }
    V2 g0 = { GRIP_LEN, -0.085f }, g1 = { GRIP_LEN, 0.085f };
    V2 r0 = rotate2(g0, phi), r1 = rotate2(g1, phi);
    guard[0].x = h.x + r0.x; guard[0].y = h.y + r0.y;
    guard[1].x = h.x + r1.x; guard[1].y = h.y + r1.y;
}

/* ---- modes: points table --------------------------------------------- */

static void print_points_csv(void) {
    /* Record pass: uniform arc-length tip positions over one full cut. */
    Trail rec; memset(&rec, 0, sizeof(rec));
    TrSampler rs; tr_sampler_init(&rs);
    int steps = (int)(SLASH_DUR / SIM_DT + 0.5f);
    printf("i,x,y,speed\n");
    for (int s = 0; s <= steps; s++) {
        float t = SLASH_DUR * (float)s / steps;
        V2 p = tip_at(t);
        tr_sample(&rec, &rs, p.x, p.y, speed_at(t), RECORD_SPACING);
    }
    for (int i = 0; i < rec.n; i++)
        printf("%d,%.5f,%.5f,%.4f\n", i,
               rec.p[i].x, rec.p[i].y, rec.p[i].speed);
    printf("%d,%.5f,%.5f,%.4f\n", rec.n, rs.cur_x, rs.cur_y, rs.cur_speed);
    fprintf(stderr, "tip records: %d fixed + live head, spacing=%.3f\n",
            rec.n, RECORD_SPACING);
}

/* ---- mode: full JSON dump -------------------------------------------- */

typedef struct {
    int used;
    float t;
    V2 blade[BLADE_NV]; int bn;
    V2 grip[4]; V2 guard[2];
    RV rows[MAXT + 1]; int nr;
} FrameSnap;

static void print_dump_json(void) {
    WidthModel wm = {
        /*max_age*/ 0.16f, /*v_ref*/ 2.00f, /*gamma*/ 1.25f,
        /*beta*/ 0.90f, /*power*/ 1.20f,
        /*qmin*/ 0.30f, /*qmax*/ 1.60f,
        /*life_min*/ 0.80f, /*life_max*/ 1.60f
    };
    const float base_width = 0.050f;
    int steps = (int)(SLASH_DUR / SIM_DT + 0.5f);

    float frame_u[MAX_FRAMES] = { 0.10f, 0.22f, 0.34f, 0.46f,
                                  0.56f, 0.68f, 0.80f, 0.93f };
    FrameSnap snaps[MAX_FRAMES];
    memset(snaps, 0, sizeof(snaps));
    for (int f = 0; f < MAX_FRAMES; f++) {
        int fs = (int)lroundf(frame_u[f] * steps);
        snaps[f].used = 1;
        snaps[f].t = SLASH_DUR * (float)fs / steps;
    }

    /* legacy equal-time samples over the whole cut (diagnostic) */
    V2 oldpts[64]; float oldt[64]; int nold = 0;
    for (float t = 0.0f; t <= SLASH_DUR + 1e-6f && nold < 64; t += OLD_DT) {
        oldt[nold] = t; oldpts[nold++] = tip_at(t);
    }

    printf("{\n \"params\": {\"sim_dt\":%.6f,\"slash_dur\":%.4f,"
           "\"record_spacing\":%.4f,\"ribbon_spacing\":%.4f,"
           "\"hard_age\":%.3f,\"old_dt\":%.4f,"
           "\"base_width\":%.4f,\"v_ref\":%.3f,\"gamma\":%.3f,"
           "\"beta\":%.3f,\"power\":%.3f,\"max_age\":%.3f,"
           "\"curve_r\":%.3f,\"blade_len\":%.3f,\"grip_len\":%.3f,"
           "\"phi0\":%.4f,\"dphi\":%.4f},\n",
           SIM_DT, SLASH_DUR, RECORD_SPACING, RIBBON_SPACING,
           HARD_AGE, OLD_DT, base_width,
           wm.v_ref, wm.gamma, wm.beta, wm.power, wm.max_age,
           CURVE_R, BLADE_LEN, GRIP_LEN, PHI0, DPHI);

    /* dense true tip curve + speed profile */
    printf(" \"dense\": [\n");
    for (int s = 0; s <= steps; s++) {
        float t = SLASH_DUR * (float)s / steps;
        V2 p = tip_at(t);
        printf("  {\"t\":%.5f,\"x\":%.5f,\"y\":%.5f,\"v\":%.5f}%s\n",
               t, p.x, p.y, speed_at(t), s == steps ? "" : ",");
    }
    printf(" ],\n");

    /* legacy equal-time points */
    printf(" \"time_samples\": [\n");
    for (int i = 0; i < nold; i++)
        printf("  {\"t\":%.4f,\"x\":%.5f,\"y\":%.5f}%s\n", oldt[i],
               oldpts[i].x, oldpts[i].y, i == nold - 1 ? "" : ",");
    printf(" ],\n");

    /* full-slash position records (the dozen+ coordinates) */
    Trail rec; memset(&rec, 0, sizeof(rec));
    TrSampler rs0; tr_sampler_init(&rs0);
    for (int s = 0; s <= steps; s++) {
        float t = SLASH_DUR * (float)s / steps;
        V2 p = tip_at(t);
        tr_sample(&rec, &rs0, p.x, p.y, speed_at(t), RECORD_SPACING);
    }
    printf(" \"tip_records\": [\n");
    for (int i = 0; i < rec.n; i++)
        printf("  {\"i\":%d,\"x\":%.5f,\"y\":%.5f,\"v\":%.5f},\n", i,
               rec.p[i].x, rec.p[i].y, rec.p[i].speed);
    printf("  {\"i\":%d,\"x\":%.5f,\"y\":%.5f,\"v\":%.5f,\"head\":1}\n",
           rec.n, rs0.cur_x, rs0.cur_y, rs0.cur_speed);
    printf(" ],\n");

    /* aged-trail pass: stream swept poses, capture frame snapshots */
    Trail tr; memset(&tr, 0, sizeof(tr));
    TrSampler sm; tr_sampler_init(&sm);

    printf(" \"poses\": [\n");
    for (int s = 0; s <= steps; s++) {
        float t = SLASH_DUR * (float)s / steps;
        tr_age(&tr, SIM_DT, HARD_AGE);
        V2 tip = tip_at(t);
        tr_sample(&tr, &sm, tip.x, tip.y, speed_at(t), RIBBON_SPACING);

        V2 poly[BLADE_NV];
        int vn = blade_poly(t, poly, BLADE_NV);
        printf("  {\"t\":%.5f,\"poly\":[", t);
        for (int j = 0; j < vn; j++)
            printf("[%.4f,%.4f]%s", poly[j].x, poly[j].y,
                   j == vn - 1 ? "" : ",");
        printf("]}%s\n", s == steps ? "" : ",");

        for (int f = 0; f < MAX_FRAMES; f++) {
            if (!snaps[f].used || fabsf(snaps[f].t - t) > 0.5f * SIM_DT)
                continue;
            snaps[f].bn = blade_poly(t, snaps[f].blade, BLADE_NV);
            fittings_at(t, snaps[f].grip, snaps[f].guard);
            snaps[f].nr = tr_ribbon(&tr, &sm, base_width, &wm,
                                    snaps[f].rows, MAXT + 1);
            snaps[f].used = 2; /* captured */
        }
    }
    printf(" ],\n");

    printf(" \"frames\": [\n");
    for (int f = 0; f < MAX_FRAMES; f++) {
        FrameSnap* z = &snaps[f];
        printf("  {\"t\":%.5f,\"u\":%.3f,\n", z->t, z->t / SLASH_DUR);
        printf("   \"blade\":[");
        for (int j = 0; j < z->bn; j++)
            printf("[%.4f,%.4f]%s", z->blade[j].x, z->blade[j].y,
                   j == z->bn - 1 ? "" : ",");
        printf("],\n   \"grip\":[");
        for (int j = 0; j < 4; j++)
            printf("[%.4f,%.4f]%s", z->grip[j].x, z->grip[j].y,
                   j == 3 ? "" : ",");
        printf("],\n   \"guard\":[[%.4f,%.4f],[%.4f,%.4f]],\n",
               z->guard[0].x, z->guard[0].y, z->guard[1].x, z->guard[1].y);
        printf("   \"ribbon\":[\n");
        for (int r = 0; r < z->nr; r++) {
            RV* q = &z->rows[r];
            printf("    {\"cx\":%.5f,\"cy\":%.5f,\"lx\":%.5f,\"ly\":%.5f,"
                   "\"rx\":%.5f,\"ry\":%.5f,\"w\":%.5f,\"age\":%.4f,"
                   "\"v\":%.4f,\"life\":%.4f}%s\n",
                   q->cx, q->cy, q->lx, q->ly, q->rx, q->ry,
                   q->width, q->age, q->speed, q->life,
                   r == z->nr - 1 ? "" : ",");
        }
        printf("   ]}%s\n", f == MAX_FRAMES - 1 ? "" : ",");
    }
    printf(" ],\n"); /* end frames */

    /* full per-frame ribbon boundary track, so verification can cover every
       simulated frame rather than only the rendered snapshots */
    printf(" \"track\": [\n");
    Trail tt; memset(&tt, 0, sizeof(tt));
    TrSampler tsm; tr_sampler_init(&tsm);
    for (int s = 0; s <= steps; s++) {
        float t = SLASH_DUR * (float)s / steps;
        tr_age(&tt, SIM_DT, HARD_AGE);
        V2 tip = tip_at(t);
        tr_sample(&tt, &tsm, tip.x, tip.y, speed_at(t), RIBBON_SPACING);
        RV rows[MAXT + 1];
        int nr = tr_ribbon(&tt, &tsm, base_width, &wm, rows, MAXT + 1);
        V2 tgrip[4], tguard[2];
        fittings_at(t, tgrip, tguard);
        printf("  {\"t\":%.5f,\"L\":[", t);
        for (int r = 0; r < nr; r++)
            printf("[%.4f,%.4f]%s", rows[r].lx, rows[r].ly,
                   r == nr - 1 ? "" : ",");
        printf("],\"R\":[");
        for (int r = 0; r < nr; r++)
            printf("[%.4f,%.4f]%s", rows[r].rx, rows[r].ry,
                   r == nr - 1 ? "" : ",");
        printf("],\"W\":[");
        for (int r = 0; r < nr; r++)
            printf("%.4f%s", rows[r].width, r == nr - 1 ? "" : ",");
        printf("],\"grip\":[");
        for (int j = 0; j < 4; j++)
            printf("[%.4f,%.4f]%s", tgrip[j].x, tgrip[j].y,
                   j == 3 ? "" : ",");
        printf("],\"guard\":[[%.4f,%.4f],[%.4f,%.4f]]}%s\n",
               tguard[0].x, tguard[0].y, tguard[1].x, tguard[1].y,
               s == steps ? "" : ",");
    }
    printf(" ]\n}\n");
}

/* ---- self tests ------------------------------------------------------- */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static void run_tests(void) {
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

    /* position-based sampling: exact uniform gaps */
    Trail s1; memset(&s1, 0, sizeof(s1));
    TrSampler sm1; tr_sampler_init(&sm1);
    tr_sample(&s1, &sm1, 0, 0, 1.0f, 1.0f);
    check("sample_first", s1.n == 1);
    tr_sample(&s1, &sm1, 0.6f, 0, 1.0f, 1.0f);
    check("sample_under_gap", s1.n == 1);
    tr_sample(&s1, &sm1, 1.2f, 0, 1.2f, 1.0f);
    check("sample_gap_emit", s1.n == 2 &&
          fabsf(s1.p[1].x - 1.0f) < 1e-5f);
    check("sample_live_head", fabsf(sm1.cur_x - 1.2f) < 1e-5f);
    tr_sample(&s1, &sm1, 2.4f, 0, 1.4f, 1.0f);
    check("sample_second_gap", s1.n == 3 &&
          fabsf(s1.p[2].x - 2.0f) < 1e-5f);
    check("sample_speed_kept", fabsf(s1.p[2].speed - 1.4f) < 1e-5f);
    check("sample_bad_spacing", tr_sample(&s1, &sm1, 3, 0, 1, 0) == -1);

    /* curved walk: emitted gaps equal spacing and polyline hugs the circle */
    Trail sc; memset(&sc, 0, sizeof(sc));
    TrSampler smc; tr_sampler_init(&smc);
    float R = 5.0f, sp = 0.35f;
    for (int k = 0; k <= 400; k++) {
        float a = (float)k / 400.0f * 1.2f;
        tr_sample(&sc, &smc, R * cosf(a), R * sinf(a), 2.0f, sp);
    }
    float gap_err = 0.0f, max_sag = 0.0f;
    for (int i = 1; i < sc.n; i++) {
        float dx = sc.p[i].x - sc.p[i - 1].x;
        float dy = sc.p[i].y - sc.p[i - 1].y;
        gap_err = fmaxf(gap_err, fabsf(sqrtf(dx * dx + dy * dy) - sp));
        float mx = 0.5f * (sc.p[i].x + sc.p[i - 1].x);
        float my = 0.5f * (sc.p[i].y + sc.p[i - 1].y);
        max_sag = fmaxf(max_sag, R - hypotf(mx, my)); /* chord sagitta */
    }
    check("sample_uniform_gaps", gap_err < 1e-4f);
    check("sample_curve_hug", max_sag <= sp * sp / (8.0f * R) + 1e-3f);

    /* speed-derived width envelope */
    WidthModel m = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                     0.1f, 4.0f, 0.5f, 2.0f };
    Trail ws; memset(&ws, 0, sizeof(ws));
    tr_push_v(&ws, 0, 0, 2.0f); /* idx0 fast q=2 */
    tr_push_v(&ws, 1, 0, 0.5f); /* idx1 slow q=.5 */
    float life_fast = 0, life_slow = 0;
    float wf0 = tr_width_speed(&ws, 0, 1.0f, &m, &life_fast);
    float ws0 = tr_width_speed(&ws, 1, 1.0f, &m, &life_slow);
    check("width_fast_thinner", wf0 < ws0 - 1e-6f);
    check("life_fast_longer", life_fast > life_slow + 1e-6f);
    check("width_birth_values",
          fabsf(wf0 - 0.5f) < 1e-5f && fabsf(ws0 - 1.0f) < 1e-5f);
    ws.p[0].age = 0.8f; ws.p[1].age = 0.8f;
    float wf8 = tr_width_speed(&ws, 0, 1.0f, &m, NULL);
    float ws8 = tr_width_speed(&ws, 1, 1.0f, &m, NULL);
    check("width_age_decays", wf8 < wf0 && ws8 == 0.0f);
    check("width_bad_index", tr_width_speed(&ws, 9, 1.0f, &m, NULL) < 0);

    /* tip path of the modelled cut is measurably curved, not a straight line */
    V2 p0 = tip_at(0.0f), p1 = tip_at(SLASH_DUR * 0.5f), p2 = tip_at(SLASH_DUR);
    V2 ab = { p2.x - p0.x, p2.y - p0.y };
    float cross = fabsf(ab.x * (p1.y - p0.y) - ab.y * (p1.x - p0.x));
    float chord = hypotf(ab.x, ab.y);
    check("tip_path_curved", cross / chord > 0.05f);

    /* one full cut records a dozen-plus position samples */
    Trail rc; memset(&rc, 0, sizeof(rc));
    TrSampler rcs; tr_sampler_init(&rcs);
    int steps = (int)(SLASH_DUR / SIM_DT + 0.5f);
    for (int s = 0; s <= steps; s++) {
        float t = SLASH_DUR * (float)s / steps;
        V2 p = tip_at(t);
        tr_sample(&rc, &rcs, p.x, p.y, speed_at(t), RECORD_SPACING);
    }
    int total_records = rc.n + 1; /* plus live head */
    check("tip_records_dozen", total_records >= 13 && total_records <= 24);

    printf("all_ok=%d\n", failures == 0);
    if (failures) exit(1);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "points") == 0) {
        print_points_csv();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        print_dump_json();
        return 0;
    }
    run_tests();
    return failures == 0 ? 0 : 1;
}
