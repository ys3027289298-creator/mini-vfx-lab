/* field: gravity, linear drag, swirl and wind integrated per step. */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define PI_F 3.14159265358979323846f
#define TRAJN 128

typedef struct { float x, y, vx, vy, wx, wy; } Part;

/*
 * Two different models, and they must not be confused:
 *
 *   (A) wind sampled once, at spawn  -> w(t) = w0 for the whole life
 *       x(T) = w0 * T                        (a straight line, no drag)
 *
 *   (B) wind sampled every frame     -> w(t) = w0 + alpha*t for a time-varying field
 *       x(T) = w0*T + alpha*T^2/2            (a parabola)
 *
 * Here the particle carries its wind vector, filled once at spawn, so (A) is
 * structural. The per-frame variant lives only in the shape probe below.
 *
 * Wind is the air velocity, not an acceleration. The stored (vx, vy) is the
 * velocity relative to the air: drag and swirl act on it, and the wind drift is
 * added only when the position is integrated. That is what keeps the lateral
 * displacement exactly wind * age even under drag. Adding wind to vx as an
 * acceleration would give w*T^2/2 instead.
 */
static void part_spawn(Part* p, float x, float y, float vx, float vy, float wx, float wy) {
    *p = (Part){ x, y, vx, vy, wx, wy };
}

/* core step: wind is an argument so the caller chooses when it is sampled. */
static int step_wind(Part* p, float dt, float gx, float gy, float drag, float swirl,
                     float wx, float wy) {
    if (dt < 0.0f || drag < 0.0f) return -1;
    /* drag applied as exponential decay of the relative velocity */
    float k = expf(-drag * dt);
    p->vx *= k; p->vy *= k;
    /* swirl: tangential acceleration around origin */
    float r = sqrtf(p->x * p->x + p->y * p->y);
    if (r > 1e-6f && swirl != 0.0f) {
        float tx = -p->y / r, ty = p->x / r;
        p->vx += tx * swirl * dt;
        p->vy += ty * swirl * dt;
    }
    p->vx += gx * dt;
    p->vy += gy * dt;
    /* ambient wind drifts the position; it is not stored in the relative velocity */
    p->x += (p->vx + wx) * dt;
    p->y += (p->vy + wy) * dt;
    return 0;
}

/* production path: the wind frozen at spawn, model (A). */
static int step(Part* p, float dt, float gx, float gy, float drag, float swirl) {
    return step_wind(p, dt, gx, gy, drag, swirl, p->wx, p->wy);
}

static float speed(const Part* p) { return sqrtf(p->vx * p->vx + p->vy * p->vy); }

static float drift_over_life(float wind, float life, float dt) {
    Part p;
    part_spawn(&p, 0, 0, 0, 0, wind, 0);
    int n = (int)(life / dt + 0.5f);
    for (int i = 0; i < n; i++) step(&p, dt, 0, 0, 0, 0);
    return p.x;
}

static void fit_line3(const double x[3], const double y[3], double* a, double* b) {
    double sx = x[0] + x[1] + x[2];
    double sy = y[0] + y[1] + y[2];
    double sxx = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    double sxy = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    double d = 3.0 * sxx - sx * sx;
    *a = (3.0 * sxy - sx * sy) / d;
    *b = (sy - *a * sx) / 3.0;
}

static double max_residual3(const double x[3], const double y[3], double a, double b) {
    double m = 0.0;
    for (int i = 0; i < 3; i++) {
        double r = fabs(y[i] - (a * x[i] + b));
        if (r > m) m = r;
    }
    return m;
}

/* --- trajectory shape probe: spawn-once vs per-frame wind sampling --- */

typedef struct { float x, y; } TPt;

/* per_frame=0: freeze the wind at spawn. per_frame=1: w(t)=w0+alpha*t. */
static int run_traj(TPt* pts, int per_frame, float ux, float uy,
                    float w0, float alpha, float dt, int steps) {
    if (steps + 1 > TRAJN) return -1;
    Part p;
    part_spawn(&p, 0, 0, ux, uy, w0, 0);
    pts[0] = (TPt){ p.x, p.y };
    for (int k = 0; k < steps; k++) {
        float wx = per_frame ? w0 + alpha * ((float)k * dt) : w0;
        if (step_wind(&p, dt, 0, 0, 0, 0, wx, 0) != 0) return -1;
        pts[k + 1] = (TPt){ p.x, p.y };
    }
    return steps + 1;
}

/* direction of a window of points, principal axis, modulo pi */
static float window_angle(const TPt* p, int from, int count) {
    double mx = 0, my = 0;
    for (int i = 0; i < count; i++) { mx += p[from + i].x; my += p[from + i].y; }
    mx /= count; my /= count;
    double sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < count; i++) {
        double dx = p[from + i].x - mx, dy = p[from + i].y - my;
        sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
    }
    return (float)(0.5 * atan2(2.0 * sxy, sxx - syy));
}

