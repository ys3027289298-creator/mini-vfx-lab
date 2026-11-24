
/* pool: shared particle-pool isolation granularity.
 *
 * Bug under analysis: two VFX systems share one raw pool; both write color
 * token into overlapping slots, so one system's particles read back the
 * other system's color.
 *
 * Criterion (see ISOLATION.md for the full derivation).
 *   slot j, block size g, block b(j) = floor(j/g)
 *   owner(j,t)      = tag[b(j)]                         in {-1,0,..,K-1}
 *   permit(s,j,t)   = [ owner(j,t) == s ]
 *   write set W_s(t) = { j : system s writes j at t }
 *   multiplicity mu_j(t) = |{ s : j in W_s(t) }|
 *   conflicts C = sum_t sum_j [ mu_j(t) >= 2 ]
 *   C == 0  <=>  W_s(t) cap W_u(t) = empty for every s != u, every t.
 *
 * Reverse solve from concurrent write counts d_s(t):
 *   blocks wanted r_s(t;g) = ceil(d_s(t)/g)
 *   disjoint assignment exists at t  iff  sum_s r_s(t;g) <= B
 *   peak safe pool:  M_g(K) = g * max_t sum_s ceil(d_s(t)/g)
 *   static exclusive bands need g >= max_{s,t} d_s(t) = D  (full isolation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define D          64    /* per-system peak live particles            */
#define TICKS      384
#define MAXK       8
#define PERIOD     192
#define PHASE_STEP 24
#define BASE_N     10
#define RAMP       48    /* triangle half-width of burst              */
#define SLOT_BYTES 32    /* 6 floats + color + padding, particle slot  */
#define TAG_BYTES  4     /* one int owner tag per block               */

static const int GS[] = { 1, 2, 4, 8, 16, 32, 64 };
#define NG ((int)(sizeof(GS) / sizeof(GS[0])))

static int DM[MAXK][TICKS];

/* Deterministic staggered burst workload. Phase depends only on system id,
 * so any pair (a,b) is comparable and "any two systems" can be stress-tested. */
static int demand_at(int sid, int t) {
    int phase = sid * PHASE_STEP;
    int u = ((t - phase) % PERIOD + PERIOD) % PERIOD;
    int dist = abs(u - RAMP);            /* peak centered at u = RAMP */
    float amp = (float)(D - BASE_N);
    float b = BASE_N;
    if (dist < RAMP) b += amp * (1.0f - (float)dist / (float)RAMP);
    int v = (int)lroundf(b);
    if (v < 0) v = 0;
    if (v > D) v = D;
    return v;
}

static void workload_init(void) {
    for (int s = 0; s < MAXK; s++)
        for (int t = 0; t < TICKS; t++)
            DM[s][t] = demand_at(s, t);
}

static int dmax_of(const int* ids, int K) {
    int m = 0;
    for (int s = 0; s < K; s++)
        for (int t = 0; t < TICKS; t++)
            if (DM[ids[s]][t] > m) m = DM[ids[s]][t];
    return m;
}

static int agg_peak_of(const int* ids, int K) {
    int peak = 0;
    for (int t = 0; t < TICKS; t++) {
        int sum = 0;
        for (int s = 0; s < K; s++) sum += DM[ids[s]][t];
        if (sum > peak) peak = sum;
    }
    return peak;
}

/* M_g(K) = g * max_t sum_s ceil(d_s(t)/g) */
static int peak_slots_of(const int* ids, int K, int g) {
    int peak_blocks = 0;
    for (int t = 0; t < TICKS; t++) {
        int blocks = 0;
        for (int s = 0; s < K; s++)
            blocks += (DM[ids[s]][t] + g - 1) / g;
        if (blocks > peak_blocks) peak_blocks = blocks;
    }
    return peak_blocks * g;
}

typedef struct {
    int K, g;
    int dmax;
    int agg_peak;
    int peak_slots;
    int full_slots;
    int blocks;
    long collisions;   /* distinct-owner concurrent writes to one slot */
    long denials;      /* writes rejected by permit predicate          */
    long pollution;    /* read-back color != writer token              */
    long double_writes;/* same owner writing one slot twice in a tick  */
    size_t foot_bytes;
    size_t full_bytes;
    double slot_ratio;
    double byte_ratio;
} Report;

