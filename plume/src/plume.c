
/* plume: wind-bent smoke column.

   A parcel rises at constant speed w through a wind that grows with height,
   u(z) = U (z/H)^n.  It keeps the horizontal velocity it picked up on the way
   up, so the deflection is an integral over height, not a single product:

       x(z) = INT_0^z u(z')/w dz' = H a /(n+1) * s^(n+1),
       s = z/H,   a = U/w.

   Curvature of that centre line:

       kappa(s) = (a n / H) s^(n-1) (1 + a^2 s^(2n))^(-3/2)

   Zero at the base, peaking where a^2 s^(2n) = (n-1)/(2n+1), falling off as
   1/z above.  The bend the eye latches onto is that peak:

       s* = [ (n-1) / (a^2 (2n+1)) ]^(1/(2n)),   z* = H s*.

   Two things fall out of that peak.  It only exists for n > 1 -- at n = 1 the
   wind is linear and kappa never turns over, the column just keeps curving --
   and it only sits inside the column while a^2 >= (n-1)/(2n+1), i.e. while
   the top wind is strong enough to have finished turning the column by H.
   Outside that the sharpest point is at the top edge.

   Run backwards, a measured kink height sets the exponent: n is the root of
   s*^(2n) = (n-1)/(a^2 (2n+1)).  With a = U/w = 4 the whole n window [2,6]
   maps onto s* in [0.33, 0.73]: the kink height constrains n well but not
   tightly, because it moves only ~0.08 per unit n near n = 4.

   n on its own is taste.  Large n leaves the base straight and whips the top
   over; small n leans the whole column evenly and it stops reading as a plume.
   Two curves over n settle it -- kink sharpness (peak curvature in units of
   total turn, rising) against chord tilt atan(a/(n+1)) in degrees (falling) --
   and the pick is where their normalised versions cross.  */

#include <stdio.h>
#include <string.h>
#include <math.h>

#define N_MIN 2.0f
#define N_MAX 6.0f
#define N_REF 4.016012f      /* crossing of the two selection curves at a = 4 */
#define SELECT_SAMPLES 20001
#define BEND_LAYERS 512
#define PL_MAX_LAYER 4096
#define DEG 57.2957795f

static double xline[PL_MAX_LAYER + 1];

typedef struct {
    float H;   /* column height */
    float U;   /* wind at the top of the column */
    float w;   /* constant rise speed */
    float n;   /* wind profile exponent, u = U (z/H)^n */
} Plume;

static int pl_init(Plume* p, float H, float U, float w, float n) {
    if (!(H > 0.0f) || !(w > 0.0f) || !(n >= 0.0f) || U < 0.0f) return -1;
    p->H = H; p->U = U; p->w = w; p->n = n;
    return 0;
}

/* Horizontal deflection at height z: the height integral of u/w. */
static float pl_bend(const Plume* p, float z) {
    if (z <= 0.0f) return 0.0f;
    float s = z / p->H;
    float a = p->U / p->w;
    return p->H * a * powf(s, p->n + 1.0f) / (p->n + 1.0f);
}

/* The integral done layer by layer, as an integrator would: each layer adds
   its own wind times the time spent rising through it. */
static float pl_bend_layers(const Plume* p, float z, int layers) {
    if (layers < 1 || z <= 0.0f) return 0.0f;
    float dz = z / (float)layers;
    float x = 0.0f;
    for (int i = 0; i < layers; i++) {
        float zc = ((float)i + 0.5f) * dz;
        float u = p->U * powf(zc / p->H, p->n);
        x += u * (dz / p->w);
    }
    return x;
}

/* Lagrangian run: a parcel is carried up at w and collects u(z)/w sideways per
   unit of climb.  Written in the dimensionless variables, so it checks the
   a = U/w scaling as well as the integral. */
static float pl_bend_track(const Plume* p, float z, int steps) {
    if (steps < 1 || z <= 0.0f) return 0.0f;
    float s = z / p->H;
    float ds = s / (float)steps;
    float a = p->U / p->w;
    float x = 0.0f;
    for (int i = 0; i < steps; i++)
        x += a * powf(((float)i + 0.5f) * ds, p->n) * ds;
    return x * p->H;
}

/* Curvature of x(z), analytic. */
static float pl_curv(const Plume* p, float z) {
    if (z <= 0.0f) return 0.0f;
    float s = z / p->H;
    float a = p->U / p->w;
    float t = a * powf(s, p->n);
    return (a * p->n / p->H) * powf(s, p->n - 1.0f) / powf(1.0f + t * t, 1.5f);
}

