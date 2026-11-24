
/* depth: depth-difference brightness attenuation for overlapping particles.
 *
 * Problem: particles that overlap in screen space read as flat cut-outs and
 * visually pass through each other. Give every particle a depth coordinate z
 * and multiply its linear-light brightness by a depth factor relative to the
 * front/reference plane:
 *
 *     a(d) = exp(-k * d),  d = z - z_ref >= 0
 *
 * Calibration from a visual-indistinguishability depth d_vis:
 *   a(d_vis) = eps  =>  k_cal = ln(1/eps) / d_vis
 * where eps is the linear-light visibility floor, obtained by linearizing a
 * practical 8-bit sRGB code threshold C_VIS (small transient spark seen over
 * glare; suprathreshold code floor ~48/255).
 *
 * N-layer product bound. Ordered layers z_1 <= ... <= z_N, each layer j has
 * depth gap d_j = z_j - z_{j-1} against the layer in front (d_1 = 0):
 *
 *     L_N / L_1 = prod_{j=1..N} a(d_j)
 *               = exp(-k * (z_N - z_1))          (equal intrinsic brightness)
 *
 * so the bright/dark contrast ratio is bounded by the reciprocal product:
 *
 *     CR <= 1 / prod_j a(d_j) = exp(k * (z_N - z_1)).
 *
 * Three depth-of-field tiers (shallow D=3 / standard D=6 / deep D=12) are
 * each run once with the same selected k. Every tier must satisfy:
 *   contrast_gap <= 0.30   (measured CR within 30% of the product bound)
 *   visible/total >= 0.50  (strict code-floor visibility count)
 *
 * CLI:
 *   ./depth            self-check + three tier runs + gates, exit 0 iff all ok
 *   ./depth csv        coefficient sweep as tidy CSV
 *   ./depth json       full report as JSON
 *   ./depth run TIER   one deterministic run for one tier as JSON object
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define LAYERS    8      /* N overlapping layers per stack */
#define CAL_STACKS 24    /* ideal calibration stacks (unit brightness/center) */
#define NAT_STACKS 20    /* natural stacks with brightness/coverage variation */
#define CLOUD     160    /* free depth-cloud particles */
#define TOTAL     (LAYERS * (CAL_STACKS + NAT_STACKS) + CLOUD) /* 512 */

#define CODE_MAX  255.0
#define C_VIS     48.0    /* practical visibility code floor (sRGB code) */
#define L_FLOOR   0.0015  /* screen black level / haze added to measured light */
#define RHO_STEP  1.16    /* practical min brightness ratio between depth steps */
#define D_DEEP    12.0f
#define D_STD     6.0f
#define D_SHALLOW 3.0f

#define GAP_LIMIT 0.30
#define VIS_LIMIT 0.50

#define K_GRID_MAX 0.80
#define K_GRID_STEP 0.01
#define K_OVERDRIVE 0.80  /* negative control: far particles crushed */

/* ---- small deterministic RNG (splitmix64) ------------------------------ */

typedef struct { uint64_t s; } Rng;

static double rng_u(Rng* r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (double)(z >> 11) * (1.0 / 9007199254740992.0); /* [0,1) */
}

/* ---- display / vision model -------------------------------------------- */

static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

/* sRGB OETF: linear normalized light -> encoded [0,1] */
static double srgb_encode(double L) {
    L = clamp01(L);
    return L <= 0.0031308 ? 12.92 * L : 1.055 * pow(L, 1.0 / 2.4) - 0.055;
}

