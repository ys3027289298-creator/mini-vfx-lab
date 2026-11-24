
/* emitter: rate + burst emission with lifetime expiry and capacity. */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAXP 64
#define EM_DT (1.0f / 60.0f)
#define EM_LIFE 1.0f
#define EM_ONSET 0.75f
#define EM_GAMMA 2.5f
#define EM_TARGET_Z (7.0f / 8.0f)
#define EM_RUN_FRAMES 1200
#define EM_TAIL_FRAMES 240
#define EM_SETTLE_TOL 3.0f

typedef struct { float x, y, vx, vy; float age, life; int alive; } P;
typedef struct { P p[MAXP]; int n; float rate_acc; int emitted; int dropped; } Emitter;

static void em_init(Emitter* e) { memset(e, 0, sizeof(*e)); }

static int em_alive(const Emitter* e) {
    int alive = 0;
    for (int i = 0; i < e->n; i++) if (e->p[i].alive) alive++;
    return alive;
}

static float em_clamp01(float x) { return fmaxf(0.0f, fminf(1.0f, x)); }

/* u=0 at onset occupancy, u=1 at the hard capacity. */
static float em_approach(int alive, int cap, float onset) {
    float z = (float)alive / (float)cap;
    return em_clamp01((z - onset) / (1.0f - onset));
}

/* Retained nominal emission: ((1-z)/(1-onset))^gamma in the degrade band. */
static float em_rate_scale(int alive, int cap, float onset, float gamma) {
    float u = em_approach(alive, cap, onset);
    return powf(1.0f - u, gamma);
}

/* Fraction of nominal emission removed by smooth degradation. */
static float em_reduction(int alive, int cap, float onset, float gamma) {
    return 1.0f - em_rate_scale(alive, cap, onset, gamma);
}

/* Solve z = rho * retention(z) for the continuous steady occupancy. */
static float em_steady_z(float rho, float gamma, float onset) {
    if (rho <= onset) return rho;
    float lo = onset, hi = 1.0f;
    for (int i = 0; i < 80; i++) {
        float z = 0.5f * (lo + hi);
        float scale = powf((1.0f - z) / (1.0f - onset), gamma);
        if (z < rho * scale) lo = z;
        else hi = z;
    }
    return 0.5f * (lo + hi);
}

/* Invert the steady equation at a chosen design occupancy z_target. */
static float em_required_gamma(float rho_max, float z_target, float onset) {
    float q = (1.0f - z_target) / (1.0f - onset);
    return logf(z_target / rho_max) / logf(q);
}

static int em_spawn(Emitter* e, float x, float y, float vx, float vy, float life) {
    if (life <= 0.0f) return -1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) {
            e->p[i] = (P){ x, y, vx, vy, 0.0f, life, 1 };
            e->emitted++;
            return i;
        }
    }
    if (e->n >= MAXP) { e->dropped++; return -2; }
    e->p[e->n] = (P){ x, y, vx, vy, 0.0f, life, 1 };
    e->emitted++;
    return e->n++;
}

static int em_update_ex(Emitter* e, float dt, float nominal_rate, int burst,
                        float onset, float gamma, float* used_scale) {
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

    /* Adapt after ageing/burst so this frame's rate sees the live occupancy. */
    int alive = em_alive(e);
    float scale = em_rate_scale(alive, MAXP, onset, gamma);
    if (used_scale) *used_scale = scale;

    e->rate_acc += nominal_rate * scale * dt;
    int want = (int)e->rate_acc;
    e->rate_acc -= (float)want;
    for (int i = 0; i < want; i++) em_spawn(e, 0, 0, 0, 1.0f, 1.0f);
    return em_alive(e);
}

static int em_update(Emitter* e, float dt, float rate, int burst) {
    return em_update_ex(e, dt, rate, burst, EM_ONSET, EM_GAMMA, NULL);
}

/* Age first, then adapt rate to the post-expiry occupancy before spawning. */
static int em_update_adaptive(Emitter* e, float dt, float nominal_rate,
                              float onset, float gamma, float* used_scale) {
    return em_update_ex(e, dt, nominal_rate, 0, onset, gamma, used_scale);
}

typedef struct {
    int start_frame;
    int settle_frame;
    int settle_elapsed;
    int peak;
    int drops;
    float tail_mean;
} AdaptiveRun;