/* Curvature of a bent centre line x(z), straight off three samples. */
static double pl_curv_from_x(double xm, double x0, double xp, double dz) {
    double d1 = (xp - xm) / (2.0 * dz);
    double d2 = (xp - 2.0 * x0 + xm) / (dz * dz);
    return fabs(d2) / pow(1.0 + d1 * d1, 1.5);
}

/* Normalised height of the curvature peak; -1 when no interior maximum. */
static float pl_kink_s(float n, float a) {
    if (!(n > 1.0f) || !(a > 0.0f)) return -1.0f;
    float s2n = (n - 1.0f) / (a * a * (2.0f * n + 1.0f));
    if (s2n > 1.0f) return -1.0f;
    return powf(s2n, 1.0f / (2.0f * n));
}

/* Selection curve 1: peak curvature relative to the total turning angle. */
static float pl_sharpness(float n, float a) {
    float s = pl_kink_s(n, a);
    if (s < 0.0f) return 0.0f;
    float kH = a * n * powf(s, n - 1.0f) * powf((2.0f * n + 1.0f) / (3.0f * n), 1.5f);
    return kH / atanf(a);
}

/* Selection curve 2: chord tilt in degrees, atan(a/(n+1)). */
static float pl_tilt_deg(float n, float a) {
    return atanf(a / (n + 1.0f)) * DEG;
}

/* Min/max of both curves over [n_lo, n_hi], used to normalise them. */
static void pl_select_spans(float a, float n_lo, float n_hi,
                            float* s0, float* s1, float* t0, float* t1) {
    *s0 = *t0 = HUGE_VALF;
    *s1 = *t1 = -HUGE_VALF;
    for (int i = 0; i < SELECT_SAMPLES; i++) {
        float n = n_lo + (n_hi - n_lo) * (float)i / (float)(SELECT_SAMPLES - 1);
        float s = pl_sharpness(n, a), t = pl_tilt_deg(n, a);
        if (s < *s0) *s0 = s;
        if (s > *s1) *s1 = s;
        if (t < *t0) *t0 = t;
        if (t > *t1) *t1 = t;
    }
}

/* Where the normalised curves cross, over [n_lo, n_hi]. */
static float pl_select_n(float a, float n_lo, float n_hi) {
    float s0, s1, t0, t1;
    pl_select_spans(a, n_lo, n_hi, &s0, &s1, &t0, &t1);
    float f_prev = 0.0f, n_prev = n_lo;
    for (int i = 0; i < SELECT_SAMPLES; i++) {
        float n = n_lo + (n_hi - n_lo) * (float)i / (float)(SELECT_SAMPLES - 1);
        float f = (pl_sharpness(n, a) - s0) / (s1 - s0)
                - (pl_tilt_deg(n, a) - t0) / (t1 - t0);
        if (i > 0 && f == 0.0f) return n;
        if (i > 0 && f_prev * f < 0.0f)
            return n_prev - f_prev * (n - n_prev) / (f - f_prev);
        f_prev = f;
        n_prev = n;
    }
    return -1.0f;
}

/* Invert the kink height: the n that puts the curvature peak at normalised
   height s.  g falls with n, so bisect. */
static float pl_exponent_from_s(float s, float a) {
    if (!(s > 0.0f) || !(s < 1.0f) || !(a > 0.0f)) return -1.0f;
    float lo = 1.0f + 1e-6f, hi = 60.0f;
    for (int it = 0; it < 200; it++) {
        float m = 0.5f * (lo + hi);
        float g = powf(s, 2.0f * m) - (m - 1.0f) / (a * a * (2.0f * m + 1.0f));
        if (g > 0.0f) lo = m; else hi = m;
    }
    return 0.5f * (lo + hi);
}

/* Lay the column out the way a solver would: one layer at a time, each layer
   adding wind * (time spent rising through it).  The node positions are
   computed in double and the second differences are read over a stencil
   several layers wide, because two deflections that close together differ by
   less than single float resolves. */
static void pl_build_line(const Plume* p, int nlayers, double* x) {
    float dz = p->H / (float)nlayers;
    x[0] = 0.0;
    for (int k = 0; k < nlayers; k++) {
        double zc = ((double)k + 0.5) * (double)dz;
        double u = (double)p->U * pow(zc / (double)p->H, (double)p->n);
        x[k + 1] = x[k] + u * (double)dz / (double)p->w;
    }
}

/* Sharpest point of a polyline sampled at equal spacing, with a parabola
   through the peak node and its neighbours to lift the answer off the grid. */
