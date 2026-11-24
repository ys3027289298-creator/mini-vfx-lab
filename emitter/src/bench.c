#define _POSIX_C_SOURCE 199309L

/* bench: precision sweep for contribution-based eviction.
 *
 * Produces the two curves that jointly pick a precision level:
 *   1. eviction quality vs precision (mean retained reference contribution,
 *      ranking Kendall's tau against the double-precision reference)
 *   2. extra per-frame time vs precision (both isolated scoring cost and
 *      end-to-end update cost)
 * plus a before/after replay of one identical explosion under the legacy
 * drop policy and the eviction policy.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "emitter.h"

/* deterministic xorshift so both policies replay the exact same explosion */
static unsigned long rng_state;
static float rng_f(void) {
    unsigned long x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (float)(x >> 11) / (float)(1UL << 53);
}
static float rng_range(float lo, float hi) { return lo + rng_f() * (hi - lo); }

typedef struct {
    float x, y, vx, vy, life, size, bright;
} Req;

#define NREQ 160
#define NFRAMES 60
#define DT 0.033f
#define TAIL_LO 128   /* tail particle: request id in [TAIL_LO, NREQ) */

static void build_explosion(Req req[NREQ]) {
    rng_state = 0x9e3779b97f4a7c15UL;
    for (int i = 0; i < NREQ; i++) {
        float ang = rng_range(0.0f, 6.2832f);
        float spd = rng_range(1.0f, 9.0f);
        req[i].x = rng_range(-0.5f, 0.5f);
        req[i].y = rng_range(-0.5f, 0.5f);
        req[i].vx = cosf(ang) * spd;
        req[i].vy = sinf(ang) * spd;
        /* lives span up to 6s so the last simulated frame (1.98s)
         * still holds survivors */
        req[i].life = rng_range(0.8f, 6.0f);
        req[i].size = rng_range(0.2f, 1.5f);
        req[i].bright = rng_range(0.15f, 1.0f);
    }
}

static double pool_ref_sum(const Emitter* e) {
    double s = 0.0;
    int k = 0;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        s += em_ref_score(&e->p[i]);
        k++;
    }
    return k ? s / k : 0.0;
}

typedef struct {
    double mean_ref;      /* time-averaged mean reference contribution */
    double final_ref;     /* mean reference contribution in the final frame */
    double sum_ref;       /* reference contribution accumulated over frames */
    int max_admitted;     /* highest spawn id ever admitted */
    int tail_admitted;    /* tail request ids (>=TAIL_LO) ever admitted */
    int tail_in_pool;     /* tail ids alive in the final frame */
    int alive_final;
    int emitted, evicted, rejected, dropped;
} RunStat;

static RunStat replay(Policy policy, int level, const Req req[NREQ],
                      int burst_at_frame0) {
    Emitter e;
    em_init(&e, policy, level);
    RunStat st;
    memset(&st, 0, sizeof(st));
    int admitted_mark[NREQ] = {0};
    double acc = 0.0;
    for (int fr = 0; fr < NFRAMES; fr++) {
        if (fr == 0 && burst_at_frame0) {
            for (int i = 0; i < NREQ; i++) {
                int slot = em_spawn(&e, req[i].x, req[i].y, req[i].vx,
                                    req[i].vy, req[i].life, req[i].size,
                                    req[i].bright);
                if (slot >= 0) admitted_mark[i] = 1;
            }
        }
        em_update(&e, DT, 0, 0);
        acc += pool_ref_sum(&e);
    }
    st.mean_ref = acc / NFRAMES;
    st.final_ref = pool_ref_sum(&e);
    st.sum_ref = acc;
    st.emitted = e.emitted;
    st.evicted = e.evicted;
    st.rejected = e.rejected;
    st.dropped = e.dropped;
    for (int i = TAIL_LO; i < NREQ; i++)
        if (admitted_mark[i]) st.tail_admitted++;
    for (int i = 0; i < e.n; i++) {
        if (!e.p[i].alive) continue;
        st.alive_final++;
        if (e.p[i].id > st.max_admitted) st.max_admitted = e.p[i].id;
        if (e.p[i].id >= TAIL_LO) st.tail_in_pool++;
    }
    /* highest id admitted anywhere (including later-evicted) via audit */
    int max_any = -1;
    for (int i = 0; i < e.nlog; i++) {
        int id = e.log[i].incoming_id;
        if (id > max_any) max_any = id;
    }
    if (max_any > st.max_admitted) st.max_admitted = max_any;
    return st;
}

