
/* emitter: rate + burst emission with lifetime expiry and capacity. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAXP 64
#define CURVE_N 41
#define MAX_DIAG_IDS MAXP
#define MAX_ID_LOG 128

typedef struct {
    float x, y, vx, vy;
    float age, life;
    int alive;
    int id;
    float born;
} P;

typedef struct {
    P p[MAXP];
    int n;
    float rate_acc;
    int emitted;
    int dropped;

    /* Diagnostics and shutdown state. */
    int next_id;
    float time;
    float last_dt;
    int active;
    float rate;
    int burst_pending;
    float spawn_life;
    float off_time;

    /* Complete append-only log of births after off_time. */
    int rogue_log_count;
    int rogue_log_truncated;
    int rogue_log_ids[MAX_ID_LOG];
} Emitter;

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int em_alive_count(const Emitter* e) {
    int alive = 0;
    for (int i = 0; i < e->n; i++)
        if (e->p[i].alive) alive++;
    return alive;
}

static void em_init(Emitter* e) {
    memset(e, 0, sizeof(*e));
    e->next_id = 1;
    e->active = 1;
    e->spawn_life = 1.0f;
    e->off_time = -1.0f;
}

static int em_spawn(Emitter* e, float x, float y, float vx, float vy, float life) {
    if (life <= 0.0f) return -1;

    int id = e->next_id++;
    float born = e->time;

    if (e->off_time >= 0.0f && born > e->off_time + 1e-6f) {
        if (e->rogue_log_count < MAX_ID_LOG)
            e->rogue_log_ids[e->rogue_log_count++] = id;
        else
            e->rogue_log_truncated = 1;
    }

    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) {
            e->p[i] = (P){
                .x = x, .y = y, .vx = vx, .vy = vy,
                .age = 0.0f, .life = life, .alive = 1,
                .id = id, .born = born
            };
            e->emitted++;
            return i;
        }
    }
    if (e->n >= MAXP) { e->dropped++; return -2; }
    e->p[e->n] = (P){
        .x = x, .y = y, .vx = vx, .vy = vy,
        .age = 0.0f, .life = life, .alive = 1,
        .id = id, .born = born
    };
    e->emitted++;
    return e->n++;
}

static int em_queue_burst(Emitter* e, int burst) {
    if (burst < 0) return -1;
    e->burst_pending += burst;
    return e->burst_pending;
}

/*
 * First half of shutdown: make the source term identically zero.
 * active gates both rate and burst arguments, while the stored copies and
 * fractional accumulator are cleared so stale state cannot spawn later.
 */
static void em_stop_emission(Emitter* e) {
    e->active = 0;
    e->rate = 0.0f;
    e->rate_acc = 0.0f;
    e->burst_pending = 0;
}

/*
 * horizon == 0 clears stock immediately.
 * horizon  > 0 caps every remaining lifetime to at most horizon seconds.
 * horizon >= longest life is equivalent to natural expiry.
 */
static int em_shutdown(Emitter* e, float horizon) {
    if (horizon < 0.0f) return -1;

    em_stop_emission(e);
    e->off_time = e->time;

    int removed_now = 0;
    for (int i = 0; i < e->n; i++) {
        P* p = &e->p[i];
        if (!p->alive) continue;

        if (horizon == 0.0f) {
            p->alive = 0;
            removed_now++;
        } else {
            float remaining = p->life - p->age;
            if (remaining > horizon)
                p->life = p->age + horizon;
        }
    }
    return removed_now;
}

static int em_update(Emitter* e, float dt, float rate, int burst) {
    if (dt < 0.0f) return -1;

    e->last_dt = dt;
    e->time += dt;

    /* 先推进已有粒子，再发射新粒子，避免新粒子在本帧就被计满寿命 */
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        e->p[i].age += dt;
        /* Small epsilon absorbs float32 accumulation at a frame boundary. */
        if (e->p[i].age + 1e-5f >= e->p[i].life) {
            e->p[i].alive = 0;
            continue;
        }
        e->p[i].x += e->p[i].vx * dt;
        e->p[i].y += e->p[i].vy * dt;
    }

    if (e->active) {
        e->rate = rate;

        int queued = burst + e->burst_pending;
        e->burst_pending = 0;
        for (int b = 0; b < queued; b++)
            em_spawn(e, 0, 0, 0, 1.0f, e->spawn_life);

        e->rate_acc += rate * dt;
        int want = (int)e->rate_acc;
        e->rate_acc -= (float)want;
        for (int i = 0; i < want; i++)
            em_spawn(e, 0, 0, 0, 1.0f, e->spawn_life);
    } else {
        /* Defensive: a closed emitter has no latent spawn state. */
        e->rate = 0.0f;
        e->rate_acc = 0.0f;
        e->burst_pending = 0;
    }

    return em_alive_count(e);
}

