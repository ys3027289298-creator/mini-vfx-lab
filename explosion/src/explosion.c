
/* explosion: burst emitter with capacity drops, selectable lifetime
 * distributions and per-frame snapshots for decay analysis.
 *
 * Two run modes:
 *   ./explosion                       -> built-in self tests
 *   ./explosion <mode> <outdir>       -> CSV simulation run
 *       mode: fixed | uniform | exp
 *
 * Reproduces the reported defect first: an 800-particle burst against a
 * 200-slot pool with one constant lifetime per particle means the visible
 * population collapses to capacity instantly and every survivor expires on
 * the same frame (a cliff). The exp/uniform modes sample a lifetime per
 * particle instead, so deaths spread into a smooth decay curve. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- scenario constants (shared by every mode, only lifetimes differ) ---- */
#define DT          0.0333333f   /* 30 fps */
#define NFRAMES     375          /* 12.5 s, long enough for capped 12 s lives */
#define CAPACITY    200          /* particle-pool slots, mirrors emitter MAXP */
#define BURST0      800          /* initial explosion burst requested */
#define BURST_EARLY 30           /* tail emission per frame ... */
#define BURST_FRAMES 6           /* ... for the first six frames after frame 0 */
#define NBATCH      (1 + BURST_FRAMES)
#define SPEED_MIN   20.0f
#define SPEED_MAX   80.0f
#define DRAG        0.4f
#define GY         -20.0f
/* lifetime parameters */
#define LIFE_FIXED  2.0f
#define LIFE_LO     0.8f
#define LIFE_HI     3.0f
#define EXP_MEAN    1.5f
#define LIFE_CAP    12.0f        /* truncation cap for the exponential tail */
#define SEED        20261125u

typedef struct {
    int id;
    int batch;
    float x, y, vx, vy;
    float age, life;
    int alive;
} P;

/* birth ledger: spawn-time attributes of every admitted particle, kept apart
 * from the live pool so slot reuse never overwrites a particle's history */
typedef struct { int id, batch; float life, vx, vy; } LedgerEntry;

#define MAX_EMIT 1024

typedef struct {
    P p[CAPACITY];
    int n;                 /* slots ever used */
    long emitted;          /* spawn requests accepted */
    long dropped;          /* spawn requests rejected for lack of a slot */
    unsigned int rng;
    int next_id;
    const char* mode;      /* lifetime distribution: fixed | uniform | exp */
    LedgerEntry ledger[MAX_EMIT];
    int nledger;
} Sim;

static float u01(Sim* s) {
    /* xorshift32 -> uniform in [1/2^32, 1) */
    unsigned int x = s->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->rng = x;
    return ((float)x + 0.5f) / 4294967296.0f;
}

/* lifetime samplers: the whole defect lives in this one choice */
static float sample_life(Sim* s) {
    const char* mode = s->mode;
    if (strcmp(mode, "fixed") == 0) return LIFE_FIXED;
    if (strcmp(mode, "uniform") == 0)
        return LIFE_LO + (LIFE_HI - LIFE_LO) * u01(s);
    /* truncated exponential: -mean * ln(u), clipped at LIFE_CAP */
    float u = u01(s);
    if (u < 1e-7f) u = 1e-7f;
    float life = -EXP_MEAN * logf(u);
    if (life > LIFE_CAP) life = LIFE_CAP;
    return life;
}

static int spawn(Sim* s, int batch) {
    for (int i = 0; i < s->n; i++) {
        if (!s->p[i].alive) {
            P* q = &s->p[i];
            float ang = 2.0f * 3.14159265f * u01(s);
            float spd = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * u01(s);
            float vx = spd * cosf(ang), vy = spd * sinf(ang);
            float life = sample_life(s);
            int id = s->next_id++;
            *q = (P){ id, batch, 0, 0, vx, vy, 0.0f, life, 1 };
            if (s->nledger < MAX_EMIT)
                s->ledger[s->nledger++] = (LedgerEntry){ id, batch, life, vx, vy };
            s->emitted++;
            return 0;
        }
    }
    if (s->n >= CAPACITY) { s->dropped++; return -1; }
    P* q = &s->p[s->n++];
    float ang = 2.0f * 3.14159265f * u01(s);
    float spd = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * u01(s);
    float vx = spd * cosf(ang), vy = spd * sinf(ang);
    float life = sample_life(s);
    int id = s->next_id++;
    *q = (P){ id, batch, 0, 0, vx, vy, 0.0f, life, 1 };
    if (s->nledger < MAX_EMIT)
        s->ledger[s->nledger++] = (LedgerEntry){ id, batch, life, vx, vy };
    s->emitted++;
    return 0;
}

/* brightness fades linearly with remaining life; 0 once expired */
static float brightness(const P* q) {
    if (!q->alive) return 0.0f;
    float b = 1.0f - q->age / q->life;
    return b > 0.0f ? b : 0.0f;
}