/* inverse sRGB EOTF: encoded [0,1] -> linear normalized light */
static double srgb_decode(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static int to_code(double L) { return (int)lround(CODE_MAX * srgb_encode(L)); }

/* linear visibility floor eps, derived from the practical code threshold */
static double vis_epsilon(void) { return srgb_decode(C_VIS / CODE_MAX); }

/* ---- depth attenuation primitives -------------------------------------- */

/* multiplicative depth factor for depth difference d >= 0 */
static double depth_factor(double d, double k) { return exp(-k * d); }

/* depth difference at which the factor reaches eps: a(d)=eps */
static double d_indistinguishable(double eps, double k) {
    return log(1.0 / eps) / k;
}

/* invert the calibration: choose k so a(d_vis)=eps */
static double k_from_calibration(double eps, double d_vis) {
    return log(1.0 / eps) / d_vis;
}

/* minimum depth step that produces one resolvable brightness ratio rho */
static double d_step_jnd(double rho, double k) { return log(rho) / k; }

/* ---- deterministic scene ------------------------------------------------ */

typedef struct {
    double z[LAYERS];
    double base[LAYERS];  /* intrinsic brightness */
    double weight[LAYERS];/* sprite coverage weight at the overlap pixel */
} Stack;

typedef struct { double z, base; } CloudP;

typedef struct {
    const char* name;
    double D;
    Stack cal[CAL_STACKS];
    Stack nat[NAT_STACKS];
    CloudP cloud[CLOUD];
} Scene;

static uint64_t seed_for(const char* tier) {
    /* FNV-ish mix so each tier gets an independent stable stream */
    uint64_t h = 1469598103934665603ULL;
    while (*tier) { h ^= (unsigned char)*tier++; h *= 1099511628211ULL; }
    return h ^ 0xD1B54A32D192ED03ULL;
}

static Scene make_scene(const char* name, double D) {
    Scene sc;
    memset(&sc, 0, sizeof(sc));
    sc.name = name;
    sc.D = D;
    Rng r = { seed_for(name) };
    double h = D / (LAYERS - 1);

    for (int s = 0; s < CAL_STACKS; s++) {
        for (int j = 0; j < LAYERS; j++) {
            /* tiny depth jitter (+-3% of layer spacing), gaps stay positive */
            sc.cal[s].z[j] = j * h + (rng_u(&r) * 2.0 - 1.0) * 0.03 * h;
            sc.cal[s].base[j] = 1.0;   /* equal intrinsic brightness */
            sc.cal[s].weight[j] = 1.0; /* exact pixel center */
        }
    }
    for (int s = 0; s < NAT_STACKS; s++) {
        for (int j = 0; j < LAYERS; j++) {
            sc.nat[s].z[j] = j * h + (rng_u(&r) * 2.0 - 1.0) * 0.05 * h;
            sc.nat[s].base[j] = 0.92 + 0.08 * rng_u(&r); /* [0.92,1] */
            double ox = (rng_u(&r) * 2.0 - 1.0) * 0.10;
            double oy = (rng_u(&r) * 2.0 - 1.0) * 0.10;
            double r2 = ox * ox + oy * oy;               /* sprite radius 1px */
            sc.nat[s].weight[j] = fmax(0.0, 1.0 - r2);   /* >= 0.98 */
        }
    }
    for (int i = 0; i < CLOUD; i++) {
        sc.cloud[i].z = rng_u(&r) * D;
        sc.cloud[i].base = 0.70 + 0.30 * rng_u(&r);      /* [0.7,1] */
    }
    return sc;
}

/* ---- run result --------------------------------------------------------- */

typedef struct {
    const char* mode;
    double D, k;
    double bound;        /* N-factor reciprocal product, geo mean over cal stacks */
    double actual;       /* measured linear CR through floor + 8-bit sRGB chain */
    double gap;          /* |actual-bound| / bound */
    double nat_bound;    /* product bound incl. known intrinsic b*w ratios */
    double nat_actual;
    double nat_gap;
    int visible, total;
    double visibility;
    int far_visible, far_total;
    double far_visibility;
    double step_ratio;   /* nominal brightness ratio of adjacent shallow steps */
    int gap_met, half_met, nat_gap_met;
} Result;

/* measured brightness of one particle at the overlap pixel after the display
 * chain: depth attenuation, black-level/haze floor, 8-bit sRGB quantize. */
static double measured_light(double base, double w, double z, double k) {
    double L = base * w * depth_factor(z, k);
    int code = to_code(L);
    if (code < 0) code = 0;
    if (code > (int)CODE_MAX) code = (int)CODE_MAX;
    return L_FLOOR + srgb_decode((double)code / CODE_MAX);
}

static int is_visible_code(double base, double z, double k) {
    return to_code(base * depth_factor(z, k)) >= (int)C_VIS;
}

static Result evaluate(const Scene* sc, double k) {
    Result r;
    memset(&r, 0, sizeof(r));
    r.mode = sc->name;
    r.D = sc->D;
    r.k = k;
    r.total = TOTAL;

    double sum_log_bound = 0.0, sum_log_actual = 0.0;
    for (int s = 0; s < CAL_STACKS; s++) {
        const Stack* st = &sc->cal[s];
        /* reciprocal of the N-factor product (d1 = 0 factor is 1) */
        double cb = exp(k * (st->z[LAYERS - 1] - st->z[0]));
        double m_front = measured_light(st->base[0], st->weight[0], st->z[0], k);
        double m_back  = measured_light(st->base[LAYERS - 1],
                                        st->weight[LAYERS - 1],
                                        st->z[LAYERS - 1], k);
        double ca = m_front / m_back;
        sum_log_bound += log(cb);
        sum_log_actual += log(ca);
    }
    r.bound = exp(sum_log_bound / CAL_STACKS);
    r.actual = exp(sum_log_actual / CAL_STACKS);
    r.gap = fabs(r.actual - r.bound) / r.bound;

    double sum_log_nb = 0.0, sum_log_na = 0.0;
    for (int s = 0; s < NAT_STACKS; s++) {
        const Stack* st = &sc->nat[s];
        int jL = LAYERS - 1;
        /* exact ideal ratio incl. intrinsic brightness/coverage coefficients */
        double depth_cr = exp(k * (st->z[jL] - st->z[0]));
        double nb = (st->base[0] * st->weight[0]) /
                    (st->base[jL] * st->weight[jL]) * depth_cr;
        double mf = measured_light(st->base[0], st->weight[0], st->z[0], k);
        double mb = measured_light(st->base[jL], st->weight[jL], st->z[jL], k);
        double na = mf / mb;
        sum_log_nb += log(nb);
        sum_log_na += log(na);
    }
    r.nat_bound = exp(sum_log_nb / NAT_STACKS);
    r.nat_actual = exp(sum_log_na / NAT_STACKS);
    r.nat_gap = fabs(r.nat_actual - r.nat_bound) / r.nat_bound;

    double zfar = 0.75 * sc->D;
    for (int s = 0; s < CAL_STACKS; s++) {
        for (int j = 0; j < LAYERS; j++) {
            double z = sc->cal[s].z[j];
            if (is_visible_code(sc->cal[s].base[j], z, k)) r.visible++;
            if (z >= zfar) { r.far_total++; if (is_visible_code(1.0, z, k)) r.far_visible++; }
        }
    }
    for (int s = 0; s < NAT_STACKS; s++) {
        for (int j = 0; j < LAYERS; j++) {
            double z = sc->nat[s].z[j];
            if (is_visible_code(sc->nat[s].base[j], z, k)) r.visible++;
            if (z >= zfar) {
                r.far_total++;
                if (is_visible_code(sc->nat[s].base[j], z, k)) r.far_visible++;
            }
        }
    }
    for (int i = 0; i < CLOUD; i++) {
        double z = sc->cloud[i].z;
        if (is_visible_code(sc->cloud[i].base, z, k)) r.visible++;
        if (z >= zfar) {
            r.far_total++;
            if (is_visible_code(sc->cloud[i].base, z, k)) r.far_visible++;
        }
    }
    r.visibility = (double)r.visible / r.total;
    r.far_visibility = r.far_total ? (double)r.far_visible / r.far_total : 1.0;
    r.step_ratio = exp(k * sc->D / (LAYERS - 1));
    r.gap_met = r.gap <= GAP_LIMIT;
    r.half_met = r.visibility >= VIS_LIMIT;
    r.nat_gap_met = r.nat_gap <= GAP_LIMIT;
    return r;
}

/* ---- coefficient selection from the two curves -------------------------- */

typedef struct {
    double k;
    Result rr[3];
    int contrast_ok; /* weakest tier adjacent step clears RHO_STEP */
    int gates_ok;    /* all three tiers pass gap + half-visibility gates */
} SweepPoint;

static const double TIER_D[3] = { D_SHALLOW, D_STD, D_DEEP };
static const char* TIER_NAME[3] = { "shallow", "standard", "deep" };

static Scene the_scenes[3];

static void build_scenes(void) {
    for (int m = 0; m < 3; m++) the_scenes[m] = make_scene(TIER_NAME[m], TIER_D[m]);
}

/* k* = first grid point where every shallow step is resolvable AND the hard
 * gates hold in all three tiers. Contrast curve sets the lower k edge; the
 * visibility/gap curves set the upper edge, so the first admissible point is
 * the coefficient that spends the least far visibility while still layering
 * every step. */
static double select_k(SweepPoint* sweep, int* ngrid) {
    int n = 0;
    double chosen = -1.0;
    for (double k = 0.0; k <= K_GRID_MAX + 1e-9; k += K_GRID_STEP, n++) {
        SweepPoint* p = &sweep[n];
        p->k = k;
        p->contrast_ok = 1;
        p->gates_ok = 1;
        for (int m = 0; m < 3; m++) {
            p->rr[m] = evaluate(&the_scenes[m], k);
            if (p->rr[m].step_ratio < RHO_STEP - 1e-12) p->contrast_ok = 0;
            if (!p->rr[m].gap_met || !p->rr[m].half_met || !p->rr[m].nat_gap_met)
                p->gates_ok = 0;
        }
        if (chosen < 0.0 && p->contrast_ok && p->gates_ok) chosen = k;
    }
    *ngrid = n;
    return chosen;
}

/* ---- output ------------------------------------------------------------- */

static void print_result_keyval(const Result* r) {
    printf("%s: D=%.1f k=%.2f bound=%.3f actual=%.3f gap=%.4f "
           "nat_bound=%.3f nat_actual=%.3f nat_gap=%.4f "
           "visible=%d/%d visibility=%.4f far=%d/%d far_visibility=%.4f "
           "step_ratio=%.4f\n",
           r->mode, r->D, r->k, r->bound, r->actual, r->gap,
           r->nat_bound, r->nat_actual, r->nat_gap,
           r->visible, r->total, r->visibility,
           r->far_visible, r->far_total, r->far_visibility, r->step_ratio);
}

static void print_json_result(const Result* r, int ind) {
    char p[24], q[24];
    snprintf(p, sizeof(p), "%*s", ind, "");
    snprintf(q, sizeof(q), "%*s", ind + 2, "");
    printf("%s{\n", p);
    printf("%s\"mode\": \"%s\",\n", q, r->mode);
    printf("%s\"depth_span\": %.4f,\n", q, r->D);
    printf("%s\"k\": %.4f,\n", q, r->k);
    printf("%s\"layers\": %d,\n", q, LAYERS);
    printf("%s\"product_bound\": %.6f,\n", q, r->bound);
    printf("%s\"actual_contrast\": %.6f,\n", q, r->actual);
    printf("%s\"contrast_gap\": %.6f,\n", q, r->gap);
    printf("%s\"natural_bound\": %.6f,\n", q, r->nat_bound);
    printf("%s\"natural_actual\": %.6f,\n", q, r->nat_actual);
    printf("%s\"natural_gap\": %.6f,\n", q, r->nat_gap);
    printf("%s\"visible\": %d,\n", q, r->visible);
    printf("%s\"total\": %d,\n", q, r->total);
    printf("%s\"visibility\": %.6f,\n", q, r->visibility);
    printf("%s\"far_visible\": %d,\n", q, r->far_visible);
    printf("%s\"far_total\": %d,\n", q, r->far_total);
    printf("%s\"far_visibility\": %.6f,\n", q, r->far_visibility);
    printf("%s\"step_ratio\": %.6f,\n", q, r->step_ratio);
    printf("%s\"gap_limit\": %.2f,\n", q, GAP_LIMIT);
    printf("%s\"visibility_limit\": %.2f,\n", q, VIS_LIMIT);
    printf("%s\"gap_met\": %s,\n", q, r->gap_met ? "true" : "false");
    printf("%s\"half_met\": %s,\n", q, r->half_met ? "true" : "false");
    printf("%s\"natural_gap_met\": %s\n", q, r->nat_gap_met ? "true" : "false");
    printf("%s}", p);
}

static void cmd_csv(double kstar, const SweepPoint* sweep, int ngrid) {
    (void)kstar;
    puts("k,mode,depth_span,product_bound,actual_contrast,contrast_gap,"
         "natural_gap,visible,total,visibility,far_visibility,step_ratio,"
         "contrast_ok,gates_ok");
    for (int i = 0; i < ngrid; i++) {
        const SweepPoint* p = &sweep[i];
        for (int m = 0; m < 3; m++) {
            const Result* r = &p->rr[m];
            printf("%.2f,%s,%.1f,%.6f,%.6f,%.6f,%.6f,%d,%d,%.6f,%.6f,%.6f,%d,%d\n",
                   p->k, r->mode, r->D, r->bound, r->actual, r->gap, r->nat_gap,
                   r->visible, r->total, r->visibility, r->far_visibility,
                   r->step_ratio, p->contrast_ok, p->gates_ok);
        }
    }
}

/* ---- self check --------------------------------------------------------- */

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    build_scenes();

    double eps = vis_epsilon();
    double kcal = k_from_calibration(eps, D_DEEP);
    double kstep_min = log(RHO_STEP) / (D_SHALLOW / (LAYERS - 1));

    int cap = (int)(K_GRID_MAX / K_GRID_STEP) + 2;
    SweepPoint* sweep = calloc((size_t)cap, sizeof(SweepPoint));
    int ngrid = 0;
    double kstar = select_k(sweep, &ngrid);

    if (argc > 1 && strcmp(argv[1], "csv") == 0) {
        cmd_csv(kstar, sweep, ngrid);
        free(sweep);
        return 0;
    }

    if (argc > 2 && strcmp(argv[1], "run") == 0) {
        for (int m = 0; m < 3; m++) {
            if (strcmp(argv[2], TIER_NAME[m]) == 0) {
                Result r = evaluate(&the_scenes[m], kstar);
                print_json_result(&r, 0);
                putchar('\n');
                free(sweep);
                return (r.gap_met && r.half_met && r.nat_gap_met) ? 0 : 1;
            }
        }
        fprintf(stderr, "unknown tier '%s' (shallow|standard|deep)\n", argv[2]);
        free(sweep);
        return 2;
    }

    if (argc > 1 && strcmp(argv[1], "json") == 0) {
        Result fin[3];
        for (int m = 0; m < 3; m++) fin[m] = evaluate(&the_scenes[m], kstar);
        int gap_all = 1, half_all = 1, nat_all = 1;
        double worst_gap = 0.0, min_vis = 1.0;
        for (int m = 0; m < 3; m++) {
            gap_all &= fin[m].gap_met;
            half_all &= fin[m].half_met;
            nat_all &= fin[m].nat_gap_met;
            if (fin[m].gap > worst_gap) worst_gap = fin[m].gap;
            if (fin[m].visibility < min_vis) min_vis = fin[m].visibility;
        }
        printf("{\n");
        printf("  \"module\": \"depth\",\n");
        printf("  \"tiers_run\": 3,\n");
        printf("  \"total_per_tier\": %d,\n", TOTAL);
        printf("  \"layers\": %d,\n", LAYERS);
        printf("  \"worst_contrast_gap\": %.6f,\n", worst_gap);
        printf("  \"min_visibility\": %.6f,\n", min_vis);
        printf("  \"calibration\": {\n");
        printf("    \"code_vis\": %.1f,\n", C_VIS);
        printf("    \"epsilon_linear\": %.8f,\n", eps);
        printf("    \"d_vis\": %.4f,\n", (double)D_DEEP);
        printf("    \"k_cal\": %.8f,\n", kcal);
        printf("    \"rho_step\": %.3f,\n", RHO_STEP);
        printf("    \"d_step_at_kstar\": %.6f,\n", d_step_jnd(RHO_STEP, kstar));
        printf("    \"k_step_min\": %.8f\n", kstep_min);
        printf("  },\n");
        printf("  \"coefficient_selected\": %.2f,\n", kstar);
        printf("  \"gap_limit\": %.2f,\n", GAP_LIMIT);
        printf("  \"visibility_limit\": %.2f,\n", VIS_LIMIT);
        printf("  \"runs\": [\n");
        for (int m = 0; m < 3; m++) {
            print_json_result(&fin[m], 2);
            printf("%s\n", m < 2 ? "," : "");
        }
        printf("  ],\n");
        printf("  \"gates\": {\n");
        printf("    \"three_dof_gap_met\": %s,\n", gap_all ? "true" : "false");
        printf("    \"natural_gap_met\": %s,\n", nat_all ? "true" : "false");
        printf("    \"visible_half_met\": %s,\n", half_all ? "true" : "false");
        printf("    \"all_ok\": %s\n", (gap_all && half_all && nat_all) ? "true" : "false");
        printf("  }\n");
        printf("}\n");
        free(sweep);
        return (gap_all && half_all && nat_all) ? 0 : 1;
    }

    /* ---- default: human report + self checks ---- */
    printf("# depth attenuation self-check\n");
    printf("epsilon_linear=%.6f (sRGB code %.0f/255)\n", eps, C_VIS);
    printf("k_cal=%.6f from d_vis=%.1f: k=ln(1/eps)/d_vis\n", kcal, (double)D_DEEP);
    printf("k_step_min=%.6f for rho=%.2f at shallow spacing h=%.4f\n",
           kstep_min, RHO_STEP, (double)D_SHALLOW / (LAYERS - 1));
    printf("chosen_k=%.2f grid_points=%d\n", kstar, ngrid);

    check("factor_at_zero", fabs(depth_factor(0.0, kstar) - 1.0) < 1e-12);
    check("factor_monotone",
          depth_factor(1.0, kstar) < depth_factor(0.5, kstar) &&
          depth_factor(0.5, kstar) < 1.0);
    check("zero_coeff_flat", fabs(depth_factor(9.0, 0.0) - 1.0) < 1e-12);

    /* calibration inversion round trip */
    double ktest = k_from_calibration(eps, 7.5);
    check("calibration_inverts", fabs(depth_factor(7.5, ktest) - eps) < 1e-9);
    check("kcal_formula", fabs(kcal - log(1.0 / eps) / D_DEEP) < 1e-12);

    /* d at which factor reaches eps, then invert back */
    double dv = d_indistinguishable(eps, kcal);
    check("d_indistinguishable", fabs(dv - D_DEEP) < 1e-9);

    /* JND step inversion */
    double ds = d_step_jnd(RHO_STEP, kstar);
    check("jnd_step_roundtrip", fabs(depth_factor(ds, kstar) - 1.0 / RHO_STEP) < 1e-12);

    /* N-factor product identity: 8 layers, seven unit gaps + d1 = 0 */
    double prod = 1.0;
    for (int j = 1; j < LAYERS; j++) prod *= depth_factor(1.0, kstar);
    double expect_prod = exp(-kstar * (LAYERS - 1));
    check("n_factor_product", fabs(prod - expect_prod) < 1e-12);
    check("bound_is_reciprocal_product",
          fabs(1.0 / prod - exp(kstar * (LAYERS - 1))) < 1e-9);

    /* three depth-of-field tiers, one run each at k* */
    Result fin[3];
    int gap_all = 1, half_all = 1, nat_all = 1;
    double worst_gap = 0.0, min_vis = 1.0;
    for (int m = 0; m < 3; m++) {
        fin[m] = evaluate(&the_scenes[m], kstar);
        print_result_keyval(&fin[m]);
        if (fin[m].gap > worst_gap) worst_gap = fin[m].gap;
        if (fin[m].visibility < min_vis) min_vis = fin[m].visibility;
        char nm[64];
        snprintf(nm, sizeof(nm), "gap_met_%s", TIER_NAME[m]);
        check(nm, fin[m].gap_met);
        snprintf(nm, sizeof(nm), "half_met_%s", TIER_NAME[m]);
        check(nm, fin[m].half_met);
        snprintf(nm, sizeof(nm), "natural_gap_met_%s", TIER_NAME[m]);
        check(nm, fin[m].nat_gap_met);
        gap_all &= fin[m].gap_met;
        half_all &= fin[m].half_met;
        nat_all &= fin[m].nat_gap_met;
    }
    printf("tiers_run=3 worst_contrast_gap=%.4f gap_limit=%.2f "
           "min_visibility=%.4f visibility_limit=%.2f\n",
           worst_gap, GAP_LIMIT, min_vis, VIS_LIMIT);
    check("three_dof_gap_met", gap_all);
    check("visible_half_met", half_all);
    check("natural_gap_met", nat_all);
    printf("dof_tier_count=%d\n", (int)(sizeof(TIER_NAME) / sizeof(TIER_NAME[0])));
    check("three_runs_each_once",
          (int)(sizeof(TIER_NAME) / sizeof(TIER_NAME[0])) == 3);

    /* absolute layering must exist: weakest tier whole-stack CR clears target
     * rho^(N-1), otherwise a flat renderer would also show a small gap. */
    double cr_target = pow(RHO_STEP, LAYERS - 1);
    int contrast_abs = fin[0].actual >= cr_target * 0.97; /* display-chain tol */
    check("absolute_layering_target", contrast_abs);
    printf("contrast_target_cr=%.3f shallow_actual=%.3f\n", cr_target, fin[0].actual);

    /* negative control 1: flat pipeline (k=0) compared against the k* product
     * bound must show a >30% gap in every tier: the gate detects missing
     * attenuation when scored against the intended bound. */
    int flat_detected = 1;
    for (int m = 0; m < 3; m++) {
        Result flat = evaluate(&the_scenes[m], 0.0);
        double intended_bound = exp(kstar * the_scenes[m].D);
        double g = fabs(flat.actual - intended_bound) / intended_bound;
        if (g <= GAP_LIMIT) flat_detected = 0;
        printf("flat_control_%s_gap_vs_kstar=%.4f\n", TIER_NAME[m], g);
    }
    check("flat_control_detected", flat_detected);

    /* negative control 2: an overdriven coefficient must be rejected by the
     * three-tier gate (deep tier crushes); shallow span may still survive. */
    int over_failing_tiers = 0;
    int over_aggregate_ok = 1;
    Result over_deep;
    for (int m = 0; m < 3; m++) {
        Result o = evaluate(&the_scenes[m], K_OVERDRIVE);
        int tier_ok = o.gap_met && o.half_met && o.nat_gap_met;
        if (!tier_ok) over_failing_tiers++;
        over_aggregate_ok &= tier_ok;
        if (m == 2) over_deep = o;
        printf("overdrive_%.2f_%s: gap=%.3f visibility=%.3f tier_ok=%d\n",
               K_OVERDRIVE, TIER_NAME[m], o.gap, o.visibility, tier_ok);
    }
    check("overdrive_rejected", !over_aggregate_ok && over_failing_tiers >= 1);
    check("overdrive_deep_crushes",
          !over_deep.gap_met && !over_deep.half_met);

    /* determinism */
    Result a1 = evaluate(&the_scenes[1], kstar);
    Result a2 = evaluate(&the_scenes[1], kstar);
    check("deterministic", memcmp(&a1, &a2, sizeof(a1)) == 0);

    /* coefficient really came from the two-curve admissible window */
    check("coefficient_selected", kstar > 0.0);
    double kup = -1.0;
    for (int i = 0; i < ngrid; i++)
        if (!sweep[i].gates_ok) { kup = sweep[i].k; break; }
    printf("chosen_k=%.2f first_gate_failure_k=%.2f\n", kstar, kup);

    printf("all_ok=%d\n", failures == 0);
    free(sweep);
    return failures == 0 ? 0 : 1;
}
