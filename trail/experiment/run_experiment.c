
/* experiment: three speed tiers on a tightening Archimedean spiral
   followed by a tangent straight ejection.
   phase 1 (vortex): R = R0 - b*phi, rho shrinks and crosses r (q<1);
   phase 2 (ejected): tangent straight line, rho -> inf, zone closes.
   analytic derivatives feed the production sampler in ../src/trail.c. */
#define TRAIL_SAMPLER_LIB
#include "../src/trail.c"

#include <stdio.h>

#define RADIUS   1.0f
#define FIXED_K  4
#define DT       0.002f
#define W        1.5f
#define MAXZONES 64

typedef struct { float v_enter, v_leave; } ZoneLog;

typedef struct { float R0, b, phi1, t2; } Path;

/* path state at time t; returns speed, rho via analytic derivatives */
static float path_state(const Path* p, float t,
                        float* x, float* y,
                        float* vx, float* vy,
                        float* ax, float* ay, float* rho) {
    float t1 = p->phi1 / W;
    if (t <= t1) {
        float phi = W * t;
        float cp = cosf(phi), sp = sinf(phi);
        float R = p->R0 - p->b * phi;
        float Rp = -p->b * W;                 /* dR/dt: R = R0 - b*phi */
        *x = R * cp;
        *y = R * sp;
        /* x' = R' cos - R sin W ; y' = R' sin + R cos W */
        float xd = Rp * cp - R * sp * W;
        float yd = Rp * sp + R * cp * W;
        /* x'' = -R W^2 cos - 2 R' W sin ; y'' = -R W^2 sin + 2 R' W cos */
        float xdd = -R * cp * W * W - 2.0f * Rp * sp * W;
        float ydd = -R * sp * W * W + 2.0f * Rp * cp * W;
        *vx = xd; *vy = yd; *ax = xdd; *ay = ydd;
        *rho = smp_curvature_radius(xd, yd, xdd, ydd);
        return sqrtf(xd * xd + yd * yd);
    } else {
        /* tangent line: freeze the t1 endpoint frame */
        float phi = p->phi1;
        float cp = cosf(phi), sp = sinf(phi);
        float R = p->R0 - p->b * phi;
        float Rp = -p->b * W;
        float ex = Rp * cp - R * sp * W;
        float ey = Rp * sp + R * cp * W;
        float x0 = R * cp, y0 = R * sp;
        float u = t - t1;
        *x = x0 + ex * u; *y = y0 + ey * u;
        *vx = ex; *vy = ey; *ax = 0.0f; *ay = 0.0f;
        *rho = SMP_RHOMAX;
        return sqrtf(ex * ex + ey * ey);
    }
}

/* analytic rho of the spiral phase, with R'=-bW.
   v^2 = W^2 (R^2 + b^2),  |v x a| = W^3 (R^2 + 2b^2), hence
   rho = (R^2 + b^2)^{3/2} / (R^2 + 2b^2)   (W cancels) */
static float rho_spiral(float R, float b) {
    float s = R * R + b * b;
    return s * sqrtf(s) / (R * R + 2.0f * b * b);
}

/* speed on the spiral at radius R: v = W sqrt(R^2 + b^2) */
static float speed_spiral(float R, float b) {
    return W * sqrtf(R * R + b * b);
}

/* solve rho(R)=1 numerically on [0.5, R0] for analytic cross-check.
   monotone increasing in R for R>=0, so bisection works */