static void advance(Sim* s) {
    float k = expf(-DRAG * DT);
    for (int i = 0; i < s->n; i++) {
        P* q = &s->p[i];
        if (!q->alive) continue;
        q->age += DT;
        /* half-frame tolerance: age accumulation at 30 fps never lands exactly
         * on a lifetime boundary, so expire on the frame whose midpoint covers
         * it instead of letting float rounding kill particles a frame early */
        if (q->age >= q->life - 0.5f * DT) { q->alive = 0; continue; }
        q->vx *= k; q->vy *= k;
        q->vy += GY * DT;
        q->x += q->vx * DT;
        q->y += q->vy * DT;
    }
}

static int alive_count(const Sim* s) {
    int c = 0;
    for (int i = 0; i < s->n; i++) c += s->p[i].alive;
    return c;
}

static void write_particles(Sim* s, FILE* f) {
    fputs("id,batch,life,vx,vy\n", f);
    for (int i = 0; i < s->nledger; i++) {
        const LedgerEntry* q = &s->ledger[i];
        fprintf(f, "%d,%d,%.6f,%.4f,%.4f\n", q->id, q->batch, q->life, q->vx, q->vy);
    }
}

static void run_csv(const char* mode, const char* outdir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/particles.csv", outdir);
    FILE* fp = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/frames.csv", outdir);
    FILE* ff = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/snaps.csv", outdir);
    FILE* fs = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/batches.csv", outdir);
    FILE* fb = fopen(path, "w");
    if (!fp || !ff || !fs || !fb) { perror("open output"); exit(2); }

    fputs("frame,t,alive,spawned,dropped,mean_brightness\n", ff);
    fputs("frame,id,batch,x,y,brightness\n", fs);
    fputs("frame,batch,alive,mean_brightness\n", fb);

    Sim s;
    memset(&s, 0, sizeof(s));
    s.rng = SEED;
    s.mode = mode;

    for (int frame = 0; frame < NFRAMES; frame++) {
        long emitted_before = s.emitted;
        if (frame == 0) {
            for (int i = 0; i < BURST0; i++) spawn(&s, 0);
        } else if (frame <= BURST_FRAMES) {
            for (int i = 0; i < BURST_EARLY; i++) spawn(&s, frame);
        }
        long spawned = s.emitted - emitted_before;

        int alive = alive_count(&s);
        double bsum = 0.0;
        for (int i = 0; i < s.n; i++) if (s.p[i].alive) bsum += brightness(&s.p[i]);
        fprintf(ff, "%d,%.6f,%d,%ld,%ld,%.6f\n", frame, frame * DT, alive,
                spawned, s.dropped, alive ? bsum / alive : 0.0);

        for (int i = 0; i < s.n; i++) {
            const P* q = &s.p[i];
            if (q->alive)
                fprintf(fs, "%d,%d,%d,%.4f,%.4f,%.6f\n", frame, q->id, q->batch,
                        q->x, q->y, brightness(q));
        }
        for (int b = 0; b < NBATCH; b++) {
            int bc = 0; double bbs = 0.0;
            for (int i = 0; i < s.n; i++)
                if (s.p[i].alive && s.p[i].batch == b) { bc++; bbs += brightness(&s.p[i]); }
            fprintf(fb, "%d,%d,%d,%.6f\n", frame, b, bc, bc ? bbs / bc : 0.0);
        }

        if (frame == NFRAMES - 1) write_particles(&s, fp);
        advance(&s);
    }
    fclose(fp); fclose(ff); fclose(fs); fclose(fb);
}