/* Longest full lifetime among current stock; spawn_life is the source bound. */
static float em_stock_max_life(const Emitter* e) {
    float lmax = 0.0f;
    for (int i = 0; i < e->n; i++) {
        if (e->p[i].alive && e->p[i].life > lmax)
            lmax = e->p[i].life;
    }
    if (lmax <= 0.0f) lmax = e->spawn_life;
    return lmax;
}

static int em_collect_rogue_alive(const Emitter* e, float off_time,
                                  int* ids, int cap) {
    int n = 0;
    for (int i = 0; i < e->n && n < cap; i++) {
        const P* p = &e->p[i];
        if (p->alive && p->born > off_time + 1e-4f)
            ids[n++] = p->id;
    }
    return n;
}

static int cmp_int(const void* a, const void* b) {
    int x = *(const int*)a;
    int y = *(const int*)b;
    return (x > y) - (x < y);
}

typedef struct {
    float horizon;
    float clear_time;
    float abruptness;
} CurvePoint;

/*
 * Sweep shutdown horizons on disposable copies.
 * abruptness(h) = largest one-frame death/removal fraction of N_off.
 * clear_time(h)  = simulated seconds until the copied emitter is empty.
 */
static int em_plan_horizon(const Emitter* base, float dt, float abrupt_budget,
                           CurvePoint* curve, int points) {
    int n_off = em_alive_count(base);
    float lmax = em_stock_max_life(base);
    int best = -1;

    for (int j = 0; j < points; j++) {
        float h = lmax * (float)j / (float)(points - 1);
        Emitter trial = *base;

        int max_drop = em_shutdown(&trial, h);
        int guard = 0;

        while (em_alive_count(&trial) > 0 && guard < 100000) {
            int before = em_alive_count(&trial);
            em_update(&trial, dt, 0.0f, 0);
            int after = em_alive_count(&trial);
            int drop = before - after;
            if (drop > max_drop) max_drop = drop;
            guard++;
        }

        float elapsed = (float)guard * dt;
        curve[j].horizon = h;
        curve[j].clear_time = elapsed;
        curve[j].abruptness = n_off > 0 ? (float)max_drop / (float)n_off : 0.0f;

        if (curve[j].abruptness <= abrupt_budget + 1e-6f &&
            curve[j].clear_time <= lmax + 1e-4f) {
            if (best < 0 || curve[j].clear_time < curve[best].clear_time)
                best = j;
        }
    }
    return best;
}

#define LAB_DT 0.05f
#define LAB_RATE 20.0f
#define LAB_LIFE 2.0f
#define ABRUPT_BUDGET 0.25f

typedef struct {
    const char* label;
    int off_steps;
    int leak_steps;

    float off_time;
    int n_off;
    float lmax;

    CurvePoint curve[CURVE_N];
    int best_index;
    float best_horizon;

    int emitted_at_off;
    int emissions_after_off;

    int cleared;
    int clear_step;
    float clear_time;
    int bound_ok;

    int violation_seen;
    int rogue_count;
    int rogue_ids[MAX_DIAG_IDS];

    int secret_count;
    int secret_truncated;
    int secret_ids[MAX_ID_LOG];
} RunReport;

