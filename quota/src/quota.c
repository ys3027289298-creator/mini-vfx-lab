/* quota: per-instance particle pools with quotas solved from a global cap.
 *
 * Isolation criterion: each effect instance owns its pool; instance i may
 * hold at most q_i particles and sum(q_i) <= G at all times, so a newly
 * added instance can never clear another instance's particles.
 *
 * Quota as a function of instance count n and occupancy o_i:
 *   r(n) = floor(BETA*G/n)         per-instance guaranteed floor
 *   R(n) = G - n*r(n)              distributable remainder (>= (1-BETA)*G)
 *   u_i  = max(0, demand_i - o_i)  unmet demand given current occupancy
 *   q_i  = r(n) + floor(R(n)*u_i / sum(u))     (no extra when sum(u) == 0)
 *
 * Proof: floors only shrink each term, so for any n >= 1 and any occupancy:
 *   sum(q_i) <= n*r(n) + R(n) * sum(u)/sum(u) = n*r(n) + G - n*r(n) = G.  QED
 *
 * Metrics:
 *   big drop rate  = 1 - (time-averaged alive of big instances) / demand
 *   starvation     = small instance whose time-averaged quota is < 1 particle
 */
#include <stdio.h>
#include <string.h>

#define G 256     /* global particle cap */
#define MAXN 64   /* max simultaneous effect instances */
#define FRAMES 120

typedef struct {
    int demand;          /* desired steady-state occupancy */
    int life;            /* particle lifetime in frames */
    int quota;           /* slots granted this frame */
    int alive;           /* current occupancy */
    int age[G];          /* per-particle age, slots [0, alive) in use */
    long asum;           /* alive summed over frames (drop metric) */
    long qsum;           /* quota summed over frames (starvation metric) */
} Inst;

static float BETA = 0.5f; /* fraction of G reserved for per-instance floors */

static void inst_init(Inst* e, int demand, int life) {
    memset(e, 0, sizeof(*e));
    e->demand = demand;
    e->life = life;
}

/* q_i as derived in the header comment; never exceeds the global cap. */
static void alloc(Inst* ins, int n) {
    int r = (int)(BETA * G / n);
    int rem = G - r * n;
    long sumu = 0;
    for (int i = 0; i < n; i++) {
        int u = ins[i].demand - ins[i].alive;
        if (u > 0) sumu += u;
    }
    for (int i = 0; i < n; i++) {
        int u = ins[i].demand - ins[i].alive;
        if (u < 0) u = 0;
        int e = sumu > 0 ? (int)((long long)rem * u / sumu) : 0;
        ins[i].quota = r + e;
    }
}

static void frame(Inst* ins, int n) {
    for (int i = 0; i < n; i++) {
        Inst* e = &ins[i];
        int w = 0;
        for (int j = 0; j < e->alive; j++) {
            e->age[j]++;
            if (e->age[j] < e->life) e->age[w++] = e->age[j];
        }
        e->alive = w;
    }
    alloc(ins, n);
    for (int i = 0; i < n; i++) {
        Inst* e = &ins[i];
        e->qsum += e->quota;
        if (e->alive > e->quota) e->alive = e->quota; /* cull newest to fit quota */
        int want = e->demand - e->alive;
        for (int s = 0; s < want; s++) {
            if (e->alive < e->quota) e->age[e->alive++] = 0;
            else break;
        }
        e->asum += e->alive;
    }
}

typedef struct {
    double big_drop;            /* 1 - avg alive / demand, big instances */
    double starve;              /* fraction of small instances starved */
    int starved[MAXN];          /* ids of starved small instances */
    int nstarved;
    int cap_violations;         /* frames with sum(q) > G or alive > quota */
} Run;