/* ------------------------------- self tests ------------------------------- */
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    if (argc == 3) {
        if (strcmp(argv[1], "fixed") && strcmp(argv[1], "uniform")
            && strcmp(argv[1], "exp")) {
            fprintf(stderr, "unknown mode: %s\n", argv[1]);
            return 2;
        }
        run_csv(argv[1], argv[2]);
        return 0;
    }

    Sim s;
    memset(&s, 0, sizeof(s));
    s.rng = SEED;

    /* fixed lifetime: every draw is exactly the constant, zero spread */
    s.mode = "fixed";
    int all_const = 1;
    for (int i = 0; i < 1000; i++)
        if (sample_life(&s) != LIFE_FIXED) all_const = 0;
    check("fixed_life_constant", all_const);

    /* uniform draws stay inside [lo, hi] and actually spread */
    s.mode = "uniform";
    float umin = 1e9f, umax = -1e9f, usum = 0.0f;
    int n = 50000;
    for (int i = 0; i < n; i++) {
        float v = sample_life(&s);
        if (v < umin) umin = v;
        if (v > umax) umax = v;
        usum += v;
    }
    float umean = usum / n;
    check("uniform_in_range", umin >= LIFE_LO && umax <= LIFE_HI);
    check("uniform_spreads", (umax - umin) > 0.5f * (LIFE_HI - LIFE_LO));
    check("uniform_mean", fabsf(umean - 0.5f * (LIFE_LO + LIFE_HI)) < 0.05f);

    /* exponential draws: positive, capped, mean close to EXP_MEAN */
    s.mode = "exp";
    float emin = 1e9f, emax = -1e9f, esum = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = sample_life(&s);
        if (v < emin) emin = v;
        if (v > emax) emax = v;
        esum += v;
    }
    float emean = esum / n;
    check("exp_positive_capped", emin > 0.0f && emax <= LIFE_CAP);
    check("exp_mean", fabsf(emean - EXP_MEAN) < 0.08f);
    /* exponential is right-skewed: median below the mean */
    float* vals = malloc((size_t)n * sizeof(float));
    s.rng = SEED ^ 0x9e3779b9u;
    for (int i = 0; i < n; i++) vals[i] = sample_life(&s);
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (vals[j] < vals[i]) { float t = vals[i]; vals[i] = vals[j]; vals[j] = t; }
    float emed = vals[n / 2];
    check("exp_right_skewed", emed < emean);
    free(vals);

    /* capacity: burst of 800 into a 200-slot pool admits 200, drops 600 */
    Sim c;
    memset(&c, 0, sizeof(c));
    c.rng = SEED;
    c.mode = "fixed";
    for (int i = 0; i < BURST0; i++) spawn(&c, 0);
    check("capacity_admits_200", c.n == CAPACITY && alive_count(&c) == 200);
    check("capacity_drops_600", c.dropped == BURST0 - CAPACITY);

    /* fixed lifetime cliff: 59 steps in all 200 are still alive, step 60 (age
     * reaches 2.0 s) kills the whole batch on one frame */
    int steps = (int)lroundf(LIFE_FIXED / DT);
    for (int f = 0; f < steps - 1; f++) advance(&c);
    int before = alive_count(&c);
    advance(&c);
    int after = alive_count(&c);
    check("fixed_cliff_dies_together", before == 200 && after == 0);

    /* exponential mode never loses half of its peak population in one frame.
     * Frames with fewer than 10 survivors are excluded: at the tail a single
     * particle's death is mathematically a 100% step but not a visible cliff,
     * matching the >=10-population guard used in the Python analysis. */
    Sim e;
    memset(&e, 0, sizeof(e));
    e.rng = SEED;
    e.mode = "exp";
    int peak = 0, cliff = 0;
    float worst_ratio = 0.0f;
    int prev = 0;
    for (int frame = 0; frame < NFRAMES; frame++) {
        if (frame == 0) for (int i = 0; i < BURST0; i++) spawn(&e, 0);
        else if (frame <= BURST_FRAMES)
            for (int i = 0; i < BURST_EARLY; i++) spawn(&e, frame);
        int a = alive_count(&e);
        if (a > peak) peak = a;
        if (prev >= 10) {
            float ratio = (float)(prev - a) / (float)prev;
            if (ratio > worst_ratio) worst_ratio = ratio;
            if ((prev - a) * 2 >= prev) cliff = 1;
        }
        prev = a;
        advance(&e);
    }
    check("exp_no_half_frame_cliff", !cliff && worst_ratio < 0.5f);
    check("exp_peak_near_capacity", peak >= 190 && peak <= CAPACITY);
    check("exp_worst_frame_drop_under_20pct", worst_ratio < 0.20f);

    /* uniform mode must be smooth as well; its worst step is larger (uniform
     * lifetimes pile up near the far edge) but still nowhere near a cliff */
    Sim u;
    memset(&u, 0, sizeof(u));
    u.rng = SEED;
    u.mode = "uniform";
    cliff = 0; prev = 0;
    for (int frame = 0; frame < NFRAMES; frame++) {
        if (frame == 0) for (int i = 0; i < BURST0; i++) spawn(&u, 0);
        else if (frame <= BURST_FRAMES)
            for (int i = 0; i < BURST_EARLY; i++) spawn(&u, frame);
        int a = alive_count(&u);
        if (prev >= 10 && (prev - a) * 2 >= prev) cliff = 1;
        prev = a;
        advance(&u);
    }
    check("uniform_no_half_frame_cliff", !cliff);

    /* determinism: two seeds produce the same first draw */
    Sim d1, d2;
    memset(&d1, 0, sizeof(d1)); memset(&d2, 0, sizeof(d2));
    d1.rng = 42u; d2.rng = 42u;
    d1.mode = "exp"; d2.mode = "exp";
    check("rng_deterministic",
          fabsf(sample_life(&d1) - sample_life(&d2)) < 1e-6f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
