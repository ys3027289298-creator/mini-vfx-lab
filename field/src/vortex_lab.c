
/*
 * vortex_lab: deterministic radius-aware vortex tuning experiment.
 *
 * The old swirl parameter is a radius-independent tangential acceleration.
 * Here the vortex is written as the centripetal acceleration demanded by a
 * particle's tangential speed:
 *
 *     a_req(r, v_tau) = v_tau^2 / r = Omega * v_tau = Omega^2 * r
 *
 * and the field has a finite inward grip budget a_grip.  In solid-body curl,
 * v_tau = Omega*r, so a particle can be held while Omega^2*r <= a_grip:
 *
 *     r_max = a_grip / Omega^2
 *     Omega(D) = q * sqrt(2*a_grip / D),  R = D/2
 *
 * q is a derating factor.  The sweep below chooses q from the crossing of the
 * outer-ring escape curve E(q) and the curl-deficit curve 1 - T(q), where T is
 * the tangential share of speed during a fixed onset window.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define N_PART 48
#define N_OUTER 24
#define MAX_ESC N_OUTER

typedef struct {
    int id;
    int outer;
    double x, y;
    double vx, vy;
    double r0;
    double rmax;
} Part;

typedef struct {
    double a_grip;       /* maximum inward centripetal acceleration, u/s^2 */
    double drag;         /* linear drag / tangential coupling rate, 1/s */
    double dt;
    double duration;
    double t_start;
    double t_end;
    double escape_rmax;  /* max-radius multiple of R */
    double escape_final; /* final-radius multiple of R */
} Cfg;

typedef struct {
    const char *label;
    double diameter;
} Diameter;

typedef struct {
    double q;
    double omega;
    double rmax_formula;
    double escape_rate;
    double tangential_share;
    int n_escape;
    int escape_ids[MAX_ESC];
} Result;

