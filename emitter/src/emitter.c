
/* emitter: rate + burst emission with lifetime expiry and capacity.
 *
 * Grain size model
 * ---------------
 * Sizes are not uniform: grain size S (>0) follows a right-skewed Gamma
 * distribution  S ~ Gamma(k, theta)  (many small grains, few large grains).
 *
 *   p(s) = s^(k-1) exp(-s/theta) / (Gamma(k) theta^k)
 *   mean E[S]   = k theta          -> target mean mu fixes theta* = mu / k
 *   var  Var[S] = k theta^2        = mu^2 / k
 *   standardized skewness = 2/sqrt(k)   (smaller k = more skewed)
 *
 * The shape k (the "skewness") is chosen from two opposing curves:
 *   coverage-area ratio C(k)  (rises with k: more even-sized grains cover
 *                             the emitter footprint more reliably)
 *   size variance       V(k)  (falls with k: large k collapses to one size)
 * We normalize both benefit curves over the candidate k grid and take the
 * maximin / product balance, which lands on k* = 2. A hard variance floor
 * VAR_FLOOR = 0.20 flags a run as degenerated to near-uniform.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define MAXP 64

/* ---- size distribution tuning ---------------------------------------- */
#define SIZE_K          2.0      /* selected Gamma shape (skewness)        */
#define SIZE_SAMPLES    20000    /* Monte-Carlo grains per mean-size tier   */
#define VAR_FLOOR       0.20     /* variance under this => uniform collapse */
#define COV_REQ_FRAC    0.80     /* grain must reach 0.8*mean to cover cell  */
#define COV_LAMBDA      2.0      /* mean grains landing per coverage cell   */

typedef struct { float x, y, vx, vy; float age, life; int alive; float size; } P;
typedef struct { P p[MAXP]; int n; float rate_acc; int emitted; int dropped; } Emitter;

static void em_init(Emitter* e) { memset(e, 0, sizeof(*e)); }

static int em_spawn_sz(Emitter* e, float x, float y, float vx, float vy,
                       float life, float size) {
    if (life <= 0.0f || size < 0.0f) return -1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) {
            e->p[i] = (P){ x, y, vx, vy, 0.0f, life, 1, size };
            e->emitted++;
            return i;
        }
    }
    if (e->n >= MAXP) { e->dropped++; return -2; }
    e->p[e->n] = (P){ x, y, vx, vy, 0.0f, life, 1, size };
    e->emitted++;
    return e->n++;
}

/* legacy spawn: homogeneous nominal size 1.0 */
static int em_spawn(Emitter* e, float x, float y, float vx, float vy, float life) {
    return em_spawn_sz(e, x, y, vx, vy, life, 1.0f);
}

static int em_update(Emitter* e, float dt, float rate, int burst) {
    if (dt < 0.0f) return -1;
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
    int alive = 0;
    for (int i = 0; i < e->n; i++) if (e->p[i].alive) alive++;
    return alive;
}

/* ====================================================================== */
/* Deterministic RNG + Gamma sampler (splitmix64, Marsaglia-Tsang)         */
/* ====================================================================== */
typedef struct { uint64_t state; int have_spare; double spare; } Rng;