static float R_at_q1(float R0, float b) {
    float lo = 0.5f, hi = R0;
    if (rho_spiral(lo, b) >= RADIUS) return -1.0f;   /* never crosses */
    for (int i = 0; i < 60; i++) {
        float mid = 0.5f * (lo + hi);
        if (rho_spiral(mid, b) < RADIUS) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

static void run_tier(const char* name, float R0, float b) {
    /* tighten down to R_end = 0.5 so rho dips well below r=1 */
    float phi1 = (R0 - 0.5f) / b;
    Path p = { R0, b, phi1, phi1 / W + 1.5f };
    int nsteps = (int)(p.t2 / DT) + 10;

    Sampler s;
    smp_init(&s, RADIUS, SMP_CMIN, FIXED_K);

    ZoneLog zones[MAXZONES];
    int nz = 0;
    float v_enter = 0.0f;

    for (int i = 0; i <= nsteps; i++) {
        float t = DT * (float)i;
        float x, y, vx, vy, ax, ay, rho, v;
        v = path_state(&p, t, &x, &y, &vx, &vy, &ax, &ay, &rho);
        int was_zone = s.in_zone;
        smp_update_va(&s, x, y, vx, vy, ax, ay);
        if (s.in_zone && !was_zone) v_enter = v;
        if (!s.in_zone && was_zone && nz < MAXZONES) {
            zones[nz].v_enter = v_enter;
            zones[nz].v_leave = v;
            nz++;
        }
    }
    int open_zone = s.in_zone;
    smp_finish(&s);
    if (open_zone && nz < MAXZONES) {
        float x, y, vx, vy, ax, ay, rho;
        float v = path_state(&p, p.t2, &x, &y, &vx, &vy, &ax, &ay, &rho);
        zones[nz].v_enter = v_enter;
        zones[nz].v_leave = v;
        nz++;
    }

    float v0 = speed_spiral(R0, b);
    float Rc = R_at_q1(R0, b);
    float v_analytic_enter = Rc > 0.0f ? speed_spiral(Rc, b) : -1.0f;
    float v_analytic_eject = speed_spiral(0.5f, b);
    printf("tier=%s v0=%.3f phi1=%.2f rho_end=%.4f\n",
           name, v0, phi1, rho_spiral(0.5f, b));
    printf("  output_points=%d breaks=%d normal_overlap=%d zone_entries=%d "
           "zone_fixed_points=%d\n",
           s.n, s.breaks, s.overlap_viol, s.zone_entries, s.zone_emitted);
    printf("  analytic q=1 boundary: R*=%.4f v_enter=%.4f; "
           "eject speed v_leave=%.4f\n",
           Rc, v_analytic_enter, v_analytic_eject);
    for (int i = 0; i < nz; i++)
        printf("  measured q<1 zone%d: speed interval [%.4f, %.4f]\n",
               i + 1, zones[i].v_enter, zones[i].v_leave);
    printf("  accept_breaks_zero=%d\n", s.breaks == 0);
}

/* B(c): break count; N(c): point count over the medium-tier path.
   fine grid around the c=2 connectivity edge; low c included to show
   overlap violations where spacing drops below the curved-path bound. */
static void coef_sweep(void) {
    FILE* f = fopen("coef_sweep.csv", "w");
    if (!f) { perror("coef_sweep.csv"); return; }
    fprintf(f, "c,breaks,points,overlap\n");
    Path p = { 12.0f, 0.667f, (12.0f - 0.5f) / 0.667f, 0.0f };
    p.t2 = p.phi1 / W + 1.5f;
    int nsteps = (int)(p.t2 / DT) + 10;
    for (int ci = 0; ci <= 60; ci++) {
        float c = 0.6f + 0.045f * (float)ci;      /* 0.60 .. 3.30 */
        Sampler s;
        smp_init(&s, RADIUS, SMP_CMIN, FIXED_K);
        s.cfg.c = c;                               /* bypass clamp for sweep */
        for (int i = 0; i <= nsteps; i++) {
            float t = DT * (float)i;
            float x, y, vx, vy, ax, ay, rho;
            path_state(&p, t, &x, &y, &vx, &vy, &ax, &ay, &rho);
            smp_update_va(&s, x, y, vx, vy, ax, ay);
        }
        smp_finish(&s);
        fprintf(f, "%.4f,%d,%d,%d\n", c, s.breaks, s.n, s.overlap_viol);
    }
    fclose(f);
    printf("coef_sweep written: coef_sweep.csv (window pi/3=%.4f .. 2.0)\n",
           SMP_CMIN);
}

int main(void) {
    /* b = R0/18 so R reaches 0.5 within phi1; rho_min = b/2 stays < r */
    run_tier("low",     6.0f, 0.333f);
    run_tier("medium", 12.0f, 0.667f);
    run_tier("high",   20.0f, 1.111f);
    coef_sweep();
    return 0;
}