static float pl_kink_of_line(const double* x, int nodes, double dz) {
    int half = 4;
    if (nodes < 8 * half) half = nodes / 8;
    if (half < 1) half = 1;
    double bbest = -1.0, bprev = 0.0, bnext = 0.0;
    int bi = half;
    for (int k = half; k <= nodes - 1 - half; k++) {
        double kap = pl_curv_from_x(x[k - half], x[k], x[k + half],
                                    dz * (double)half);
        if (kap > bbest) { bbest = kap; bi = k; }
    }
    int have_prev = 0, have_next = 0;
    for (int side = -1; side <= 1; side += 2) {
        int k = bi + side;
        if (k - half < 0 || k + half >= nodes) continue;
        double kap = pl_curv_from_x(x[k - half], x[k], x[k + half],
                                    dz * (double)half);
        if (side < 0) { bprev = kap; have_prev = 1; }
        else { bnext = kap; have_next = 1; }
    }
    /* the parabola needs both neighbours; without them the grid point stands */
    if (!have_prev || !have_next) return (float)((double)bi * dz);
    double denom = bprev - 2.0 * bbest + bnext;
    double off = 0.0;
    if (denom < 0.0) off = 0.5 * (bprev - bnext) / denom;
    if (off > 0.5) off = 0.5;
    if (off < -0.5) off = -0.5;
    return (float)(((double)bi + off) * dz);
}

/* Measured kink off the rendered column. */
static float pl_kink_rendered(const Plume* p, int nlayers) {
    if (nlayers < 32) nlayers = 32;
    if (nlayers > PL_MAX_LAYER) nlayers = PL_MAX_LAYER;
    pl_build_line(p, nlayers, xline);
    return pl_kink_of_line(xline, nlayers + 1, (double)p->H / (double)nlayers);
}

/* Same measurement on a column advected forward in time: a parcel is carried
   up by w and pushed sideways by u(z) each dt, with no closed form anywhere.
   This is the check that the analytic bend and the rendered one agree with a
   solver that never heard of either. */
static float pl_kink_stepped(const Plume* p, float dt) {
    if (!(dt > 0.0f)) return 0.0f;
    int steps = (int)(p->H / (p->w * dt));
    if (steps < 32) steps = 32;
    if (steps > PL_MAX_LAYER - 1) steps = PL_MAX_LAYER - 1;
    double rise = (double)p->H / (double)steps;
    double step_dt = rise / (double)p->w;   /* the step the parcel actually takes */
    double x = 0.0, y = 0.0;
    xline[0] = 0.0;
    for (int k = 0; k < steps; k++) {
        /* midpoint: the wind over this step, not at its foot */
        double u = (double)p->U * pow((y + 0.5 * rise) / (double)p->H, (double)p->n);
        x += u * step_dt;
        y += rise;
        xline[k + 1] = x;
    }
    return pl_kink_of_line(xline, steps + 1, rise);
}

static int failures = 0, checks = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    checks++;
    if (!ok) failures++;
}

static void test_integral(void) {
    Plume p; if (pl_init(&p, 10.0f, 6.0f, 1.0f, 3.0f) < 0) { check("integral_valid", 0); return; }
    float closed = pl_bend(&p, 5.0f);
    float coarse = pl_bend_layers(&p, 5.0f, 4);
    float fine = pl_bend_layers(&p, 5.0f, BEND_LAYERS);
    float track = pl_bend_track(&p, 5.0f, 4096);
    float once = p.U * (5.0f / p.w);   /* one gust held for the whole climb */
    printf("integral: closed=%.6f coarse4=%.6f fine=%.6f track=%.6f once=%.6f\n",
           closed, coarse, fine, track, once);
    check("layers_converge_to_closed", fabsf(fine - closed) < fabsf(coarse - closed));
    check("layers_match_closed", fabsf(fine - closed) < 1e-3f * closed);
    check("track_matches_closed", fabsf(track - closed) < 1e-3f * closed);
    check("integral_not_single_product", fabsf(closed - once) > 0.5f * once);
    check("bend_monotone", pl_bend(&p, 9.0f) > pl_bend(&p, 5.0f));

    /* the closed form really is the integral: differentiate it back */
    float z = 0.3f * p.H, h = 1e-4f * p.H;
    float d = (pl_bend(&p, z + h) - pl_bend(&p, z - h)) / (2.0f * h);
    float u_over_w = (p.U / p.w) * powf(z / p.H, p.n);
    printf("integral: dxdz=%.6f u_over_w=%.6f\n", d, u_over_w);
    check("derivative_is_wind_over_rise", fabsf(d - u_over_w) < 1e-3f * u_over_w);
}