/* Kendall tau of the quantization-only probe (equal weights, fixed L4 model,
 * only bins vary) against exact floats. With every other factor held fixed,
 * finer bins can only preserve more ordering information -> monotone tau. */
static double kendall_tau(int level) {
    enum { M = 200 };
    P probe[M];
    rng_state = 0x1234567UL;
    for (int i = 0; i < M; i++) {
        memset(&probe[i], 0, sizeof(probe[i]));
        probe[i].x = rng_range(-10.0f, 10.0f);
        probe[i].y = rng_range(-10.0f, 10.0f);
        probe[i].age = rng_range(0.0f, 2.0f);
        probe[i].life = rng_range(0.5f, 4.0f);
        probe[i].size = rng_range(0.1f, 1.5f);
        probe[i].bright = rng_range(0.1f, 1.0f);
        probe[i].alive = 1;
    }
    double ref[M]; float ls[M];
    for (int i = 0; i < M; i++) {
        ref[i] = (double)em_quant_probe(&probe[i], NLEVELS - 1);
        ls[i] = em_quant_probe(&probe[i], level);
    }
    long conc = 0, disc = 0;
    for (int i = 0; i < M; i++)
        for (int j = i + 1; j < M; j++) {
            double dr = ref[i] - ref[j];
            float dl = ls[i] - ls[j];
            if (dr == 0.0 || dl == 0.0f) continue;
            if ((dr > 0) == (dl > 0)) conc++; else disc++;
        }
    long den = conc + disc;
    return den ? (double)(conc - disc) / den : 1.0;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* isolated scoring micro-benchmark: cost per particle per frame.
 * Median of NROUNDS rounds to suppress scheduling noise. */
static int dbl_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double bench_scoring(int level) {
    enum { M = 256, REPS = 4000, NROUNDS = 7 };
    P probe[M];
    rng_state = 0xabcdefUL;
    for (int i = 0; i < M; i++) {
        memset(&probe[i], 0, sizeof(probe[i]));
        probe[i].x = rng_range(-10.0f, 10.0f);
        probe[i].y = rng_range(-10.0f, 10.0f);
        probe[i].age = rng_range(0.0f, 2.0f);
        probe[i].life = rng_range(0.5f, 4.0f);
        probe[i].size = rng_range(0.1f, 1.5f);
        probe[i].bright = rng_range(0.1f, 1.0f);
    }
    double rounds[NROUNDS];
    float sink = 0.0f;
    for (int k = 0; k < NROUNDS; k++) {
        double t0 = now_sec();
        for (int r = 0; r < REPS; r++)
            for (int i = 0; i < M; i++)
                sink += em_score(&probe[i], level);
        double t1 = now_sec();
        rounds[k] = (t1 - t0) / (REPS * M) * 1e9;
    }
    if (sink == 12345.0f) printf("x"); /* prevent dead-code elimination */
    qsort(rounds, NROUNDS, sizeof(double), dbl_cmp);
    return rounds[NROUNDS / 2]; /* ns per particle score */
}

/* end-to-end frame cost on a steady saturated workload (pool stays full) */
static double bench_frame(Policy policy, int level, double* extra) {
    enum { FRAMES = 20000, NROUNDS = 5 };
    double main_r[NROUNDS], base_r[NROUNDS];
    for (int k = 0; k < NROUNDS; k++) {
        Emitter e;
        em_init(&e, policy, level);
        for (int i = 0; i < MAXP; i++)
            em_spawn(&e, 0, 0, 0.5f, 0.5f, 5.0f, 0.8f, 0.8f);
        double t0 = now_sec();
        for (int fr = 0; fr < FRAMES; fr++)
            em_update(&e, DT, 30.0f, 0);  /* ~1 new particle per frame, full */
        double t1 = now_sec();
        main_r[k] = (t1 - t0) / FRAMES * 1e6;

        Emitter base;
        em_init(&base, POLICY_DROP, 0);
        for (int i = 0; i < MAXP; i++)
            em_spawn(&base, 0, 0, 0.5f, 0.5f, 5.0f, 0.8f, 0.8f);
        t0 = now_sec();
        for (int fr = 0; fr < FRAMES; fr++)
            em_update(&base, DT, 30.0f, 0);
        t1 = now_sec();
        base_r[k] = (t1 - t0) / FRAMES * 1e6;
    }
    qsort(main_r, NROUNDS, sizeof(double), dbl_cmp);
    qsort(base_r, NROUNDS, sizeof(double), dbl_cmp);
    double per_frame_us = main_r[NROUNDS / 2];
    *extra = per_frame_us - base_r[NROUNDS / 2];
    return per_frame_us;
}

static void ascii_curve(const char* title, const double y[NLEVELS],
                        const char* ylabel) {
    printf("\n%s\n", title);
    double lo = y[0], hi = y[0];
    for (int i = 1; i < NLEVELS; i++) {
        if (y[i] < lo) lo = y[i];
        if (y[i] > hi) hi = y[i];
    }
    if (hi - lo < 1e-12) hi = lo + 1.0;
    enum { H = 8 };
    for (int row = H; row >= 1; row--) {
        double threshold = lo + (hi - lo) * (row - 0.5) / H;
        printf("|");
        for (int i = 0; i < NLEVELS; i++)
            putchar(y[i] >= threshold ? '*' : ' ');
        if (row == H) printf("  %.4g %s", hi, ylabel);
        if (row == 1) printf("  %.4g %s", lo, ylabel);
        putchar('\n');
    }
    printf("+");
    for (int i = 0; i < NLEVELS; i++) putchar('-');
    printf("\n L0   L1   L2   L3   L4   (scoring precision)\n");
}

int main(void) {
    Req req[NREQ];
    build_explosion(req);

    /* ---- before / after: same explosion, legacy vs eviction -------------- */
    RunStat before = replay(POLICY_DROP, 0, req, 1);
    printf("=== explosion replay: %d particles requested, %d frames ===\n",
           NREQ, NFRAMES);
    printf("%-28s %14s %14s\n", "metric", "legacy drop", "evict L4");
    RunStat after = replay(POLICY_EVICT, 4, req, 1);
    printf("%-28s %14d %14d\n", "total spawn requests", NREQ, NREQ);
    printf("%-28s %14d %14d\n", "ever admitted to pool",
           before.emitted, after.emitted);
    printf("%-28s %14d %14d\n", "dropped newcomers", before.dropped, after.dropped);
    printf("%-28s %14d %14d\n", "rejected (score contest)",
           before.rejected, after.rejected);
    printf("%-28s %14d %14d\n", "evicted incumbents",
           before.evicted, after.evicted);
    printf("%-28s %14d %14d\n", "alive in final frame",
           before.alive_final, after.alive_final);
    printf("%-28s %14d %14d\n", "highest request id admitted",
           before.max_admitted, after.max_admitted);
    printf("%-28s %14d %14d\n", "tail particles ever admitted (id>=128)",
           before.tail_admitted, after.tail_admitted);
    printf("%-28s %14d %14d\n", "tail particles alive in final frame",
           before.tail_in_pool, after.tail_in_pool);
    printf("%-28s %14.6f %14.6f\n", "mean retained contribution (avg frame)",
           before.mean_ref, after.mean_ref);
    printf("%-28s %14.6f %14.6f\n", "mean retained contribution (last frame)",
           before.final_ref, after.final_ref);
    double gain = (after.mean_ref - before.mean_ref) / before.mean_ref * 100.0;
    double gain_last = before.final_ref > 1e-9
        ? (after.final_ref - before.final_ref) / before.final_ref * 100.0
        : 0.0;
    printf("retained-contribution gain: %+.1f%% (avg frame), %+.1f%% (last frame)\n",
           gain, gain_last);

    /* ---- curve 1: quality vs precision ----------------------------------- */
    double q_mean[NLEVELS], tau[NLEVELS];
    printf("\n=== curve 1: eviction quality vs precision ===\n");
    printf("%-6s %-22s %12s %12s %12s %12s\n",
           "level", "weights(l,a,b)", "mean_contrib", "vs_legacy", "tau", "bins");
    for (int lv = 0; lv < NLEVELS; lv++) {
        RunStat st = replay(POLICY_EVICT, lv, req, 1);
        q_mean[lv] = st.mean_ref;
        tau[lv] = kendall_tau(lv);
        int bins[3]; float w[3]; const char* nm;
        em_level_config(lv, bins, w, &nm);
        printf("L%-5d (%.3f,%.3f,%.3f) %12.6f %+11.1f%% %12.4f (%d,%d,%d)\n",
               lv, w[0], w[1], w[2], q_mean[lv],
               (q_mean[lv] - before.mean_ref) / before.mean_ref * 100.0,
               tau[lv], bins[0], bins[1], bins[2]);
        (void)nm;
    }
    ascii_curve("quality curve: mean retained contribution (ref scale)",
                q_mean, "contrib");
    ascii_curve("quality curve: Kendall tau vs exact-float same-model order",
                tau, "tau");

    /* ---- curve 2: per-frame extra time vs precision ---------------------- */
    printf("\n=== curve 2: extra per-frame time vs precision ===\n");
    double score_ns[NLEVELS], frame_us[NLEVELS], extra_us[NLEVELS];
    printf("%-6s %18s %22s %18s\n", "level", "ns per particle score",
           "us per frame (total)", "extra us vs legacy");
    for (int lv = 0; lv < NLEVELS; lv++) {
        score_ns[lv] = bench_scoring(lv);
        frame_us[lv] = bench_frame(POLICY_EVICT, lv, &extra_us[lv]);
        printf("L%-5d %18.3f %22.3f %18.3f\n",
               lv, score_ns[lv], frame_us[lv], extra_us[lv]);
    }
    double legacy_frame;
    double dummy;
    legacy_frame = bench_frame(POLICY_DROP, 0, &dummy);
    printf("legacy drop policy baseline: %.3f us/frame\n", legacy_frame);
    ascii_curve("cost curve: ns per particle score", score_ns, "ns");
    ascii_curve("cost curve: extra us per frame vs legacy", extra_us, "us");

    /* monotonicity: the precision curves must not bend backwards.
     * tau (pure quantization ordering) is strictly non-decreasing.
     * mean contribution may wobble by <=1% between adjacent levels because
     * cheaper levels use cheaper *models* (falloff/envelope approximation),
     * not just coarser bins. */
    int tau_mono = 1, contrib_near_mono = 1;
    for (int lv = 1; lv < NLEVELS; lv++) {
        if (tau[lv] < tau[lv - 1] - 1e-9) tau_mono = 0;
        double wobble = (q_mean[lv - 1] - q_mean[lv]) / q_mean[lv - 1];
        if (wobble > 0.01) contrib_near_mono = 0;
    }
    printf("curve checks: tau_monotone=%d contribution_wobble<=1%%=%d\n",
           tau_mono, contrib_near_mono);

    /* machine-readable curves for external plotting */
    FILE* csv = fopen("../artifacts/precision_curves.csv", "w");
    if (!csv) csv = fopen("artifacts/precision_curves.csv", "w");
    if (csv) {
        fprintf(csv, "level,mean_contrib,vs_legacy_pct,kendall_tau,"
                     "score_ns_per_particle,frame_us,extra_us\n");
        for (int lv = 0; lv < NLEVELS; lv++)
            fprintf(csv, "L%d,%.6f,%.2f,%.4f,%.3f,%.3f,%.3f\n", lv,
                    q_mean[lv],
                    (q_mean[lv] - before.mean_ref) / before.mean_ref * 100.0,
                    tau[lv], score_ns[lv], frame_us[lv], extra_us[lv]);
        fclose(csv);
        printf("\ncurves written to artifacts/precision_curves.csv\n");
    }

    /* ---- joint precision decision ---------------------------------------- */
    printf("\n=== joint decision ===\n");
    /* knee: smallest level capturing >=90%% of the available quality gain
     * (L0 -> L4 span), while staying below half of L4's extra frame cost */
    double span = q_mean[NLEVELS - 1] - q_mean[0];
    double cost_cap = 0.5 * extra_us[NLEVELS - 1];
    int pick = -1;
    for (int lv = 0; lv < NLEVELS; lv++) {
        double captured = span > 0 ? (q_mean[lv] - q_mean[0]) / span : 1.0;
        if (captured >= 0.90 && extra_us[lv] <= cost_cap) { pick = lv; break; }
    }
    if (pick < 0) pick = NLEVELS - 1;
    printf("rule: smallest L with >=90%% of L0->L4 quality gain and "
           "<=50%% of L4 extra cost\n");
    printf("chosen precision: L%d (quality=%.6f tau=%.4f extra=%.3f us/frame)\n",
           pick, q_mean[pick], tau[pick], extra_us[pick]);

    /* ---- audit verification on the L4 explosion replay ------------------- */
    printf("\n=== audit spot check (evict L4 replay) ===\n");
    Emitter e;
    em_init(&e, POLICY_EVICT, 4);
    for (int i = 0; i < NREQ; i++)
        em_spawn(&e, req[i].x, req[i].y, req[i].vx, req[i].vy,
                 req[i].life, req[i].size, req[i].bright);
    for (int fr = 0; fr < NFRAMES; fr++) em_update(&e, DT, 0, 0);
    int losses = e.evicted + e.rejected;
    printf("loss events: evicted=%d rejected=%d audit_rows=%d overflow=%d\n",
           e.evicted, e.rejected, e.nlog, e.log_overflow);
    int reason_bad = 0, query_miss = 0;
    int reason_count[8] = {0};
    for (int i = 0; i < e.nlog; i++) {
        const Audit* a = &e.log[i];
        const Audit* q = em_audit_find(&e, a->kind == AUDIT_EVICT
                                              ? a->victim_id
                                              : a->incoming_id);
        if (!q) query_miss++;
        if (a->kind == AUDIT_EVICT) {
            int again = em_reason_from_factors(a->level, a->fl, a->fa, a->fb);
            if (again != a->reason) reason_bad++;
        }
        reason_count[a->reason]++;
    }
    printf("query misses=%d (must be 0), reason mismatches=%d (must be 0)\n",
           query_miss, reason_bad);
    for (int r = 0; r < 8; r++)
        if (reason_count[r])
            printf("  reason %-28s %d\n", em_reason_str(r), reason_count[r]);
    printf("sample rows:\n");
    for (int s = 0; s < 5 && s < e.nlog; s++) {
        int i = s * (e.nlog / 5);
        printf("  frame=%d %s reason=%s victim=%d incoming=%d "
               "f=(%.3f,%.3f,%.3f) s=(%.4f vs %.4f)\n",
               e.log[i].frame, em_audit_kind_str(e.log[i].kind),
               em_reason_str(e.log[i].reason), e.log[i].victim_id,
               e.log[i].incoming_id, e.log[i].fl, e.log[i].fa, e.log[i].fb,
               e.log[i].victim_score, e.log[i].incoming_score);
    }

    int ok = losses == e.nlog && !query_miss && !reason_bad &&
             !e.log_overflow && gain > 0.0 && tau_mono && contrib_near_mono &&
             after.max_admitted == NREQ - 1 &&
             after.tail_admitted > before.tail_admitted &&
             after.tail_in_pool > before.tail_in_pool &&
             after.final_ref > before.final_ref;
    printf("\nbench_ok=%d\n", ok);
    return ok ? 0 : 1;
}
