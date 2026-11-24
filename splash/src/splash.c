
/* splash: ring-shaped secondary emission when a spark hits the ground.
 *
 * Model chain (see README for the full derivation):
 *   reflection axis  d = reflect(v_hat, n) = v_hat - 2 (v_hat.n) n
 *   energy loss      s = e * |v_in|              (e: material coefficient,
 *                                                  outgoing/incoming speed ratio)
 *   ring particle i  phi_i = 2 pi (i + 1/2) / N            (azimuth, full ring)
 *                    theta_i = theta0 + dtheta (2i/(N-1) - 1) (elev. spread)
 *                    v0_i = s (cos theta_i d
 *                              + sin theta_i (cos phi_i t1 + sin phi_i t2))
 *   ballistics       gravity g + linear drag k (same exp(-k dt) law as field/):
 *                    z(T)=0, q = kT, a = k v0z/g
 *                    (a+1)(1 - e^-q) - q = 0   -> positive root by bisection
 *                    range R = |v0h| (1 - e^-q) / k
 *                    k = 0 limit: T = 2 v0z/g, R = 2 |v0h| v0z / g
 *   inverse solve    mean range Rbar(e) is monotone in e, so e is recovered
 *                    from a target range by bisection on (0, 1].
 */
#include <stdio.h>
#include <math.h>

#define PI 3.14159265358979323846f
#define NMAX 64
#define G 9.8f

typedef struct { float x, y, z; } V3;