static RunReport run_tier(const char* label, int off_steps, int leak_steps) {
    RunReport r;
    memset(&r, 0, sizeof(r));
    r.label = label;
    r.off_steps = off_steps;
    r.leak_steps = leak_steps;
    r.best_index = -1;
    r.clear_time = -1.0f;

    Emitter e;
    em_init(&e);
    e.spawn_life = LAB_LIFE;

    for (int s = 0; s < off_steps; s++)
        em_update(&e, LAB_DT, LAB_RATE, 0);

    r.off_time = e.time;
    r.n_off = em_alive_count(&e);
    r.lmax = LAB_LIFE;
    r.emitted_at_off = e.emitted;

    r.best_index = em_plan_horizon(&e, LAB_DT, ABRUPT_BUDGET,
                                   r.curve, CURVE_N);
    if (r.best_index >= 0)
        r.best_horizon = r.curve[r.best_index].horizon;

    if (leak_steps == 0) {
        /* The real two-step close: zero sources, then cap/clear stock. */
        em_shutdown(&e, r.best_horizon);
    } else {
        /*
         * Fault injection: caller believes it closed the emitter, but the
         * active/rate source terms were not zeroed.  Only rate_acc is reset,
         * which is the incomplete fix that leaves secret emission running.
         */
        e.active = 1;
        e.rate = LAB_RATE;
        e.rate_acc = 0.0f;
        e.off_time = e.time;
    }

    /* Leak window plus more than enough time for every LAB_LIFE particle. */
    int max_steps = leak_steps + (int)(LAB_LIFE / LAB_DT) + 80;
    for (int step = 1; step <= max_steps; step++) {
        int leaking = leak_steps > 0 && step <= leak_steps;

        if (leak_steps > 0 && step == leak_steps + 1)
            em_stop_emission(&e);

        em_update(&e, LAB_DT, leaking ? LAB_RATE : 0.0f, 0);
        float elapsed = (float)step * LAB_DT;
        int alive = em_alive_count(&e);

        if (!r.violation_seen && elapsed > r.lmax + 1e-4f && alive > 0) {
            r.violation_seen = 1;
            r.rogue_count = em_collect_rogue_alive(
                &e, r.off_time, r.rogue_ids, MAX_DIAG_IDS);
            qsort(r.rogue_ids, (size_t)r.rogue_count,
                  sizeof(r.rogue_ids[0]), cmp_int);
        }

        if (alive == 0) {
            r.cleared = 1;
            r.clear_step = step;
            r.clear_time = elapsed;
            break;
        }
    }

    r.emissions_after_off = e.emitted - r.emitted_at_off;
    r.secret_count = e.rogue_log_count;
    r.secret_truncated = e.rogue_log_truncated;
    memcpy(r.secret_ids, e.rogue_log_ids,
           (size_t)e.rogue_log_count * sizeof(r.secret_ids[0]));
    r.bound_ok = r.cleared && r.clear_time <= r.lmax + 1e-6f;
    return r;
}

static void print_ids(const int* ids, int n) {
    if (n == 0) {
        printf("none");
        return;
    }
    for (int i = 0; i < n; i++) {
        if (i) putchar(',');
        printf("%d", ids[i]);
    }
}