/* endpoint tangent minus start tangent, wrapped to [-pi/2, pi/2] */
static float angle_change(const TPt* p, int n) {
    int m = n < 10 ? n / 2 : 5;
    float a0 = window_angle(p, 0, m);
    float a1 = window_angle(p, n - m, m);
    float d = a1 - a0;
    while (d > PI_F * 0.5f) d -= PI_F;
    while (d < -PI_F * 0.5f) d += PI_F;
    return fabsf(d);
}

/* max perpendicular deviation from the start-end chord */
static float chord_sagitta(const TPt* p, int n) {
    float dx = p[n - 1].x - p[0].x, dy = p[n - 1].y - p[0].y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) return 0.0f;
    float h = 0.0f;
    for (int i = 1; i < n - 1; i++) {
        float ex = p[i].x - p[0].x, ey = p[i].y - p[0].y;
        float d = fabsf(ex * dy - ey * dx) / len;
        if (d > h) h = d;
    }
    return h;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Part a;
    part_spawn(&a, 0, 10, 0, 0, 0, 0);
    step(&a, 1.0f, 0, -9.8f, 0, 0);
    check("gravity_pulls", a.vy < 0.0f);
    check("gravity_magnitude", fabsf(a.vy + 9.8f) < 1e-3f);

    Part b;
    part_spawn(&b, 0, 0, 10, 0, 0, 0);
    for (int i = 0; i < 10; i++) step(&b, 0.1f, 0, 0, 5.0f, 0);
    check("drag_slows", speed(&b) < 10.0f * 0.5f);

    Part c;
    part_spawn(&c, 1, 0, 0, 0, 0, 0);
    step(&c, 0.1f, 0, 0, 0, 10.0f);
    check("swirl_perpendicular", c.vx < 0.01f && c.vy > 0.5f);

    Part d;
    part_spawn(&d, 0, 0, 1, 0, 0, 0);
    float before = speed(&d);
    for (int i = 0; i < 5; i++) step(&d, 0.1f, 0, 0, 0, 0);
    check("free_flight", fabsf(speed(&d) - before) < 1e-5f);

    check("bad_dt", step(&d, -1.0f, 0, 0, 0, 0) == -1);
    check("bad_drag", step(&d, 0.1f, 0, 0, -1.0f, 0) == -1);

    /* --- wind (A): sampled once, lateral displacement = wind * lifetime --- */
    const float life = 1.0f, sdt = 0.01f;
    int wn = (int)(life / sdt + 0.5f);

    Part wd;
    part_spawn(&wd, 0, 0, 0, 0, 3.0f, 0);
    for (int i = 0; i < wn; i++) step(&wd, sdt, 0, 0, 0, 0);
    check("wind_drift_3x1s", fabsf(wd.x - 3.0f * life) < 1e-4f);
    check("wind_no_vertical_leak", fabsf(wd.y) < 1e-6f);

    Part ws;
    part_spawn(&ws, 0, 0, 3, 0, 2.0f, 0);
    for (int i = 0; i < wn; i++) step(&ws, sdt, 0, 0, 0, 0);
    check("wind_adds_to_self", fabsf(ws.x - 5.0f * life) < 1e-4f);

    Part wg, ng;
    part_spawn(&wg, 0, 0, 3, 0, 2.0f, 0);
    part_spawn(&ng, 0, 0, 3, 0, 0, 0);
    for (int i = 0; i < wn; i++) {
        step(&wg, sdt, 0, 0, 5.0f, 0);
        step(&ng, sdt, 0, 0, 5.0f, 0);
    }
    check("wind_drift_survives_drag", fabsf((wg.x - ng.x) - 2.0f * life) < 1e-4f);

    /* same wind, half the dt: same displacement, so no hidden per-frame term */
    Part wf;
    part_spawn(&wf, 0, 0, 0, 0, 3.0f, 0);
    for (int i = 0; i < 2 * wn; i++) step(&wf, sdt * 0.5f, 0, 0, 0, 0);
    check("wind_dt_independent", fabsf(wf.x - 3.0f * life) < 1e-4f);

    /* three wind speeds, one run each: (wind, displacement at expiry) */
    double winds[3] = { 2.0, 5.0, 8.0 };
    double dxs[3];
    double max_pt_err = 0.0, max_w_err = 0.0;
    for (int i = 0; i < 3; i++) {
        double dv = (double)drift_over_life((float)winds[i], life, sdt);
        dxs[i] = dv;
        double e = fabs(dv - winds[i] * (double)life);
        if (e > max_pt_err) max_pt_err = e;
        double implied = dv / (double)life;
        if (fabs(implied - winds[i]) > max_w_err) max_w_err = fabs(implied - winds[i]);
    }

    double fa, fb;
    fit_line3(winds, dxs, &fa, &fb);
    double max_res = max_residual3(winds, dxs, fa, fb);
    /* dx = T * wind: the slope has units of seconds and equals the lifetime;
       slope / lifetime is the scale on wind and must be 1 */
    double scale = fa / (double)life;
    for (int i = 0; i < 3; i++) {
        printf("fit_pt%d_wind=%.6f dx=%.6f residual=%.3e implied_wind=%.6f\n",
               i, winds[i], dxs[i], dxs[i] - (fa * winds[i] + fb), dxs[i] / (double)life);
    }
    printf("fit_slope=%.9f\n", fa);
    printf("fit_intercept=%.9f\n", fb);
    printf("fit_slope_minus_life=%.3e\n", fa - (double)life);
    printf("fit_wind_scale=%.9f\n", scale);
    printf("fit_max_abs_residual=%.3e\n", max_res);
    printf("fit_max_displacement_error=%.3e\n", max_pt_err);
    printf("fit_max_implied_wind_error=%.3e\n", max_w_err);

    /* same three speeds, three time samples per run: dx/dt slope must equal wind */
    double max_time_slope_err = 0.0, max_time_res = 0.0;
    for (int i = 0; i < 3; i++) {
        Part p;
        part_spawn(&p, 0, 0, 0, 0, (float)winds[i], 0);
        double ts[3], xs[3];
        int marks[3] = { wn / 3, 2 * wn / 3, wn };
        int seen = 0;
        for (int k = 1; k <= wn && seen < 3; k++) {
            step(&p, sdt, 0, 0, 0, 0);
            if (k == marks[seen]) { ts[seen] = (double)k * sdt; xs[seen] = p.x; seen++; }
        }
        double a2, b2;
        fit_line3(ts, xs, &a2, &b2);
        double e = fabs(a2 - winds[i]);
        if (e > max_time_slope_err) max_time_slope_err = e;
        double r = max_residual3(ts, xs, a2, b2);
        if (r > max_time_res) max_time_res = r;
        printf("time_pt%d wind=%.6f t=[%.3f,%.3f,%.3f] x=[%.6f,%.6f,%.6f] slope=%.9f err=%.3e\n",
               i, winds[i], ts[0], ts[1], ts[2], xs[0], xs[1], xs[2], a2, e);
    }
    printf("time_fit_max_slope_error=%.3e\n", max_time_slope_err);
    printf("time_fit_max_residual=%.3e\n", max_time_res);

    check("wind_drift_linear", max_pt_err < 1e-4);
    check("fit_slope_equals_life", fabs(fa - (double)life) < 1e-4);
    check("fit_residual_bound", max_res < 1e-4);
    check("implied_wind_within_0p1", max_w_err < 0.1);
    check("time_slope_within_0p1", max_time_slope_err < 0.1);
    check("time_fit_residual_bound", max_time_res < 1e-4);

    /* --- shape discriminator: frozen wind = straight, per-frame = curved --- */
    const float u = 2.0f, w0 = 2.0f, alpha = 4.0f, T = 100 * 0.01f;
    TPt straight[TRAJN], curved[TRAJN];
    int ns = run_traj(straight, 0, 0.0f, u, w0, alpha, 0.01f, 100);
    int nc = run_traj(curved, 1, 0.0f, u, w0, alpha, 0.01f, 100);
    float dth_s = angle_change(straight, ns);
    float dth_c = angle_change(curved, nc);
    float sag_s = chord_sagitta(straight, ns);
    float sag_c = chord_sagitta(curved, nc);

    /* analytic: theta(t) = atan((w0+alpha*t)/u), h = (alpha*T^2/8)*|sin(beta)|
       with beta the angle between the wind axis and the chord */
    float dth_theory = atanf((w0 + alpha * T) / u) - atanf(w0 / u);
    float chord_dx = w0 * T + 0.5f * alpha * T * T, chord_dy = u * T;
    float beta = atan2f(chord_dy, chord_dx);
    float sag_theory = (alpha * T * T / 8.0f) * fabsf(sinf(beta));

    printf("disc_straight_dtheta_deg=%.3f sagitta=%.6f\n", dth_s * 180.0f / PI_F, sag_s);
    printf("disc_perframe_dtheta_deg=%.3f sagitta=%.6f\n", dth_c * 180.0f / PI_F, sag_c);
    printf("disc_theory_dtheta_deg=%.3f sagitta=%.6f\n", dth_theory * 180.0f / PI_F, sag_theory);

    /* constant per-frame wind is the degenerate case: it must look straight too */
    TPt constf[TRAJN];
    int nk = run_traj(constf, 1, 0.0f, u, w0, 0.0f, 0.01f, 100);
    float dth_k = angle_change(constf, nk);
    check("disc_perframe_constant_is_straight", dth_k < 0.02f);

    check("disc_straight_angle", dth_s < 0.02f);
    check("disc_curved_angle", dth_c > 0.30f);
    check("disc_straight_sagitta", sag_s < 0.01f);
    check("disc_curved_sagitta", sag_c > 0.10f);
    check("disc_curved_matches_theory",
          fabsf(dth_c - dth_theory) < 0.05f && fabsf(sag_c - sag_theory) < 0.05f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