/* SplitMix64 so the experiment is deterministic without platform rand(). */
static double unit_random(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    z ^= z >> 31;
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

static double range_random(uint64_t *state, double lo, double hi) {
    return lo + (hi - lo) * unit_random(state);
}

static void init_puff(Part ps[N_PART], double diameter, int diameter_index) {
    static const int counts[3] = {8, 16, 24};
    static const double radii[3] = {0.32, 0.66, 1.00};
    const double R = 0.5 * diameter;
    uint64_t seed = UINT64_C(0x5eed202611250000)
                  ^ (uint64_t)(diameter_index + 1) * UINT64_C(0x100000001b3);
    int id = 0;

    for (int ring = 0; ring < 3; ++ring) {
        for (int i = 0; i < counts[ring]; ++i) {
            double angle = 2.0 * PI * (double)i / (double)counts[ring]
                         + range_random(&seed, -0.055, 0.055);
            double r = R * radii[ring]
                      * (1.0 + range_random(&seed, -0.035, 0.035));
            double expansion_scale = ring == 2 ? 1.0 : 0.55 + 0.20 * ring;
            double vr = expansion_scale * range_random(&seed, 0.16, 0.50);
            double vt0 = expansion_scale * range_random(&seed, -0.10, 0.20);

            Part *p = &ps[id];
            p->id = id;
            p->outer = ring == 2;
            p->x = r * cos(angle);
            p->y = r * sin(angle);
            p->vx = vr * cos(angle) - vt0 * sin(angle);
            p->vy = vr * sin(angle) + vt0 * cos(angle);
            p->r0 = r;
            p->rmax = r;
            ++id;
        }
    }
}

static void acceleration(double x, double y, double vx, double vy,
                         double omega, const Cfg *cfg,
                         double *ax, double *ay) {
    double r = hypot(x, y);
    if (r < 1e-12) {
        *ax = 0.0;
        *ay = 0.0;
        return;
    }

    double ux = x / r, uy = y / r;
    double tx = -uy,    ty = ux;
    double vr = vx * ux + vy * uy;
    double vt = vx * tx + vy * ty;

    /* Inward grip supplies the demanded centripetal acceleration until it
       saturates.  Beyond that point r_ddot becomes outward/positive. */
    double required = vt * vt / r;
    double radial = -fmin(required, cfg->a_grip);

    /* Linear drag damps radial motion.  On the tangential component, adding
       drag*(Omega*r) gives lambda*(Omega*r - v_tau), targeting solid body. */
    *ax = radial * ux
        - cfg->drag * vr * ux
        + cfg->drag * (omega * r - vt) * tx;
    *ay = radial * uy
        - cfg->drag * vr * uy
        + cfg->drag * (omega * r - vt) * ty;
}

static void rk4_step(Part *p, double omega, const Cfg *cfg) {
    double h = cfg->dt;

    double a1x, a1y;
    acceleration(p->x, p->y, p->vx, p->vy, omega, cfg, &a1x, &a1y);
    double k1[4] = {p->vx, p->vy, a1x, a1y};

    double x2 = p->x + 0.5 * h * k1[0];
    double y2 = p->y + 0.5 * h * k1[1];
    double vx2 = p->vx + 0.5 * h * k1[2];
    double vy2 = p->vy + 0.5 * h * k1[3];
    double a2x, a2y;
    acceleration(x2, y2, vx2, vy2, omega, cfg, &a2x, &a2y);
    double k2[4] = {vx2, vy2, a2x, a2y};

    double x3 = p->x + 0.5 * h * k2[0];
    double y3 = p->y + 0.5 * h * k2[1];
    double vx3 = p->vx + 0.5 * h * k2[2];
    double vy3 = p->vy + 0.5 * h * k2[3];
    double a3x, a3y;
    acceleration(x3, y3, vx3, vy3, omega, cfg, &a3x, &a3y);
    double k3[4] = {vx3, vy3, a3x, a3y};

    double x4 = p->x + h * k3[0];
    double y4 = p->y + h * k3[1];
    double vx4 = p->vx + h * k3[2];
    double vy4 = p->vy + h * k3[3];
    double a4x, a4y;
    acceleration(x4, y4, vx4, vy4, omega, cfg, &a4x, &a4y);
    double k4[4] = {vx4, vy4, a4x, a4y};

    p->x  += h / 6.0 * (k1[0] + 2.0 * k2[0] + 2.0 * k3[0] + k4[0]);
    p->y  += h / 6.0 * (k1[1] + 2.0 * k2[1] + 2.0 * k3[1] + k4[1]);
    p->vx += h / 6.0 * (k1[2] + 2.0 * k2[2] + 2.0 * k3[2] + k4[2]);
    p->vy += h / 6.0 * (k1[3] + 2.0 * k2[3] + 2.0 * k3[3] + k4[3]);
}

static Result run_case(double diameter, double q, int diameter_index,
                       const Cfg *cfg, Part final_parts[N_PART]) {
    Part ps[N_PART];
    init_puff(ps, diameter, diameter_index);

    const double R = 0.5 * diameter;
    const double omega = q * sqrt(cfg->a_grip / R);
    const int steps = (int)lround(cfg->duration / cfg->dt);

    double tangential_sum = 0.0;
    double speed_sum = 0.0;

    for (int step = 0; step < steps; ++step) {
        for (int i = 0; i < N_PART; ++i) {
            rk4_step(&ps[i], omega, cfg);
            double r = hypot(ps[i].x, ps[i].y);
            if (r > ps[i].rmax) ps[i].rmax = r;

            double t = (step + 1) * cfg->dt;
            if (ps[i].outer && t >= cfg->t_start && t <= cfg->t_end) {
                double ux = ps[i].x / r, uy = ps[i].y / r;
                double tx = -uy, ty = ux;
                double vt = ps[i].vx * tx + ps[i].vy * ty;
                double speed = hypot(ps[i].vx, ps[i].vy);
                tangential_sum += fabs(vt);
                speed_sum += speed;
            }
        }
    }

    Result result;
    memset(&result, 0, sizeof(result));
    result.q = q;
    result.omega = omega;
    result.rmax_formula = cfg->a_grip / (omega * omega);
    result.tangential_share = tangential_sum / speed_sum;

    for (int i = 0; i < N_PART; ++i) {
        if (!ps[i].outer) continue;
        double rfinal = hypot(ps[i].x, ps[i].y);
        if (ps[i].rmax > cfg->escape_rmax * R &&
            rfinal > cfg->escape_final * R &&
            result.n_escape < MAX_ESC) {
            result.escape_ids[result.n_escape++] = ps[i].id;
        }
    }
    result.escape_rate = (double)result.n_escape / (double)N_OUTER;

    if (final_parts) memcpy(final_parts, ps, sizeof(ps));
    return result;
}

static int self_check(void) {
    const Cfg cfg = {6.0, 1.6, 1.0 / 240.0, 6.0, 0.10, 0.50, 1.60, 1.25};
    int failed = 0;

    for (int i = 0; i < 3; ++i) {
        double D = (double[]){1.2, 2.0, 3.2}[i];
        double R = 0.5 * D;
        double omega = sqrt(2.0 * cfg.a_grip / D);
        double rmax = cfg.a_grip / (omega * omega);
        int ok = fabs(rmax - R) < 1e-10;
        printf("diameter_roundtrip_%d=%d\n", i, ok);
        failed += !ok;
    }

    Part ps[N_PART];
    Result r = run_case(2.0, 0.0, 1, &cfg, ps);
    int stable_metric = r.tangential_share > 0.0 && r.tangential_share < 1.0;
    printf("zero_strength_metric_bounded=%d\n", stable_metric);
    failed += !stable_metric;

    printf("vortex_lab_all_ok=%d\n", failed == 0);
    return failed == 0 ? 0 : 1;
}

static void write_particle_csv(FILE *fp, const Diameter *d, const Result *r,
                               const Cfg *cfg, const Part ps[N_PART]) {
    (void)cfg;
    const double R = 0.5 * d->diameter;
    for (int i = 0; i < N_PART; ++i) {
        if (!ps[i].outer) continue;
        double rf = hypot(ps[i].x, ps[i].y);
        double ux = ps[i].x / rf, uy = ps[i].y / rf;
        double tx = -uy, ty = ux;
        double vr = ps[i].vx * ux + ps[i].vy * uy;
        double vt = ps[i].vx * tx + ps[i].vy * ty;
        int escaped = 0;
        for (int j = 0; j < r->n_escape; ++j)
            if (r->escape_ids[j] == i) escaped = 1;
        fprintf(fp, "%s,%.1f,P%02d,%d,%.9f,%.6f,%.6f,%.6f,%.6f,%d\n",
                d->label, d->diameter, i + 1, i,
                ps[i].x, ps[i].y, rf / R, vr, vt, escaped);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--check") == 0)
        return self_check();
    if (argc != 2) {
        fprintf(stderr, "usage: %s <artifacts-dir>|--check\n", argv[0]);
        return 2;
    }

    const char *outdir = argv[1];
    char path[4096];
    Cfg cfg = {6.0, 1.6, 1.0 / 240.0, 6.0, 0.10, 0.50, 1.60, 1.25};
    Diameter diameters[3] = {
        {"S", 1.2},
        {"M", 2.0},
        {"L", 3.2},
    };

    snprintf(path, sizeof(path), "%s/vortex_curves.csv", outdir);
    FILE *curves = fopen(path, "w");
    if (!curves) { perror(path); return 1; }
    fprintf(curves,
            "size,diameter,q,omega,rmax_formula,escape_rate,n_escape,"
            "n_outer,tangential_share,curl_deficit,balance_gap\n");

    snprintf(path, sizeof(path), "%s/vortex_outer_particles.csv", outdir);
    FILE *particles = fopen(path, "w");
    if (!particles) { perror(path); fclose(curves); return 1; }
    fprintf(particles,
            "size,diameter,label,zero_based_id,x,y,r_final_over_R,"
            "vr_final,vt_final,escaped\n");

    snprintf(path, sizeof(path), "%s/vortex_results.json", outdir);
    FILE *json = fopen(path, "w");
    if (!json) { perror(path); fclose(curves); fclose(particles); return 1; }

    fprintf(json, "{\n");
    fprintf(json, "  \"model\": \"a_req=v_tau^2/r=Omega*v_tau=Omega^2*r; rmax=a_grip/Omega^2\",\n");
    fprintf(json, "  \"config\": {\n");
    fprintf(json, "    \"a_grip\": %.17g,\n", cfg.a_grip);
    fprintf(json, "    \"drag\": %.17g,\n", cfg.drag);
    fprintf(json, "    \"dt\": %.17g,\n", cfg.dt);
    fprintf(json, "    \"duration\": %.17g,\n", cfg.duration);
    fprintf(json, "    \"tangent_window_s\": [%.17g, %.17g],\n",
            cfg.t_start, cfg.t_end);
    fprintf(json, "    \"escape_rmax_multiple_R\": %.17g,\n", cfg.escape_rmax);
    fprintf(json, "    \"escape_final_multiple_R\": %.17g,\n", cfg.escape_final);
    fprintf(json, "    \"n_particles\": %d,\n", N_PART);
    fprintf(json, "    \"n_outer\": %d,\n", N_OUTER);
    fprintf(json, "    \"escape_threshold\": 0.05\n");
    fprintf(json, "  },\n  \"runs\": [\n");

    for (int di = 0; di < 3; ++di) {
        Result fallback = {0};
        Result previous = {0};
        double fallback_gap = INFINITY;
        double previous_h = 0.0;
        double q_star = NAN;
        int have_previous = 0;

        for (int k = 0; k <= 260; ++k) {
            double q = 0.005 * (double)k;
            Result r = run_case(diameters[di].diameter, q, di, &cfg, NULL);
            double deficit = 1.0 - r.tangential_share;
            double gap = fabs(r.escape_rate - deficit);
            double h = r.escape_rate - deficit;

            fprintf(curves, "%s,%.1f,%.3f,%.9f,%.9f,%.9f,%d,%d,%.9f,%.9f,%.9f\n",
                    diameters[di].label, diameters[di].diameter, q,
                    r.omega, r.rmax_formula, r.escape_rate, r.n_escape,
                    N_OUTER, r.tangential_share, deficit, gap);

            if (gap < fallback_gap) {
                fallback = r;
                fallback_gap = gap;
            }

            /* First crossing of E(q) and 1-T(q); linearize its q position. */
            if (isnan(q_star) && have_previous &&
                previous_h < 0.0 && h >= 0.0) {
                double fraction = previous_h / (previous_h - h);
                q_star = previous.q + fraction * (r.q - previous.q);
            }
            previous = r;
            previous_h = h;
            have_previous = 1;
        }

        if (isnan(q_star)) q_star = fallback.q;

        Part final_parts[N_PART];
        Result final_run = run_case(diameters[di].diameter, q_star, di,
                                    &cfg, final_parts);
        double final_gap = fabs(final_run.escape_rate
                                - (1.0 - final_run.tangential_share));
        double edge_drive_accel = cfg.drag * final_run.omega
                                * (0.5 * diameters[di].diameter);
        write_particle_csv(particles, &diameters[di], &final_run, &cfg,
                           final_parts);

        printf("%s D=%.1f q*=%.6f Omega=%.6f rmax=%.6f escape=%d/%d (%.2f%%) "
               "tangential=%.4f ids=",
               diameters[di].label, diameters[di].diameter, final_run.q,
               final_run.omega, final_run.rmax_formula, final_run.n_escape,
               N_OUTER, 100.0 * final_run.escape_rate,
               final_run.tangential_share);
        for (int j = 0; j < final_run.n_escape; ++j)
            printf("P%02d%s", final_run.escape_ids[j] + 1,
                   j + 1 == final_run.n_escape ? "" : ",");
        if (final_run.n_escape == 0) printf("none");
        printf("\n");

        fprintf(json, "    {\n");
        fprintf(json, "      \"size\": \"%s\",\n", diameters[di].label);
        fprintf(json, "      \"diameter\": %.17g,\n", diameters[di].diameter);
        fprintf(json, "      \"q\": %.17g,\n", final_run.q);
        fprintf(json, "      \"omega\": %.17g,\n", final_run.omega);
        fprintf(json, "      \"edge_drive_accel\": %.17g,\n",
                edge_drive_accel);
        fprintf(json, "      \"rmax_formula\": %.17g,\n",
                final_run.rmax_formula);
        fprintf(json, "      \"escape_rate\": %.17g,\n",
                final_run.escape_rate);
        fprintf(json, "      \"escape_percent\": %.17g,\n",
                100.0 * final_run.escape_rate);
        fprintf(json, "      \"n_escape\": %d,\n", final_run.n_escape);
        fprintf(json, "      \"n_outer\": %d,\n", N_OUTER);
        fprintf(json, "      \"over_five_percent\": %s,\n",
                final_run.escape_rate > 0.05 ? "true" : "false");
        fprintf(json, "      \"tangential_share\": %.17g,\n",
                final_run.tangential_share);
        fprintf(json, "      \"selection\": \"first linearized crossing E(q)=1-tangential_share(q)\",\n");
        fprintf(json, "      \"balance_gap\": %.17g,\n", final_gap);
        fprintf(json, "      \"escape_ids_zero_based\": [");
        for (int j = 0; j < final_run.n_escape; ++j)
            fprintf(json, "%d%s", final_run.escape_ids[j],
                    j + 1 == final_run.n_escape ? "" : ", ");
        fprintf(json, "],\n");
        fprintf(json, "      \"escape_labels\": [");
        for (int j = 0; j < final_run.n_escape; ++j)
            fprintf(json, "\"P%02d\"%s", final_run.escape_ids[j] + 1,
                    j + 1 == final_run.n_escape ? "" : ", ");
        fprintf(json, "]\n");
        fprintf(json, "    }%s\n", di == 2 ? "" : ",");
    }

    fprintf(json, "  ]\n}\n");
    fclose(curves);
    fclose(particles);
    fclose(json);

    printf("wrote %s/vortex_curves.csv\n", outdir);
    printf("wrote %s/vortex_outer_particles.csv\n", outdir);
    printf("wrote %s/vortex_results.json\n", outdir);
    return 0;
}