static uint64_t rng_splitmix(Rng* r) {
    uint64_t z = (r->state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* uniform in [0,1) */
static double rng_uniform(Rng* r) {
    double u = (double)(rng_splitmix(r) >> 11) * 0x1.0p-53;
    return u < 1e-17 ? 1e-17 : u;   /* keep log() finite */
}

static double rng_normal(Rng* r) {
    if (r->have_spare) { r->have_spare = 0; return r->spare; }
    double u1 = rng_uniform(r), u2 = rng_uniform(r);
    double mag = sqrt(-2.0 * log(u1));
    r->spare = mag * sin(2.0 * 3.14159265358979323846 * u2);
    r->have_spare = 1;
    return mag * cos(2.0 * 3.14159265358979323846 * u2);
}

/* Gamma(k, 1): scale by theta = mean/k to hit a target mean. */
static double gamma_unit_sample(Rng* r, double k) {
    if (k < 1.0) {
        double u = rng_uniform(r);
        return gamma_unit_sample(r, k + 1.0) * pow(u, 1.0 / k);
    }
    double d = k - 1.0 / 3.0;
    double c = 1.0 / sqrt(9.0 * d);
    for (;;) {
        double x, v;
        do { x = rng_normal(r); v = 1.0 + c * x; } while (v <= 0.0);
        v = v * v * v;
        double u = rng_uniform(r);
        double x2 = x * x;
        if (u < 1.0 - 0.0331 * x2 * x2) return d * v;
        if (log(u) < 0.5 * x2 + d * (1.0 - v + log(v))) return d * v;
    }
}

/* ====================================================================== */
/* Moments + one size run                                                  */
/* ====================================================================== */
typedef struct { long n; double mean, m2; } Moments;

static void moments_add(Moments* m, double x) {
    m->n++;
    double d = x - m->mean;
    m->mean += d / (double)m->n;
    m->m2 += d * (x - m->mean);
}
static double moments_variance(const Moments* m) {
    return m->n > 1 ? m->m2 / (double)(m->n - 1) : 0.0;  /* unbiased */
}

typedef struct {
    long n;
    double target, k, theta;
    double sample_mean, variance, variance_norm;
    double frac_below_target, frac_above_2target;
} SizeRun;

/* One Monte-Carlo run for a target mean size mu and Gamma shape k. */
static SizeRun size_run(double target, double k, uint64_t seed, long n) {
    Rng rng = { seed, 0, 0.0 };
    Moments m = { 0, 0.0, 0.0 };
    double theta = target / k;                 /* mean inversion */
    long below = 0, above2 = 0;
    for (long i = 0; i < n; i++) {
        double s = theta * gamma_unit_sample(&rng, k);
        moments_add(&m, s);
        if (s < target) below++;
        if (s > 2.0 * target) above2++;
    }
    SizeRun r;
    r.n = n; r.target = target; r.k = k; r.theta = theta;
    r.sample_mean = m.mean;
    r.variance = moments_variance(&m);
    r.variance_norm = r.variance / (target * target);  /* CV^2, scale-free */
    r.frac_below_target = (double)below / n;
    r.frac_above_2target = (double)above2 / n;
    return r;
}

/* One run from an explicit uniform law on [lo,hi] (degeneration control). */
static SizeRun uniform_run(double lo, double hi, uint64_t seed, long n) {
    Rng rng = { seed, 0, 0.0 };
    Moments m = { 0, 0.0, 0.0 };
    for (long i = 0; i < n; i++) {
        double s = lo + (hi - lo) * rng_uniform(&rng);
        moments_add(&m, s);
    }
    SizeRun r;
    memset(&r, 0, sizeof(r));
    r.n = n; r.k = 0.0; r.theta = 0.0;
    r.target = 0.5 * (lo + hi);
    r.sample_mean = m.mean;
    r.variance = moments_variance(&m);
    r.variance_norm = r.variance / (r.target * r.target);
    return r;
}

/* ====================================================================== */
/* Coverage-area curve                                                     */
/*                                                                         */
/* Emitter footprint split into equal-area cells, lambda grains/cell. A    */
/* cell counts as covered when it receives a grain large enough to span    */
/* it (s >= c*mu). For integer k the Gamma upper tail is the finite       */
/* Poisson sum  Q(k,x) = e^-x sum_{j=0}^{k-1} x^j/j!, with x = c*k.        */
/* Thinning gives P(cell covered) = 1 - exp(-lambda * Q(k,c*k)), which    */
/* is exactly the covered area fraction (all cells equal area).           */
/* ====================================================================== */
static double gamma_tail_int(int k, double x) {
    double term = 1.0, sum = 1.0;             /* j = 0 term */
    for (int j = 1; j < k; j++) { term *= x / (double)j; sum += term; }
    return exp(-x) * sum;
}

static double coverage_ratio(int k, double cover_frac, double lambda) {
    double x = cover_frac * (double)k;
    double q = gamma_tail_int(k, x);
    return 1.0 - exp(-lambda * q);
}

/* ====================================================================== */
/* test harness output                                                     */
/* ====================================================================== */
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    /* ---------------- legacy emitter behaviour (unchanged) ----------- */
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

    /* ---------------- size parameter inversion ----------------------- */
    /* theta* = mu / k : k*theta must reproduce the target mean exactly. */
    double mu_demo = 1.0, theta_demo = mu_demo / SIZE_K;
    check("mean_inversion", fabs(SIZE_K * theta_demo - mu_demo) < 1e-9);
    check("inverse_scale_medium", fabs(theta_demo - 0.5) < 1e-9);
    check("negative_size_rejected",
          em_spawn_sz(&e, 0, 0, 0, 0, 1.0f, -0.1f) == -1);

    /* ---------------- trade-off curves over candidate k -------------- */
    enum { NK = 7 };
    const int ks[NK] = { 1, 2, 3, 4, 5, 6, 8 };
    double vnorm[NK], cov[NK], dv[NK], dc[NK], bal[NK], prod[NK];
    double vmin = 1e9, vmax = -1e9, cmin = 1e9, cmax = -1e9;
    for (int i = 0; i < NK; i++) {
        vnorm[i] = 1.0 / (double)ks[i];                 /* = Var / mu^2 */
        cov[i] = coverage_ratio(ks[i], COV_REQ_FRAC, COV_LAMBDA);
        if (vnorm[i] < vmin) vmin = vnorm[i];
        if (vnorm[i] > vmax) vmax = vnorm[i];
        if (cov[i] < cmin) cmin = cov[i];
        if (cov[i] > cmax) cmax = cov[i];
    }
    int pick_bal = 0, pick_prod = 0;
    for (int i = 0; i < NK; i++) {
        dv[i] = (vnorm[i] - vmin) / (vmax - vmin);
        dc[i] = (cov[i] - cmin) / (cmax - cmin);
        bal[i] = dv[i] < dc[i] ? dv[i] : dc[i];        /* maximin score */
        prod[i] = dv[i] * dc[i];                       /* product score */
        if (bal[i] > bal[pick_bal]) pick_bal = i;
        if (prod[i] > prod[pick_prod]) pick_prod = i;
        double skew = 2.0 / sqrt((double)ks[i]);
        printf("CURVE k=%d skew=%.4f coverage=%.4f variance_norm=%.4f "
               "d_var=%.4f d_cov=%.4f balance=%.4f product=%.4f\n",
               ks[i], skew, cov[i], vnorm[i], dv[i], dc[i], bal[i], prod[i]);
    }

    int cov_rises = 1, var_falls = 1;
    for (int i = 1; i < NK; i++) {
        if (!(cov[i] > cov[i - 1])) cov_rises = 0;
        if (!(vnorm[i] < vnorm[i - 1])) var_falls = 0;
    }
    check("coverage_curve_rises_with_k", cov_rises);
    check("variance_curve_falls_with_k", var_falls);
    check("balance_and_product_agree", pick_bal == pick_prod);
    check("skew_pick_k2", ks[pick_bal] == (int)SIZE_K);
    check("skew_pick_interior", pick_bal > 0 && pick_bal < NK - 1);
    /* normalized variance 0.2 floor is crossed exactly at k = 5 */
    check("variance_floor_crosses_k5",
          fabs(vnorm[4] - VAR_FLOOR) < 1e-9 && ks[4] == 5);
    printf("selected k=%d skew=%.4f coverage=%.4f variance_norm=%.4f\n",
           ks[pick_bal], 2.0 / sqrt((double)ks[pick_bal]),
           cov[pick_bal], vnorm[pick_bal]);

    /* ---------------- three target mean-size tiers, one run each ----- */
    const char* tag[3] = { "small", "medium", "large" };
    const double means[3] = { 0.80, 1.00, 1.20 };
    SizeRun tiers[3];
    int all_above_floor = 1, all_mean_match = 1;
    for (int t = 0; t < 3; t++) {
        tiers[t] = size_run(means[t], SIZE_K,
                            UINT64_C(0x51A7EED) + (uint64_t)t * 7919ULL,
                            SIZE_SAMPLES);
        const SizeRun* r = &tiers[t];
        int degenerate = r->variance < VAR_FLOOR;     /* literal abs rule */
        if (degenerate) all_above_floor = 0;
        if (fabs(r->sample_mean - means[t]) / means[t] > 0.03)
            all_mean_match = 0;

        printf("target_mean_%s=%.2f\n", tag[t], means[t]);
        printf("theta_%s=%.6f\n", tag[t], r->theta);
        printf("sample_mean_%s=%.6f\n", tag[t], r->sample_mean);
        printf("size_variance_%s=%.6f\n", tag[t], r->variance);
        printf("variance_norm_%s=%.6f\n", tag[t], r->variance_norm);
        printf("frac_below_mean_%s=%.4f frac_above_2mean_%s=%.4f\n",
               tag[t], r->frac_below_target, tag[t], r->frac_above_2target);
        /* named data flag for this tier (key=0/1, not an assertion) */
        printf("uniform_degenerate_%s=%d\n", tag[t], degenerate);

        char nm[64];
        snprintf(nm, sizeof nm, "mean_match_%s", tag[t]);
        check(nm, fabs(r->sample_mean - means[t]) / means[t] < 0.03);
        snprintf(nm, sizeof nm, "variance_theory_%s", tag[t]);
        double theory_var = means[t] * means[t] / SIZE_K;
        check(nm, fabs(r->variance - theory_var) / theory_var < 0.08);
        snprintf(nm, sizeof nm, "above_floor_%s", tag[t]);
        check(nm, r->variance >= VAR_FLOOR);
    }
    check("three_tiers_ran", tiers[0].n == SIZE_SAMPLES &&
                             tiers[1].n == SIZE_SAMPLES &&
                             tiers[2].n == SIZE_SAMPLES);
    check("all_tier_means_match", all_mean_match);
    check("selected_tiers_above_floor", all_above_floor);

    /* skew direction: at k=2 most grains are below the mean, few > 2x mean */
    check("many_small_grains", tiers[1].frac_below_target > 0.55);
    check("few_large_grains",
          tiers[1].frac_above_2target > 0.05 &&
          tiers[1].frac_above_2target < 0.14);

    /* ---------------- size actually reaches emitter particles --------- */
    Emitter se; em_init(&se);
    Rng sr = { UINT64_C(0xBEEFBEEF), 0, 0.0 };
    float smin = 1e9f, smax = -1e9f; double ssum = 0.0;
    for (int i = 0; i < 48; i++) {
        float sz = (float)(theta_demo * gamma_unit_sample(&sr, SIZE_K));
        int slot = em_spawn_sz(&se, 0, 0, 0, 0, 5.0f, sz);
        if (slot >= 0) {
            float got = se.p[slot].size;
            ssum += got;
            if (got < smin) smin = got;
            if (got > smax) smax = got;
        }
    }
    check("emitter_sizes_vary", (smax - smin) > 0.25f);
    check("emitter_sizes_positive", smin > 0.0f);
    check("emitter_size_mean_plausible",
          fabsf((float)(ssum / 48.0) - (float)mu_demo) < 0.30f);

    /* ---------------- degeneration controls (variance < 0.2) --------- */
    /* Gamma k=6 at mean 1: theory var = 1/6 = 0.1667 < 0.2 -> flagged. */
    SizeRun narrow = size_run(1.0, 6.0, UINT64_C(0xDEC0A7), SIZE_SAMPLES);
    printf("CONTROL kind=gamma_k6 sample_mean=%.6f size_variance=%.6f "
           "variance_norm=%.6f\n",
           narrow.sample_mean, narrow.variance, narrow.variance_norm);
    printf("uniform_degenerate_control_gamma_k6=%d\n",
           narrow.variance < VAR_FLOOR);
    check("gamma_k6_below_floor", narrow.variance < VAR_FLOOR);

    /* literal bounded uniform U[0.4,1.6]: var = 1.2^2/12 = 0.12 < 0.2 */
    SizeRun uni = uniform_run(0.4, 1.6, UINT64_C(0xF00DBA11), SIZE_SAMPLES);
    printf("CONTROL kind=uniform_0.4_1.6 sample_mean=%.6f "
           "size_variance=%.6f variance_norm=%.6f\n",
           uni.sample_mean, uni.variance, uni.variance_norm);
    printf("uniform_degenerate_control_uniform=%d\n",
           uni.variance < VAR_FLOOR);
    check("literal_uniform_below_floor", uni.variance < VAR_FLOOR);

    /* and the same law at the chosen k stays clearly above the floor */
    check("chosen_skew_not_degenerate", tiers[1].variance_norm >= VAR_FLOOR);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