static void test_kink(void) {
    const float a = 4.0f;
    float n = pl_select_n(a, N_MIN, N_MAX);
    float s = pl_kink_s(n, a);
    Plume p; if (pl_init(&p, 20.0f, a, 1.0f, n) < 0) { check("plume_valid", 0); return; }
    check("plume_valid", 1);

    /* the layer-rendered column puts its sharpest point where s* says */
    float z_ren = pl_kink_rendered(&p, 1200);
    printf("kink: analytic_z=%.6f rendered_z=%.6f\n", p.H * s, z_ren);
    check("rendered_kink_matches", fabsf(z_ren - p.H * s) < 0.01f * p.H);

    /* and here is what the rendered profile looks like */
    printf("kink: z/H  curvature*H\n");
    for (int i = 1; i <= 10; i++) {
        float zz = 0.1f * (float)i * p.H;
        printf("kink: %.1f  %.6f\n", zz / p.H, pl_curv(&p, zz) * p.H);
    }

    check("curvature_zero_at_base", pl_curv(&p, 0.0f) == 0.0f);
    check("curvature_decays_on_top", pl_curv(&p, p.H) < pl_curv(&p, p.H * s));
    check("kink_inside_column", s > 0.05f && s < 0.95f);
    float eps = 1e-3f * p.H;
    check("kink_is_local_max",
          pl_curv(&p, p.H * s) > pl_curv(&p, p.H * s - eps) &&
          pl_curv(&p, p.H * s) > pl_curv(&p, p.H * s + eps));

    float rec = pl_exponent_from_s(s, a);
    printf("kink: n=%.6f recovered=%.6f\n", n, rec);
    check("exponent_round_trip", fabsf(rec - n) < 1e-3f);

    /* a forward time-stepped parcel, no closed form, finds the same kink */
    float z_step = pl_kink_stepped(&p, 1.0f / 240.0f);
    printf("kink: stepped_z=%.6f fast_dt=%.6f\n", z_step,
           pl_kink_stepped(&p, 1.0f / 60.0f));
    check("stepped_kink_matches", fabsf(z_step - p.H * s) < 0.01f * p.H);

    float worst = 0.0f;
    for (int i = 1; i <= 20; i++) {
        float nt = 2.0f + 4.0f * (float)i / 20.0f;
        float rt = pl_exponent_from_s(pl_kink_s(nt, a), a);
        if (fabsf(rt - nt) > worst) worst = fabsf(rt - nt);
    }
    printf("kink: inversion_worst_over_window=%.6f\n", worst);
    check("inversion_exact_over_window", worst < 1e-3f);
}

static void test_selection(void) {
    const float a = 4.0f;
    float s0, s1, t0, t1;
    pl_select_spans(a, N_MIN, N_MAX, &s0, &s1, &t0, &t1);
    float n = pl_select_n(a, N_MIN, N_MAX);
    float fs = (pl_sharpness(n, a) - s0) / (s1 - s0);
    float ft = (pl_tilt_deg(n, a) - t0) / (t1 - t0);

    printf("select: n  sharpness  sharp_norm  tilt_deg  tilt_norm\n");
    for (int i = 0; i <= 4; i++) {
        float nn = N_MIN + (N_MAX - N_MIN) * (float)i / 4.0f;
        printf("select: %.2f  %.6f  %.6f  %.4f  %.6f\n", nn,
               pl_sharpness(nn, a), (pl_sharpness(nn, a) - s0) / (s1 - s0),
               pl_tilt_deg(nn, a), (pl_tilt_deg(nn, a) - t0) / (t1 - t0));
    }
    printf("select: a=%.3f n=%.6f sharp_norm=%.6f tilt_norm=%.6f "
           "kink_s=%.6f kink_z_H=%.6f\n",
           a, n, fs, ft, pl_kink_s(n, a), pl_kink_s(n, a));
    printf("select: sharp=[%.6f..%.6f] tilt_deg=[%.4f..%.4f]\n", s0, s1, t0, t1);

    check("select_n_in_window", n > N_MIN && n < N_MAX);
    check("curves_cross", fabsf(fs - ft) < 1e-3f);
    check("sharpness_rises", pl_sharpness(N_MAX, a) > pl_sharpness(N_MIN, a));
    check("tilt_falls", pl_tilt_deg(N_MAX, a) < pl_tilt_deg(N_MIN, a));

    /* and the pick barely moves with a or with where the window is cut */
    float n4 = pl_select_n(6.0f, N_MIN, N_MAX);
    float nw = pl_select_n(a, 1.5f, 6.0f);
    printf("select: robustness a=6 n=%.6f  window[1.5,6] n=%.6f\n", n4, nw);
    check("select_stable_in_a", fabsf(n4 - n) < 0.5f);
    check("select_stable_in_window", fabsf(nw - n) < 0.4f);

    /* large n pins the bend high and upright; small n leans it evenly */
    check("large_n_kink_higher", pl_kink_s(N_MAX, a) > pl_kink_s(N_MIN, a));
    check("large_n_leans_less", pl_tilt_deg(N_MAX, a) < pl_tilt_deg(N_MIN, a));
}