static AdaptiveRun em_run_adaptive_lab(float nominal_rate) {
    AdaptiveRun report = { -1, EM_RUN_FRAMES, -1, 0, 0, 0.0f };
    Emitter e;
    em_init(&e);
    int hist[EM_RUN_FRAMES];

    for (int f = 0; f < EM_RUN_FRAMES; f++) {
        float used_scale = 1.0f;
        int alive = em_update_adaptive(&e, EM_DT, nominal_rate,
                                      EM_ONSET, EM_GAMMA, &used_scale);
        hist[f] = alive;
        if (report.start_frame < 0 && used_scale < 1.0f - 1e-6f)
            report.start_frame = f;
        if (alive > report.peak) report.peak = alive;
    }
    report.drops = e.dropped;

    float sum = 0.0f;
    for (int f = EM_RUN_FRAMES - EM_TAIL_FRAMES; f < EM_RUN_FRAMES; f++)
        sum += (float)hist[f];
    report.tail_mean = sum / (float)EM_TAIL_FRAMES;

    int last_bad = report.start_frame - 1;
    for (int f = report.start_frame; f < EM_RUN_FRAMES; f++) {
        if (fabsf((float)hist[f] - report.tail_mean) > EM_SETTLE_TOL)
            last_bad = f;
    }
    report.settle_frame = last_bad + 1;
    report.settle_elapsed = report.settle_frame - report.start_frame;
    return report;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
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

    check("degrade_onset_zero_cut",
          fabsf(em_reduction(48, MAXP, EM_ONSET, EM_GAMMA)) < 1e-6f);
    check("degrade_cap_full_cut",
          fabsf(em_rate_scale(MAXP, MAXP, EM_ONSET, EM_GAMMA)) < 1e-6f);
    float s49 = em_rate_scale(49, MAXP, EM_ONSET, EM_GAMMA);
    float s56 = em_rate_scale(56, MAXP, EM_ONSET, EM_GAMMA);
    check("degrade_monotonic", s49 > s56 && s56 > 0.0f);

    float gamma_req = em_required_gamma(4.0f, EM_TARGET_Z, EM_ONSET);
    check("inverse_gamma", fabsf(gamma_req - 2.192645f) < 1e-4f);
    check("inverse_steady_budget",
          fabsf(em_steady_z(4.0f, gamma_req, EM_ONSET) * (float)MAXP - 56.0f) < 0.01f);

    AdaptiveRun r64 = em_run_adaptive_lab(64.0f);
    AdaptiveRun r128 = em_run_adaptive_lab(128.0f);
    AdaptiveRun r256 = em_run_adaptive_lab(256.0f);
    printf("adaptive gamma_required=%.3f gamma_used=%.1f onset=%d cap=%d\n",
           gamma_req, EM_GAMMA, (int)(EM_ONSET * MAXP), MAXP);
    printf("adaptive_run rate=64  start=%d settle=%d frames=%d peak=%d tail_mean=%.2f drops=%d\n",
           r64.start_frame, r64.settle_frame, r64.settle_elapsed,
           r64.peak, r64.tail_mean, r64.drops);
    printf("adaptive_run rate=128 start=%d settle=%d frames=%d peak=%d tail_mean=%.2f drops=%d\n",
           r128.start_frame, r128.settle_frame, r128.settle_elapsed,
           r128.peak, r128.tail_mean, r128.drops);
    printf("adaptive_run rate=256 start=%d settle=%d frames=%d peak=%d tail_mean=%.2f drops=%d\n",
           r256.start_frame, r256.settle_frame, r256.settle_elapsed,
           r256.peak, r256.tail_mean, r256.drops);

    check("run64_settled", r64.settle_frame < EM_RUN_FRAMES);
    check("run128_settled", r128.settle_frame < EM_RUN_FRAMES);
    check("run256_settled", r256.settle_frame < EM_RUN_FRAMES);
    check("run64_under_cap", r64.peak <= MAXP && r64.tail_mean <= MAXP && r64.drops == 0);
    check("run128_under_cap", r128.peak <= MAXP && r128.tail_mean <= MAXP && r128.drops == 0);
    check("run256_under_cap", r256.peak <= MAXP && r256.tail_mean <= MAXP && r256.drops == 0);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