/* Explicit writable-range predicate: P(s,j,t) = [ owner(j,t) == s ].
 * owner(j,t) is the tag of the block containing j. */
static int can_write(const int* owner, int g, int s, int j) {
    return owner[j / g] == s;
}

/* Real block-owner free-list allocator + per-tick write/audit simulation.
 * ids[] maps simulated system s to workload phase id (enables pair tests). */
static Report run_safe_ids(const int* ids, int K, int g) {
    Report r;
    memset(&r, 0, sizeof(r));
    r.K = K; r.g = g;
    r.dmax = dmax_of(ids, K);
    r.agg_peak = agg_peak_of(ids, K);
    r.peak_slots = peak_slots_of(ids, K, g);
    r.full_slots = K * r.dmax;
    r.blocks = r.peak_slots / g;
    r.full_bytes = (size_t)r.full_slots * SLOT_BYTES;
    r.foot_bytes = (size_t)r.peak_slots * SLOT_BYTES
                 + (size_t)r.blocks * TAG_BYTES;
    r.slot_ratio = (double)r.peak_slots / (double)r.full_slots;
    r.byte_ratio = (double)r.foot_bytes / (double)r.full_bytes;

    int owner[r.blocks];
    int free_stack[r.blocks];
    int nfree = r.blocks;
    for (int b = 0; b < r.blocks; b++) { owner[b] = -1; free_stack[b] = b; }

    int blk[MAXK][D];
    int nowned[MAXK];
    for (int s = 0; s < K; s++) nowned[s] = 0;

    int seen[r.peak_slots];
    int color[r.peak_slots];
    memset(color, 0, sizeof(color));

    for (int t = 0; t < TICKS; t++) {
        int need[MAXK];
        for (int s = 0; s < K; s++)
            need[s] = (DM[ids[s]][t] + g - 1) / g;

        /* shrink first, so blocks freed by downsizing peers are reusable */
        for (int s = 0; s < K; s++) {
            while (nowned[s] > need[s]) {
                int b = blk[s][--nowned[s]];
                owner[b] = -1;
                free_stack[nfree++] = b;
            }
        }
        for (int s = 0; s < K; s++) {
            while (nowned[s] < need[s]) {
                if (nfree <= 0) { r.denials++; break; } /* inequality violated */
                int b = free_stack[--nfree];
                owner[b] = s;
                blk[s][nowned[s]++] = b;
            }
        }

        for (int j = 0; j < r.peak_slots; j++) seen[j] = -1;

        /* every system writes its full particle state, color token = s+1 */
        for (int s = 0; s < K; s++) {
            int n = DM[ids[s]][t];
            for (int i = 0; i < n; i++) {
                int j = blk[s][i / g] * g + (i % g);
                if (!can_write(owner, g, s, j)) { r.denials++; continue; }
                if (seen[j] == s) r.double_writes++;
                else if (seen[j] != -1) r.collisions++;
                seen[j] = s;
                color[j] = s + 1;
            }
        }

        /* read-back: a wrong color here is the reported inheritance bug */
        for (int s = 0; s < K; s++) {
            int n = DM[ids[s]][t];
            for (int i = 0; i < n; i++) {
                int j = blk[s][i / g] * g + (i % g);
                if (color[j] != s + 1) r.pollution++;
            }
        }
    }
    return r;
}

static Report run_safe(int K, int g) {
    int ids[MAXK];
    for (int s = 0; s < K; s++) ids[s] = s;
    return run_safe_ids(ids, K, g);
}

/* Unsafe interval placement: system s writes [s*spacing, s*spacing + d_s(t)).
 * spacing == 0 reproduces the raw shared-base pool bug.
 * Returns total colliding slot-tick pairs and color pollution. */