static int lab_main(int argc, char** argv) {
    int csv = argc >= 2 && strcmp(argv[1], "--curves") == 0;

    const int off_steps[3] = { 10, 24, 60 };
    const char* labels[3] = { "early", "middle", "late" };
    RunReport good[3];

    if (csv)
        printf("tier,horizon_s,clear_s,abruptness\n");

    for (int t = 0; t < 3; t++) {
        good[t] = run_tier(labels[t], off_steps[t], 0);

        if (csv) {
            for (int j = 0; j < CURVE_N; j++) {
                printf("%s,%.2f,%.2f,%.4f\n",
                       labels[t],
                       good[t].curve[j].horizon,
                       good[t].curve[j].clear_time,
                       good[t].curve[j].abruptness);
            }
            continue;
        }

        printf("\n== shutdown tier: %s (t_off=%.2fs, N_off=%d, Lmax=%.1fs) ==\n",
               good[t].label, good[t].off_time, good[t].n_off, good[t].lmax);
        printf("curve h/clear/abrupt at 0.25s grid:\n");
        for (int j = 0; j < CURVE_N; j += 5) {
            const CurvePoint* c = &good[t].curve[j];
            printf("  h=%.2fs D=%.2fs A=%.3f%s\n",
                   c->horizon, c->clear_time, c->abruptness,
                   j == good[t].best_index ? "  <- selected" : "");
        }
        if (good[t].best_index % 5 != 0) {
            const CurvePoint* c = &good[t].curve[good[t].best_index];
            printf("  h=%.2fs D=%.2fs A=%.3f  <- selected\n",
                   c->horizon, c->clear_time, c->abruptness);
        }
        printf("run: h*=%.2fs clear=%.2fs bound=%.2fs post_emissions=%d status=%s\n",
               good[t].best_horizon, good[t].clear_time, good[t].lmax,
               good[t].emissions_after_off,
               good[t].bound_ok ? "PASS" : "FAIL");
    }

    if (csv) return 0;

    printf("\n== negative control: middle tier with incomplete close ==\n");
    RunReport leak = run_tier("leak", 24, 50);
    printf("observed_clear=%.2fs, upper_bound=%.2fs, post_emissions=%d\n",
           leak.clear_time, leak.lmax, leak.emissions_after_off);
    printf("first bound violation at >%.2fs; alive IDs then: ",
           leak.lmax);
    print_ids(leak.rogue_ids, leak.rogue_count);
    printf("\n");
    printf("all post-off spawn IDs (%d%s): ",
           leak.secret_count, leak.secret_truncated ? ", truncated" : "");
    print_ids(leak.secret_ids, leak.secret_count);
    printf("\n");
    printf("detector=%s\n",
           leak.violation_seen && leak.rogue_count > 0 && leak.secret_count > 0
               ? "FOUND_IDS" : "MISSED");

    int ok = 1;
    for (int t = 0; t < 3; t++) ok &= good[t].bound_ok;
    ok &= leak.violation_seen && leak.rogue_count > 0 &&
          leak.secret_count == leak.emissions_after_off &&
          leak.clear_time > leak.lmax;
    printf("\nlab_all_ok=%d\n", ok);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && (strcmp(argv[1], "--lab") == 0 ||
                       strcmp(argv[1], "--curves") == 0))
        return lab_main(argc, argv);

    Emitter e; em_init(&e);
    check("spawn_ok", em_spawn(&e, 1, 2, 0, 0, 1.0f) == 0);
    check("zero_life_rejected", em_spawn(&e, 0, 0, 0, 0, 0.0f) == -1);
    check("spawn_id_starts_at_one", e.p[0].id == 1);

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

    Emitter off; em_init(&off);
    check("queued_burst_stored", em_queue_burst(&off, 2) == 2);
    em_shutdown(&off, 1.0f);
    check("shutdown_zero_sources",
          off.active == 0 && off.rate == 0.0f &&
          off.rate_acc == 0.0f && off.burst_pending == 0);

    Emitter now; em_init(&now);
    for (int i = 0; i < 8; i++) em_spawn(&now, 0, 0, 0, 0, 2.0f);
    int removed = em_shutdown(&now, 0.0f);
    check("immediate_clear", removed == 8 && em_alive_count(&now) == 0);

    RunReport early = run_tier("early", 10, 0);
    RunReport middle = run_tier("middle", 24, 0);
    RunReport late = run_tier("late", 60, 0);

    check("shutdown_early_bound",
          early.bound_ok && early.emissions_after_off == 0);
    check("shutdown_middle_bound",
          middle.bound_ok && middle.emissions_after_off == 0);
    check("shutdown_late_bound",
          late.bound_ok && late.emissions_after_off == 0);

    check("shutdown_early_abrupt_budget",
          early.best_index >= 0 &&
          early.curve[early.best_index].abruptness <= ABRUPT_BUDGET + 1e-6f);
    check("shutdown_middle_abrupt_budget",
          middle.best_index >= 0 &&
          middle.curve[middle.best_index].abruptness <= ABRUPT_BUDGET + 1e-6f);
    check("shutdown_late_abrupt_budget",
          late.best_index >= 0 &&
          late.curve[late.best_index].abruptness <= ABRUPT_BUDGET + 1e-6f);

    RunReport leak = run_tier("leak", 24, 50);
    check("leak_bound_exceeded",
          leak.violation_seen && !leak.bound_ok && leak.clear_time > leak.lmax);
    check("leak_reports_rogue_ids", leak.rogue_count > 0);
    check("leak_logs_every_post_off_id",
          leak.secret_count == 50 && !leak.secret_truncated &&
          leak.secret_ids[0] == 25 && leak.secret_ids[49] == 74);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