/* one big effect (demand 8x equal share) per three small ones (1/4x) */
static Run sim(int n, float beta) {
    BETA = beta;
    static Inst ins[MAXN];
    int eq = G / n;
    for (int i = 0; i < n; i++) {
        int d = (i % 4 == 0) ? 8 * eq : eq / 4;
        if (d < 1) d = 1;
        inst_init(&ins[i], d, 30);
    }
    Run r; memset(&r, 0, sizeof(r));
    for (int f = 0; f < FRAMES; f++) {
        frame(ins, n);
        long qs = 0;
        for (int i = 0; i < n; i++) {
            qs += ins[i].quota;
            if (ins[i].alive > ins[i].quota) r.cap_violations++;
        }
        if (qs > G) r.cap_violations++;
    }
    long ba = 0, bd = 0;
    int nsmall = 0;
    for (int i = 0; i < n; i++) {
        if (i % 4 == 0) { ba += ins[i].asum; bd += ins[i].demand; }
        else {
            nsmall++;
            /* starved: time-averaged granted quota below one particle */
            if (ins[i].qsum < FRAMES) r.starved[r.nstarved++] = i;
        }
    }
    r.big_drop = bd > 0 ? 1.0 - (double)ba / ((double)FRAMES * bd) : 0.0;
    r.starve = nsmall > 0 ? (double)r.nstarved / nsmall : 0.0;
    return r;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static void print_ids(const Run* r) {
    if (r->nstarved == 0) { printf("none\n"); return; }
    for (int i = 0; i < r->nstarved; i++) printf("%s%d", i ? "," : "", r->starved[i]);
    printf("\n");
}

int main(void) {
    /* isolation: adding instance B must not clear instance A's particles */
    static Inst solo[1], pair[2];
    BETA = 0.5f;
    inst_init(&solo[0], 10, 1000);
    inst_init(&pair[0], 10, 1000);
    inst_init(&pair[1], 200, 1000);
    for (int f = 0; f < 5; f++) { frame(solo, 1); frame(pair, 2); }
    int same = pair[0].alive == 10 && solo[0].alive == pair[0].alive;
    for (int j = 0; j < pair[0].alive && same; j++)
        same = solo[0].age[j] == pair[0].age[j];
    check("adding_instance_keeps_particles", same);
    check("new_instance_runs", pair[1].alive > 0);

    /* cap proof, exhaustive over n with pseudo-random demand and occupancy */
    float betas[] = { 0.0f, 0.5f, 1.0f };
    unsigned rng = 12345;
    int cap_ok = 1, nonneg = 1;
    static Inst pins[MAXN];
    for (int b = 0; b < 3; b++) {
        BETA = betas[b];
        for (int n = 1; n <= MAXN; n++) {
            for (int t = 0; t < 50; t++) {
                for (int i = 0; i < n; i++) {
                    rng = rng * 1664525u + 1013904223u;
                    inst_init(&pins[i], (int)(rng >> 16) % 513, 30);
                    rng = rng * 1664525u + 1013904223u;
                    pins[i].alive = (int)(rng >> 16) % 200;
                }
                alloc(pins, n);
                long qs = 0;
                for (int i = 0; i < n; i++) {
                    qs += pins[i].quota;
                    if (pins[i].quota < 0) nonneg = 0;
                }
                if (qs > G) cap_ok = 0;
            }
        }
    }
    check("quota_sum_within_cap", cap_ok);
    check("quota_nonnegative", nonneg);

    /* tradeoff curves: big-effect drop rate vs small-effect starvation */
    float sweep[] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
    Run ondemand; memset(&ondemand, 0, sizeof(ondemand));
    for (int b = 0; b < 6; b++) {
        Run r = sim(MAXN, sweep[b]);
        printf("curve beta=%.2f big_drop=%.3f small_starve=%.3f\n",
               sweep[b], r.big_drop, r.starve);
        if (b == 0) ondemand = r;
    }
    check("pure_ondemand_starves", ondemand.starve > 0.01);
    printf("ondemand_starved_ids=");
    print_ids(&ondemand);

    /* chosen strategy BETA=0.5 at three instance-count tiers */
    int tiers[] = { 4, 16, 64 };
    int inv_ok = 1;
    for (int t = 0; t < 3; t++) {
        Run r = sim(tiers[t], 0.5f);
        printf("tier n=%d big_drop=%.3f starve=%.4f\n",
               tiers[t], r.big_drop, r.starve);
        char name[32];
        snprintf(name, sizeof(name), "tier%d_starve_le_1pct", tiers[t]);
        check(name, r.starve <= 0.01);
        if (r.nstarved > 0) { printf("starved_ids="); print_ids(&r); }
        if (r.cap_violations > 0) inv_ok = 0;
    }
    check("occupancy_and_sum_within_cap", inv_ok);

    Run half = sim(MAXN, 0.5f);
    Run equal = sim(MAXN, 1.0f);
    check("floor_beats_equal_split_for_big", half.big_drop < equal.big_drop);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