static void test_sweep(void) {
    /* Three evenly spaced tiers, one exponent.  Each tier is rendered by the
       layer integrator, its kink measured, the exponent inverted from that
       kink, and the height that exponent predicts back.  The gate is a fifth
       of one tier height.  A breach names where the kink actually is. */
    const float a = 4.0f, w = 1.0f;
    const float bin = 8.0f;
    const float heights[3] = { 8.0f, 16.0f, 24.0f };
    float n = pl_select_n(a, N_MIN, N_MAX);
    float s_ref = pl_kink_s(n, a);
    float gate = 0.2f * bin;
    float worst = 0.0f;
    printf("sweep: n=%.6f s*=%.6f tier=%.0f gate=%.2f\n", n, s_ref, bin, gate);

    for (int i = 0; i < 3; i++) {
        float H = heights[i];
        Plume p; if (pl_init(&p, H, a * w, w, n) < 0) { check("sweep_valid", 0); continue; }

        float z_meas = pl_kink_rendered(&p, 1200);
        float z_ref = H * s_ref;                 /* where the picked n says */
        float n_back = pl_exponent_from_s(z_meas / H, a);
        float z_back = H * pl_kink_s(n_back, a); /* and back from the answer */
        float dev = fabsf(z_meas - z_ref);
        if (dev > worst) worst = dev;
        int ok = dev <= gate;
        printf("sweep: H=%.0f z_kink=%.6f (%.6f H) z_ref=%.6f n_back=%.6f "
               "z_back=%.6f dev=%.6f dev/%.1f=%.6f %s\n",
               H, z_meas, z_meas / H, z_ref, n_back, z_back, dev, bin, dev / bin,
               ok ? "ok" : "BREACH");
        /* always name the actual kink position, pass or fail */
        printf("sweep: H=%.0f actual_kink z=%.6f at %.3f of column\n",
               H, z_meas, z_meas / H);
        check("sweep_tier_within_gate", ok);
        check("sweep_tier_exponent", fabsf(n_back - n) < 1e-2f);
        check("sweep_tier_roundtrip", fabsf(z_back - z_meas) < 1e-3f);
    }
    printf("sweep: worst_dev=%.6f (%.6f of a tier)\n", worst, worst / bin);
    check("sweep_all_within_gate", worst <= gate);
    check("sweep_reference_n", fabsf(n - N_REF) < 2e-3f);
}

/* Gate breach: a test that missed the measured kink by more than the allowed
   fraction must print where it really was, not just fail.  Exercised on a
   deliberately wrong exponent so the reporting path is known to work. */
static void test_breach_report(void) {
    const float a = 4.0f, H = 16.0f, w = 1.0f;
    const float bin = 8.0f, gate = 0.2f * bin;
    /* n = 2 bends low; pretending the kink is still at s*(4.016) misses it by
       well over the gate, which is exactly the case the report exists for */
    Plume p; if (pl_init(&p, H, a * w, w, 2.0f) < 0) { check("breach_valid", 0); return; }
    float z_meas = pl_kink_rendered(&p, 1200);
    float z_claimed = H * pl_kink_s(N_REF, a);
    float dev = fabsf(z_meas - z_claimed);
    int breached = dev > gate;
    printf("breach: n_true=2.0 z_kink=%.6f z_claimed=%.6f dev=%.6f gate=%.6f %s\n",
           z_meas, z_claimed, dev, gate, breached ? "flagged" : "missed");
    check("breach_detected", breached);
    if (breached)
        printf("breach: actual kink H=%.0f z=%.6f at %.3f of column\n",
               H, z_meas, z_meas / H);
}

int main(void) {
    test_integral();
    test_kink();
    test_selection();
    test_sweep();
    test_breach_report();
    printf("checks=%d all_ok=%d\n", checks, failures == 0);
    return failures == 0 ? 0 : 1;
}