static V3 v3_add(V3 a, V3 b) { return (V3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 v3_sub(V3 a, V3 b) { return (V3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 v3_scale(V3 a, float s) { return (V3){ a.x * s, a.y * s, a.z * s }; }
static float v3_dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 v3_cross(V3 a, V3 b) {
    return (V3){ a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x };
}
static float v3_len(V3 a) { return sqrtf(v3_dot(a, a)); }
static V3 v3_norm(V3 a) {
    float l = v3_len(a);
    return l > 1e-12f ? v3_scale(a, 1.0f / l) : (V3){ 0, 0, 0 };
}
/* specular reflection about the contact normal; works for any surface */
static V3 v3_reflect(V3 vi, V3 n) {
    V3 v = v3_norm(vi);
    return v3_norm(v3_sub(v, v3_scale(n, 2.0f * v3_dot(v, n))));
}

/* Deterministic stratified ring around axis d: uniform azimuth, stratified
 * elevation. No RNG so a particle id keeps its meaning across runs. */
static void emit_ring(V3 axis, float speed, int n,
                      float theta0, float dtheta, V3 out[NMAX]) {
    V3 helper = fabsf(axis.y) > 0.9f ? (V3){ 1, 0, 0 } : (V3){ 0, 1, 0 };
    V3 t1 = v3_norm(v3_cross(helper, axis));
    V3 t2 = v3_cross(axis, t1);
    for (int i = 0; i < n; i++) {
        float phi = 2.0f * PI * (i + 0.5f) / (float)n;
        float u = n > 1 ? 2.0f * (float)i / (float)(n - 1) - 1.0f : 0.0f;
        float theta = theta0 + dtheta * u;
        V3 radial = v3_add(v3_scale(t1, cosf(phi)), v3_scale(t2, sinf(phi)));
        V3 d = v3_add(v3_scale(axis, cosf(theta)),
                      v3_scale(radial, sinf(theta)));
        out[i] = v3_scale(v3_norm(d), speed);
    }
}

/* Analytic range of one particle under gravity + linear drag.
 * Returns -1 for an invalid (downward) launch; T_out may be NULL. */
static float flight_range(V3 v0, float k, float* T_out) {
    float vh = sqrtf(v0.x * v0.x + v0.y * v0.y);
    float vz = v0.z;
    if (vz <= 0.0f) { if (T_out) *T_out = -1.0f; return -1.0f; }
    if (k <= 1e-6f) {
        float T = 2.0f * vz / G;
        if (T_out) *T_out = T;
        return vh * T;
    }
    float a = k * vz / G;
    /* positive root of f(q) = (a+1)(1 - e^-q) - q; f(0)=0, f rises then falls */
    float hi = 1.0f;
    int guard = 0;
    while ((a + 1.0f) * (1.0f - expf(-hi)) - hi > 0.0f && guard++ < 60)
        hi *= 2.0f;
    float lo = 1e-9f;
    for (int it = 0; it < 80; it++) {
        float mid = 0.5f * (lo + hi);
        float f = (a + 1.0f) * (1.0f - expf(-mid)) - mid;
        if (f > 0.0f) lo = mid; else hi = mid;
    }
    float q = 0.5f * (lo + hi);
    float T = q / k;
    if (T_out) *T_out = T;
    return vh * (1.0f - expf(-q)) / k;
}

/* Numeric integration reference (semi-implicit Euler, drag as in field/),
 * landing instant refined by interpolation inside the crossing step. */
static float sim_range(V3 v0, float k, float dt) {
    V3 p = { 0, 0, 0 }, v = v0, prev;
    float decay = expf(-k * dt);
    for (int step = 0; step < 200000; step++) {
        prev = p;
        v.x *= decay; v.y *= decay; v.z *= decay;
        v.z -= G * dt;
        p.x += v.x * dt; p.y += v.y * dt; p.z += v.z * dt;
        if (p.z <= 0.0f) {
            float alpha = prev.z / (prev.z - p.z);
            float lx = prev.x + (p.x - prev.x) * alpha;
            float ly = prev.y + (p.y - prev.y) * alpha;
            return sqrtf(lx * lx + ly * ly);
        }
    }
    return -1.0f;
}

/* Normal-incidence impact on flat ground: vi = (0,0,-u), n = (0,0,1). */
static void impact_ring(float u_in, float e, int n,
                        float theta0, float dtheta, V3 v0[NMAX]) {
    V3 vi = { 0, 0, -u_in }, nrm = { 0, 0, 1 };
    V3 axis = v3_reflect(vi, nrm);
    emit_ring(axis, e * u_in, n, theta0, dtheta, v0);
}

static float analytic_ranges(float u_in, float e, float k, int n,
                             float theta0, float dtheta,
                             V3 v0[NMAX], float ranges[NMAX]) {
    impact_ring(u_in, e, n, theta0, dtheta, v0);
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        ranges[i] = flight_range(v0[i], k, NULL);
        sum += ranges[i];
    }
    return sum / (float)n;
}

/* Recover e from a target mean range. Rbar(e) is strictly increasing on
 * (0,1]; returns -1 when even e = 1 cannot reach the target. */
static int solve_e(float u_in, float k, int n, float theta0, float dtheta,
                   float r_target, float* e_out) {
    float lo = 1e-4f, hi = 1.0f;
    V3 v0[NMAX]; float ranges[NMAX];
    if (analytic_ranges(u_in, hi, k, n, theta0, dtheta, v0, ranges) < r_target)
        return -1;
    for (int it = 0; it < 80; it++) {
        float mid = 0.5f * (lo + hi);
        float rbar = analytic_ranges(u_in, mid, k, n, theta0, dtheta,
                                     v0, ranges);
        if (rbar < r_target) lo = mid; else hi = mid;
    }
    *e_out = 0.5f * (lo + hi);
    return 0;
}

/* ids[] receives particle indices whose range exceeds r_allow. */
static int oob_scan(const float ranges[], int n, float r_allow, int ids[NMAX]) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (ranges[i] > r_allow) ids[c++] = i;
    return c;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

#define NP 36
#define THETA0 (45.0f * PI / 180.0f)
#define DTHETA (20.0f * PI / 180.0f)
#define DRAG 0.6f
#define R_TARGET 2.0f
#define R_ALLOW 2.5f

int main(void) {
    /* ---- trade-off curves: range and out-of-bounds rate vs e ---- */
    const float tiers[3] = { 6.0f, 10.0f, 14.0f };
    for (int t = 0; t < 3; t++) {
        printf("== curves u_in=%.2f k=%.2f target=%.2f allow=%.2f ==\n",
               tiers[t], DRAG, R_TARGET, R_ALLOW);
        printf("  e      Rbar     oob_rate\n");
        for (int j = 1; j <= 10; j++) {
            float e = 0.1f * (float)j;
            V3 v0[NMAX]; float ranges[NMAX];
            float rbar = analytic_ranges(tiers[t], e, DRAG, NP,
                                         THETA0, DTHETA, v0, ranges);
            int ids[NMAX];
            int c = oob_scan(ranges, NP, R_ALLOW, ids);
            printf("  %.2f  %8.4f  %6.2f%%\n", e, rbar,
                   100.0f * (float)c / (float)NP);
        }
    }

    /* ---- three incident-speed tiers: inverse-solve e, then validate ---- */
    printf("== tier validation (target range %.2f m, +/-10%% = %.2f..%.2f) ==\n",
           R_TARGET, R_TARGET * 0.9f, R_TARGET * 1.1f);
    for (int t = 0; t < 3; t++) {
        float u = tiers[t], e;
        int status = solve_e(u, DRAG, NP, THETA0, DTHETA, R_TARGET, &e);
        if (status != 0) { printf("tier u=%.2f INFEASIBLE\n", u); failures++; continue; }

        V3 v0[NMAX]; float ranges[NMAX];
        float r_an = analytic_ranges(u, e, DRAG, NP, THETA0, DTHETA,
                                     v0, ranges);
        float r_sim_sum = 0.0f, max_err = 0.0f;
        for (int i = 0; i < NP; i++) {
            float rs = sim_range(v0[i], DRAG, 0.005f);
            r_sim_sum += rs;
            float err = fabsf(rs - ranges[i]) / ranges[i];
            if (err > max_err) max_err = err;
        }
        float r_sim = r_sim_sum / (float)NP;
        float relerr = 100.0f * fabsf(r_sim - R_TARGET) / R_TARGET;

        int ids[NMAX];
        int c = oob_scan(ranges, NP, R_ALLOW, ids);
        float oob_rate = 100.0f * (float)c / (float)NP;

        printf("u=%.2f e=%.4f R_analytic=%.4f R_sim=%.4f relerr=%.2f%% "
               "oob=%.2f%% (%d/%d) analytic_vs_numeric_max=%.2f%%\n",
               u, e, r_an, r_sim, relerr, oob_rate, c, NP,
               100.0f * max_err);
        char label[64];
        snprintf(label, sizeof label, "tier_u%.0f_range_within_10pct", u);
        check(label, relerr <= 10.0f);
        check("analytic_numeric_agree", max_err < 0.03f);
        if (oob_rate > 10.0f) {
            printf("  oob_particle_ids=");
            for (int i = 0; i < c; i++)
                printf("%s%d", i ? "," : "", ids[i]);
            printf("\n");
        }
    }

    /* ---- stress: tighten the allowed radius so the oob report fires ---- */
    const float R_TIGHT = 2.1f;
    printf("== stress: same solved e, R_ALLOW tightened to %.2f ==\n", R_TIGHT);
    for (int t = 0; t < 3; t++) {
        float u = tiers[t], e;
        if (solve_e(u, DRAG, NP, THETA0, DTHETA, R_TARGET, &e) != 0) continue;
        V3 sv[NMAX]; float sr[NMAX];
        analytic_ranges(u, e, DRAG, NP, THETA0, DTHETA, sv, sr);
        int sids[NMAX];
        int sc = oob_scan(sr, NP, R_TIGHT, sids);
        float srate = 100.0f * (float)sc / (float)NP;
        printf("u=%.2f e=%.4f oob=%.2f%% (%d/%d)", u, e, srate, sc, NP);
        if (srate > 10.0f) {
            printf(" oob_particle_ids=");
            for (int i = 0; i < sc; i++) printf("%s%d", i ? "," : "", sids[i]);
        }
        printf("\n");
    }

    /* ---- unit checks ---- */
    V3 up = { 0, 0, 1 };
    V3 r1 = v3_reflect((V3){ 0, 0, -1 }, up);
    check("reflect_flips_normal", fabsf(r1.z - 1.0f) < 1e-6f);

    V3 r2 = v3_reflect((V3){ -3, 0, -4 }, up); /* norm: (-.6,0,-.8) */
    check("reflect_keeps_tangent",
          fabsf(r2.x + 0.6f) < 1e-6f && fabsf(r2.z - 0.8f) < 1e-6f);

    /* tilted surface: normal 45 degrees, normal incidence reflects along n */
    V3 tilt = v3_norm((V3){ 1, 0, 1 });
    V3 r3 = v3_reflect(v3_scale(tilt, -1.0f), tilt);
    check("reflect_any_normal", fabsf(v3_dot(r3, tilt) - 1.0f) < 1e-6f);

    V3 v0[NMAX];
    impact_ring(10.0f, 0.5f, NP, THETA0, DTHETA, v0);
    V3 hsum = { 0, 0, 0 };
    int quad[4] = { 0, 0, 0, 0 }, all_up = 1, speed_ok = 1;
    for (int i = 0; i < NP; i++) {
        float hl = sqrtf(v0[i].x * v0[i].x + v0[i].y * v0[i].y);
        hsum.x += v0[i].x / hl; hsum.y += v0[i].y / hl;
        quad[(v0[i].x >= 0 ? 0 : 2) + (v0[i].y >= 0 ? 0 : 1)]++;
        if (v0[i].z <= 0.0f) all_up = 0;
        if (fabsf(v3_len(v0[i]) - 5.0f) > 1e-4f) speed_ok = 0;
    }
    check("ring_azimuth_uniform", v3_len(hsum) < 0.1f);
    check("ring_covers_all_quadrants",
          quad[0] > 0 && quad[1] > 0 && quad[2] > 0 && quad[3] > 0);
    check("ring_all_upward", all_up);
    check("ring_speed_ratio_e", speed_ok);

    V3 shot = { 3.0f, 0, 4.0f };
    float r_drag0 = flight_range(shot, 0.0f, NULL);
    float r_closed = 2.0f * 3.0f * 4.0f / G;
    check("no_drag_closed_form", fabsf(r_drag0 - r_closed) < 1e-4f);
    check("drag_reduces_range", flight_range(shot, 0.8f, NULL) < r_drag0);
    check("downward_launch_rejected", flight_range((V3){ 1, 0, -1 }, 0.5f, NULL) < 0);

    float e_solved;
    int ok = solve_e(10.0f, DRAG, NP, THETA0, DTHETA, R_TARGET, &e_solved);
    float ranges[NMAX];
    float r_check = analytic_ranges(10.0f, e_solved, DRAG, NP, THETA0,
                                    DTHETA, v0, ranges);
    check("solver_hits_target",
          ok == 0 && fabsf(r_check - R_TARGET) / R_TARGET < 1e-3f);
    check("solver_infeasible_guard",
          solve_e(10.0f, DRAG, NP, THETA0, DTHETA, 100.0f, &e_solved) == -1);

    float lo_r = analytic_ranges(10.0f, 0.3f, DRAG, NP, THETA0, DTHETA,
                                 v0, ranges);
    float hi_r = analytic_ranges(10.0f, 0.9f, DRAG, NP, THETA0, DTHETA,
                                 v0, ranges);
    check("range_monotone_in_e", hi_r > lo_r);

    /* forced out-of-bounds: listed ids must be exactly the violating set */
    analytic_ranges(10.0f, 0.9f, DRAG, NP, THETA0, DTHETA, v0, ranges);
    int ids[NMAX];
    int c = oob_scan(ranges, NP, 1.5f, ids);
    int exact = 1;
    for (int i = 0; i < NP; i++) {
        int listed = 0;
        for (int j = 0; j < c; j++) if (ids[j] == i) listed = 1;
        if ((ranges[i] > 1.5f) != listed) exact = 0;
    }
    check("oob_ids_exact_set", exact && c > 0);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