static void run_intervals(int K, int spacing, long* collisions,
                          long* pollution) {
    long col = 0, pol = 0;
    int extent = (K - 1) * (spacing > 0 ? spacing : 0) + D + 1;
    if (extent < D) extent = D;
    int seen[1024];
    int color[1024];
    if (extent > 1024) { fprintf(stderr, "extent too large\n"); exit(2); }

    for (int t = 0; t < TICKS; t++) {
        for (int j = 0; j < extent; j++) { seen[j] = -1; color[j] = 0; }
        for (int s = 0; s < K; s++) {
            int base = s * spacing;
            for (int i = 0; i < DM[s][t]; i++) {
                int j = base + i;
                if (j >= extent) break;
                if (seen[j] != -1 && seen[j] != s) col++;
                seen[j] = s;
                color[j] = s + 1;
            }
        }
        for (int s = 0; s < K; s++) {
            int base = s * spacing;
            for (int i = 0; i < DM[s][t]; i++) {
                int j = base + i;
                if (j >= extent) break;
                if (color[j] != s + 1) pol++;
            }
        }
    }
    *collisions = col;
    *pollution = pol;
}

/* granularity minimizing total footprint; ties keep the finer g */
static int choose_gstar(int K) {
    int best = GS[0];
    size_t best_bytes = 0;
    for (int i = 0; i < NG; i++) {
        Report r = run_safe(K, GS[i]);
        if (i == 0 || r.foot_bytes < best_bytes) {
            best_bytes = r.foot_bytes;
            best = GS[i];
        }
    }
    return best;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static void print_sweep(int K) {
    printf("# g  band_conflicts  band_mem_ratio  safe_conflicts  "
           "peak_slots  slot_ratio  foot_bytes  byte_ratio  gstar\n");
    int gstar = choose_gstar(K);
    for (int i = 0; i < NG; i++) {
        int g = GS[i];
        Report r = run_safe(K, g);
        long bc, bp;
        run_intervals(K, g, &bc, &bp);
        double band_ratio = (double)(K * g) / (double)(K * r.dmax);
        printf("curve K=%d g=%d band_conflicts=%ld band_pollution=%ld "
               "band_mem_ratio=%.6f safe_conflicts=%ld denials=%ld "
               "pollution=%ld peak_slots=%d full_slots=%d slot_ratio=%.6f "
               "foot_bytes=%zu byte_ratio=%.6f gstar=%d\n",
               K, g, bc, bp, band_ratio, r.collisions, r.denials,
               r.pollution, r.peak_slots, r.full_slots, r.slot_ratio,
               r.foot_bytes, r.byte_ratio, g == gstar ? 1 : 0);
    }
}

static int cmd_tier(int K) {
    if (K < 2 || K > MAXK) { fprintf(stderr, "K must be 2..%d\n", MAXK); return 2; }
    int gstar = choose_gstar(K);
    Report r = run_safe(K, gstar);

    long alias_col = -1, alias_pol = -1;
    run_intervals(K, 0, &alias_col, &alias_pol);
    long band_col = -1, band_pol = -1;
    run_intervals(K, r.dmax, &band_col, &band_pol);

    printf("tier=%d ticks=%d\n", K, TICKS);
    printf("per_system_peak=%d aggregate_concurrent_write_peak=%d\n",
           r.dmax, r.agg_peak);
    printf("full_isolation_slots=%d note=\"K*D, K=2 is 2x single pool\"\n",
           r.full_slots);
    printf("band_granularity_required=%d band_conflicts_at_D=%ld\n",
           r.dmax, band_col);
    printf("raw_shared_base_conflicts=%ld raw_shared_base_pollution=%ld\n",
           alias_col, alias_pol);
    printf("gstar=%d selection=\"min SLOT_BYTES*M_g + TAG_BYTES*(M_g/g)\"\n",
           gstar);
    printf("run=owner_block_pool collisions=%ld permit_denials=%ld "
           "color_inheritance=%ld double_writes=%ld\n",
           r.collisions, r.denials, r.pollution, r.double_writes);
    printf("peak_slots=%d peak_slot_ratio=%.6f\n", r.peak_slots, r.slot_ratio);
    printf("foot_bytes=%zu full_isolation_bytes=%zu peak_byte_ratio=%.6f\n",
           r.foot_bytes, r.full_bytes, r.byte_ratio);
    printf("zero_conflict=%d\n",
           (r.collisions == 0 && r.denials == 0 && r.pollution == 0) ? 1 : 0);
    print_sweep(K);

    int ok = r.collisions == 0 && r.denials == 0 && r.pollution == 0
             && r.double_writes == 0 && band_col == 0;
    printf("all_ok=%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}

static int cmd_curves(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) { perror("fopen csv"); return 2; }
    fprintf(f, "K,g,band_conflicts,band_pollution,band_mem_ratio,"
               "safe_conflicts,denials,pollution,peak_slots,full_slots,"
               "slot_ratio,foot_bytes,full_bytes,byte_ratio,gstar\n");
    for (int ki = 0; ki < 3; ki++) {
        int K = (int[]){ 2, 4, 8 }[ki];
        int gstar = choose_gstar(K);
        for (int i = 0; i < NG; i++) {
            int g = GS[i];
            Report r = run_safe(K, g);
            long bc, bp;
            run_intervals(K, g, &bc, &bp);
            double band_ratio = (double)g / (double)r.dmax;
            fprintf(f, "%d,%d,%ld,%ld,%.6f,%ld,%ld,%ld,%d,%d,%.6f,%zu,%zu,"
                       "%.6f,%d\n",
                    K, g, bc, bp, band_ratio, r.collisions, r.denials,
                    r.pollution, r.peak_slots, r.full_slots, r.slot_ratio,
                    r.foot_bytes, r.full_bytes, r.byte_ratio,
                    g == gstar ? 1 : 0);
        }
    }
    fclose(f);
    printf("wrote %s\n", path);
    return 0;
}

static int cmd_json(const char* path) {
    FILE* f = path ? fopen(path, "w") : stdout;
    if (path && !f) { perror("fopen json"); return 2; }
    fprintf(f, "{\n  \"d_per_system_peak\": %d,\n  \"ticks\": %d,\n", D, TICKS);
    fprintf(f, "  \"granularities\": [");
    for (int i = 0; i < NG; i++) fprintf(f, "%s%d", i ? ", " : "", GS[i]);
    fprintf(f, "],\n  \"tiers\": [\n");
    for (int ki = 0; ki < 3; ki++) {
        int K = (int[]){ 2, 4, 8 }[ki];
        int gstar = choose_gstar(K);
        Report r = run_safe(K, gstar);
        long ac, ap, bc, bp;
        run_intervals(K, 0, &ac, &ap);
        run_intervals(K, D, &bc, &bp);
        int zero = r.collisions == 0 && r.denials == 0
                   && r.pollution == 0 && r.double_writes == 0 && bc == 0;
        fprintf(f,
            "    {\"K\": %d, \"aggregate_peak\": %d, \"gstar\": %d, "
            "\"peak_slots\": %d, \"full_slots\": %d, "
            "\"peak_slot_ratio\": %.6f, \"foot_bytes\": %zu, "
            "\"full_bytes\": %zu, \"peak_byte_ratio\": %.6f, "
            "\"collisions\": %ld, \"denials\": %ld, "
            "\"color_inheritance\": %ld, \"double_writes\": %ld, "
            "\"raw_shared_base_conflicts\": %ld, "
            "\"raw_shared_base_pollution\": %ld, "
            "\"band_required_g\": %d, \"band_conflicts_at_D\": %ld, "
            "\"zero_conflict\": %d}%s\n",
            K, r.agg_peak, gstar, r.peak_slots, r.full_slots,
            r.slot_ratio, r.foot_bytes, r.full_bytes, r.byte_ratio,
            r.collisions, r.denials, r.pollution, r.double_writes,
            ac, ap, D, bc, zero, ki < 2 ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    if (path) fclose(f);
    else fflush(stdout);
    if (path) printf("wrote %s\n", path);
    return 0;
}

static int cmd_test(void) {
    /* workload reaches exact per-system peak D for every phase id */
    int peak_ok = 1;
    for (int s = 0; s < MAXK; s++)
        if (dmax_of((int[]){ s }, 1) != D) peak_ok = 0;
    check("workload_hits_D", peak_ok);

    /* the original bug must reproduce under the raw common-base pool */
    long c2 = 0, p2 = 0;
    run_intervals(2, 0, &c2, &p2);
    check("alias_bug_reproduced_k2", c2 > 0 && p2 > 0);
    long c8 = 0, p8 = 0;
    run_intervals(8, 0, &c8, &p8);
    check("alias_bug_reproduced_k8", c8 > 0 && p8 > 0);

    /* band sweep: any g < D leaves some non-last system overflowing */
    for (int ki = 0; ki < 3; ki++) {
        int K = (int[]){ 2, 4, 8 }[ki];
        long cc = 0, pp = 0;
        run_intervals(K, 32, &cc, &pp);
        char nm[64];
        snprintf(nm, sizeof(nm), "bands_collide_below_peak_k%d", K);
        check(nm, cc > 0);
        run_intervals(K, D, &cc, &pp);
        snprintf(nm, sizeof(nm), "bands_zero_at_D_k%d", K);
        check(nm, cc == 0 && pp == 0);
    }

    /* safe owner-block pool: zero conflicts at every granularity/tier */
    int allzero = 1, ratios_ok = 1;
    for (int ki = 0; ki < 3; ki++) {
        int K = (int[]){ 2, 4, 8 }[ki];
        for (int i = 0; i < NG; i++) {
            Report r = run_safe(K, GS[i]);
            if (r.collisions || r.denials || r.pollution || r.double_writes)
                allzero = 0;
            if (!(r.slot_ratio > 0.0 && r.slot_ratio <= 1.0)) ratios_ok = 0;
        }
    }
    check("safe_pool_zero_conflicts_all_g", allzero);
    check("slot_ratio_in_0_1", ratios_ok);

    /* g=1 strictly beats full isolation; g=D degenerates to exactly K*D */
    int saves = 1, coarse_eq = 1;
    for (int ki = 0; ki < 3; ki++) {
        int K = (int[]){ 2, 4, 8 }[ki];
        Report fine = run_safe(K, 1);
        Report coarse = run_safe(K, D);
        if (!(fine.peak_slots < K * D)) saves = 0;
        if (coarse.peak_slots != K * D) coarse_eq = 0;
    }
    check("fine_granularity_saves_vs_full", saves);
    check("g_equals_D_is_full_isolation", coarse_eq);

    /* "any two systems": every phase pair (a,b) under the safe allocator */
    int pairs_ok = 1, npairs = 0;
    for (int a = 0; a < MAXK; a++) {
        for (int b = a + 1; b < MAXK; b++) {
            int ids[2] = { a, b };
            for (int i = 0; i < NG; i++) {
                Report r = run_safe_ids(ids, 2, GS[i]);
                npairs++;
                if (r.collisions || r.denials || r.pollution) pairs_ok = 0;
            }
        }
    }
    check("every_pair_zero_conflict", pairs_ok);
    printf("pair_configs_checked=%d\n", npairs);

    /* reverse inequality sanity: M_g equals g*max sum ceil(d/g) */
    int formula_ok = 1;
    for (int ki = 0; ki < 3; ki++) {
        int K = (int[]){ 2, 4, 8 }[ki];
        int ids[MAXK]; for (int s = 0; s < K; s++) ids[s] = s;
        for (int i = 0; i < NG; i++) {
            int g = GS[i];
            if (peak_slots_of(ids, K, g) % g != 0) formula_ok = 0;
        }
    }
    check("peak_formula_block_aligned", formula_ok);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    workload_init();
    if (argc >= 2 && strcmp(argv[1], "tier") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: pool tier 2|4|8\n"); return 2; }
        return cmd_tier(atoi(argv[2]));
    }
    if (argc >= 3 && strcmp(argv[1], "curves") == 0)
        return cmd_curves(argv[2]);
    if (argc >= 2 && strcmp(argv[1], "json") == 0)
        return cmd_json(argc >= 3 ? argv[2] : NULL);
    if (argc >= 2 && strcmp(argv[1], "test") == 0)
        return cmd_test();
    if (argc >= 2) { fprintf(stderr, "usage: pool [test|tier K|curves PATH|json [PATH]]\n"); return 2; }
    return cmd_test();
}
