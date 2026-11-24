/*
 * pause: pause/resume timestamp handling for an emitter + motion + trail sim.
 *
 * The bug
 * -------
 * Wall timestamps keep advancing while the effect is paused.  If the frame
 * loop subtracts a *stale* "last simulated" timestamp on resume, the first
 * resume step receives dt = one normal frame + the whole pause interval.
 * That giant dt is fed to two independent consumers at once:
 *
 *   1. particle integration  -> every pre-pause particle teleports
 *      d_time ~= speed * pause_seconds            ("时间补偿")
 *   2. the emitter rate accumulator -> rate * pause_seconds particles are
 *      born in a single frame                     ("发射补发/倾泻")
 *
 * The fix keeps two separate ledgers:
 *
 *   * motion clock: wall base is refreshed on every tick (also while paused),
 *     so the resume integration dt is exactly one normal frame.  The pause
 *     interval is never integrated: no catch-up time is simulated.
 *   * emission ledger: the same pause interval is booked ONCE as an integer
 *     particle debt (fractional remainder retained), and repaid at a bounded
 *     per-frame cap.  The accumulated emission count is therefore not lost
 *     and is not dumped in one frame.
 *
 * Deterministic: the wall clock is a scripted double timeline, no sleeping.
 * The same scripted operation is replayed through five policy variants and
 * every trajectory is recorded as CSV under recordings/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAXP 512

#define HZ           60.0
#define DT_H         (1.0 / HZ)          /* normal frame step, seconds       */
#define PAUSE_SEC    1.0                 /* wall time the effect is paused   */
#define PRE_FRAMES   30                  /* running frames before pause       */
#define PAUSE_TICKS  60                  /* wall ticks emitted while paused   */
#define RESUME_FRAME (PRE_FRAMES + PAUSE_TICKS + 1) /* frame index 91        */
#define TAIL_FRAMES  120
#define LAST_FRAME   (RESUME_FRAME + TAIL_FRAMES)   /* 211                  */

#define RATE         60.0                /* particles / second -> 1 / frame  */
#define PAY_CAP      1                   /* max debt particles repaid/frame;
                                          * total spawns then <= 2x steady   */
#define N_COHORT     12                  /* tagged cohort present pre-pause  */
#define SPEED        2.0f                /* constant particle speed, units/s */
#define LIFE         100.0f

#define ACC_EPS      1e-9

typedef struct {
    int id;
    float x, y, vx, vy;
    float age, life;
    int alive;
} PP;

typedef struct {
    PP p[MAXP];
    int n;
    double rate;
    double acc;          /* sub-integer normal emission accrual             */
    int    debt;         /* whole owed "pause emission" particles            */
    double debt_frac;    /* fractional obligation, never rounded away        */
    int    debt_peak;    /* largest whole obligation booked at resume        */
    int    pay_cap;
    int    born;         /* rate/debt spawned (initial cohort excluded)      */
    int    dropped;
    int    spawned_frame;
    int    last_normal;
    int    last_paid;
    float  last_move_max; /* max same-particle displacement in this step */
} Sim;

/* Wall clock whose base is refreshed by every tick, including paused ones. */
typedef struct {
    double last_wall;
    int    paused;
    double pause_start;
} PClock;

static void pc_init(PClock* c, double t0) {
    c->last_wall = t0; c->paused = 0; c->pause_start = 0.0;
}
static double pc_tick(PClock* c, double now) {
    double dt = now - c->last_wall;
    c->last_wall = now;
    return dt;
}
static void pc_pause(PClock* c, double now) {
    if (!c->paused) { c->paused = 1; c->pause_start = now; }
    c->last_wall = now;
}
/* First poll after a resume. Because paused ticks kept refreshing last_wall,
 * sim_dt is just the gap from the last tick (one frame). The pause span is
 * reported separately for the emission debt ledger. */
static void pc_resume(PClock* c, double now, double* sim_dt, double* paused_span) {
    if (c->paused) {
        *paused_span = c->last_wall - c->pause_start;
        c->paused = 0;
    } else {
        *paused_span = 0.0;
    }
    *sim_dt = now - c->last_wall;
    c->last_wall = now;
}

static void sim_init(Sim* s, int pay_cap) {
    memset(s, 0, sizeof(*s));
    s->rate = RATE;
    s->pay_cap = pay_cap;
    /* Tagged cohort created directly: not counted as rate emissions. */
    for (int i = 0; i < N_COHORT; i++) {
        s->p[i] = (PP){ i, 0.0f, 0.0f, SPEED, 0.0f, 0.0f, LIFE, 1 };
    }
    s->n = N_COHORT;
}

static int sim_spawn(Sim* s, float x, float y, float vx, float vy) {
    int slot = -1;
    for (int i = 0; i < s->n; i++) {
        if (!s->p[i].alive) { slot = i; break; }
    }
    if (slot < 0) {
        if (s->n >= MAXP) { s->dropped++; return -2; }
        slot = s->n++;
    }
    int id = N_COHORT + s->born;
    s->p[slot] = (PP){ id, x, y, vx, vy, 0.0f, LIFE, 1 };
    s->born++;
    s->spawned_frame++;
    return slot;
}

enum { PH_OTHER = 0, PH_PRE = 1, PH_RESUME = 2 };

typedef struct {
    float pre_jump_max;     /* max same-particle step inside warm-up         */
    float boundary_jump;    /* same, on the resume step                      */
    float all_jump_max;     /* max over every integrated step                 */
} JumpMetrics;

static void note_move(JumpMetrics* m, float d, int phase) {
    if (d > m->all_jump_max) m->all_jump_max = d;
    if (phase == PH_PRE && d > m->pre_jump_max) m->pre_jump_max = d;
    if (phase == PH_RESUME && d > m->boundary_jump) m->boundary_jump = d;
}

/*
 * motion_dt  : seconds integrated into positions/ages (H when fixed)
 * emit_dt    : seconds fed to the normal rate accumulator
 * debt_span  : pause seconds to book into the emission debt ledger
 * Returns alive count, or -1 on a negative interval.
 */
static int sim_step(Sim* s, float motion_dt, double emit_dt, double debt_span,
                    int phase, JumpMetrics* m) {
    if (motion_dt < 0.0f || emit_dt < 0.0 || debt_span < 0.0) return -1;
    s->spawned_frame = 0;
    s->last_normal = 0;
    s->last_paid = 0;
    s->last_move_max = 0.0f;

    /* 1. integrate particles that already exist (spawn happens afterwards,
     *    so new births never get a full-size step on their birth frame). */
    for (int i = 0; i < s->n; i++) {
        PP* p = &s->p[i];
        if (!p->alive) continue;
        if (motion_dt > 0.0f) {
            float ox = p->x, oy = p->y;
            p->age += motion_dt;
            if (p->age >= p->life) { p->alive = 0; continue; }
            p->x += p->vx * motion_dt;
            p->y += p->vy * motion_dt;
            float d = hypotf(p->x - ox, p->y - oy);
            if (d > s->last_move_max) s->last_move_max = d;
            note_move(m, d, phase);
        }
    }

    /* 2. normal rate quota for genuinely simulated time. */
    s->acc += s->rate * emit_dt;
    int want = (int)floor(s->acc + ACC_EPS);
    s->acc -= want;
    for (int i = 0; i < want; i++) {
        if (sim_spawn(s, 0.0f, 0.0f, SPEED, 0.0f) >= 0) s->last_normal++;
    }

    /* 3. book the pause interval once as an emission count debt. */
    if (debt_span > 0.0) {
        s->debt_frac += s->rate * debt_span;
        int whole = (int)floor(s->debt_frac + ACC_EPS);
        s->debt_frac -= whole;
        s->debt += whole;
        int obligation = s->debt; /* before this frame's repayment */
        if (obligation > s->debt_peak) s->debt_peak = obligation;
    }

    /* 4. repay debt at a bounded per-frame cap: no one-frame flood. */
    int pay = s->debt < s->pay_cap ? s->debt : s->pay_cap;
    for (int i = 0; i < pay; i++) {
        if (sim_spawn(s, 0.0f, 0.0f, SPEED, 0.0f) >= 0) s->last_paid++;
    }
    s->debt -= s->last_paid;

    int alive = 0;
    for (int i = 0; i < s->n; i++) if (s->p[i].alive) alive++;
    return alive;
}

enum Mode { M_BUGGY_FULL = 0, M_TIME_ONLY, M_EMIT_ONLY, M_FIXED, M_SCHEDULED, M_N };

static const char* MODE_NAME[M_N] = {
    "buggy_full", "time_only", "emit_only", "fixed", "scheduled"
};

typedef struct {
    JumpMetrics jump;
    int resume_spawned;
    int max_frame_spawned;
    int born_end;
    int debt_end;
    int debt_peak;
    int dropped;
} ModeResult;

static void write_traj_header(FILE* f) {
    fputs("mode,frame,wall_t,id,x,y,vx,vy,age\n", f);
}
static void write_frame_header(FILE* f) {
    fputs("mode,frame,wall_t,stepped,motion_dt,emit_dt,debt_span,phase,"
          "spawned,normal_n,debt_paid,debt,alive,born,step_max_move\n", f);
}

/* Continuous trajectories are recorded for the tagged cohort on every frame;
 * full-population snapshots are written only at the pre-pause and resume
 * frames (needed for the centroid / emission-cohort decomposition). */
static void record_traj(FILE* f, const char* mode, int frame, double wall,
                        const Sim* s, int full_snapshot) {
    for (int i = 0; i < s->n; i++) {
        const PP* p = &s->p[i];
        if (!p->alive) continue;
        if (!full_snapshot && p->id >= N_COHORT) continue;
        fprintf(f, "%s,%d,%.6f,%d,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                mode, frame, wall, p->id, p->x, p->y, p->vx, p->vy, p->age);
    }
}

static void record_frame(FILE* f, const char* mode, int frame, double wall,
                         int stepped, float motion_dt, double emit_dt, double debt_span,
                         int phase, const Sim* s, int alive) {
    fprintf(f, "%s,%d,%.6f,%d,%.7f,%.7f,%.7f,%d,%d,%d,%d,%d,%d,%d,%.6f\n",
            mode, frame, wall, stepped, motion_dt, emit_dt, debt_span, phase,
            s->spawned_frame, s->last_normal, s->last_paid, s->debt, alive, s->born,
            s->last_move_max);
}

/* Resume-step policy for the four pause-aware variants.
 * raw = now(91H) - last simulated stamp(30H) = 61H = H + PAUSE_SEC. */
static void resume_policy(int mode, double* motion_dt, double* emit_dt, double* debt_span) {
    double raw = (RESUME_FRAME - PRE_FRAMES) * DT_H; /* 61H */
    switch (mode) {
        case M_BUGGY_FULL: /* stale timestamp feeds both consumers */
            *motion_dt = raw; *emit_dt = raw; *debt_span = 0.0;
            break;
        case M_TIME_ONLY:  /* attribution: keep time compensation, fix emission */
            *motion_dt = raw; *emit_dt = DT_H; *debt_span = 0.0;
            break;
        case M_EMIT_ONLY:  /* attribution: fix motion clock, keep raw flush */
            *motion_dt = DT_H; *emit_dt = raw; *debt_span = 0.0;
            break;
        case M_FIXED:      /* rebase clock + book capped debt */
        default:
            *motion_dt = DT_H; *emit_dt = DT_H; *debt_span = PAUSE_SEC;
            break;
    }
}

static ModeResult run_mode(int mode) {
    ModeResult r; memset(&r, 0, sizeof(r));
    r.jump.pre_jump_max = r.jump.boundary_jump = r.jump.all_jump_max = 0.0f;

    char path[256];
    snprintf(path, sizeof(path), "recordings/traj_%s.csv", MODE_NAME[mode]);
    FILE* tf = fopen(path, "w");
    snprintf(path, sizeof(path), "recordings/frames_%s.csv", MODE_NAME[mode]);
    FILE* ff = fopen(path, "w");
    if (!tf || !ff) {
        fprintf(stderr, "cannot open recordings/*.csv (run from pause/ dir)\n");
        exit(2);
    }
    write_traj_header(tf);
    write_frame_header(ff);

    Sim s; sim_init(&s, PAY_CAP);
    record_traj(tf, MODE_NAME[mode], 0, 0.0, &s, 1);

    int alive = N_COHORT;
    int max_spawn = 0;

    if (mode == M_SCHEDULED) {
        /* Counterfactual: no pause ever happened, every wall frame simulated. */
        for (int k = 1; k <= LAST_FRAME; k++) {
            int phase = (k <= PRE_FRAMES) ? PH_PRE
                       : (k == RESUME_FRAME) ? PH_RESUME : PH_OTHER;
            alive = sim_step(&s, (float)DT_H, DT_H, 0.0, phase, &r.jump);
            if (s.spawned_frame > max_spawn) max_spawn = s.spawned_frame;
            if (k == RESUME_FRAME) r.resume_spawned = s.spawned_frame;
            record_traj(tf, MODE_NAME[mode], k, k * DT_H, &s,
                        k == PRE_FRAMES || k == RESUME_FRAME);
            record_frame(ff, MODE_NAME[mode], k, k * DT_H, 1,
                         (float)DT_H, DT_H, 0.0, phase, &s, alive);
        }
    } else {
        /* warm-up running frames */
        for (int k = 1; k <= PRE_FRAMES; k++) {
            alive = sim_step(&s, (float)DT_H, DT_H, 0.0, PH_PRE, &r.jump);
            if (s.spawned_frame > max_spawn) max_spawn = s.spawned_frame;
            record_traj(tf, MODE_NAME[mode], k, k * DT_H, &s, k == PRE_FRAMES);
            record_frame(ff, MODE_NAME[mode], k, k * DT_H, 1,
                         (float)DT_H, DT_H, 0.0, PH_PRE, &s, alive);
        }
        /* wall ticks during pause: no sim step, timestamps keep moving */
        for (int k = PRE_FRAMES + 1; k < RESUME_FRAME; k++) {
            s.spawned_frame = s.last_normal = s.last_paid = 0;
            s.last_move_max = 0.0f;
            record_frame(ff, MODE_NAME[mode], k, k * DT_H, 0,
                         0.0f, 0.0, 0.0, PH_OTHER, &s, alive);
        }
        /* resume frame */
        double mot, emi, debt;
        resume_policy(mode, &mot, &emi, &debt);
        alive = sim_step(&s, (float)mot, emi, debt, PH_RESUME, &r.jump);
        if (s.spawned_frame > max_spawn) max_spawn = s.spawned_frame;
        r.resume_spawned = s.spawned_frame;
        record_traj(tf, MODE_NAME[mode], RESUME_FRAME, RESUME_FRAME * DT_H, &s, 1);
        record_frame(ff, MODE_NAME[mode], RESUME_FRAME, RESUME_FRAME * DT_H, 1,
                     (float)mot, emi, debt, PH_RESUME, &s, alive);
        /* tail */
        for (int k = RESUME_FRAME + 1; k <= LAST_FRAME; k++) {
            alive = sim_step(&s, (float)DT_H, DT_H, 0.0, PH_OTHER, &r.jump);
            if (s.spawned_frame > max_spawn) max_spawn = s.spawned_frame;
            record_traj(tf, MODE_NAME[mode], k, k * DT_H, &s, 0);
            record_frame(ff, MODE_NAME[mode], k, k * DT_H, 1,
                         (float)DT_H, DT_H, 0.0, PH_OTHER, &s, alive);
        }
    }

    fclose(tf); fclose(ff);
    r.max_frame_spawned = max_spawn;
    r.born_end = s.born;
    r.debt_end = s.debt;
    r.debt_peak = s.debt_peak;
    r.dropped = s.dropped;
    return r;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    mkdir("recordings", 0755); /* exists is fine */

    /* ---- PClock unit behaviour ---------------------------------------- */
    PClock clk; pc_init(&clk, 0.0);
    double d1 = pc_tick(&clk, DT_H);
    check("clock_running_dt", fabs(d1 - DT_H) < 1e-12);
    pc_pause(&clk, DT_H);
    for (int k = 2; k <= 61; k++) pc_tick(&clk, k * DT_H); /* 60 pause ticks */
    double sdt = -1.0, span = -1.0;
    pc_resume(&clk, 62.0 * DT_H, &sdt, &span);
    check("clock_resume_dt_one_frame", fabs(sdt - DT_H) < 1e-12);
    check("clock_pause_span_booked", fabs(span - PAUSE_SEC) < 1e-9);
    double d2 = pc_tick(&clk, 63.0 * DT_H);
    check("clock_after_resume_normal", fabs(d2 - DT_H) < 1e-12);

    /* ---- replay the same scripted operation through every variant ------ */
    ModeResult rr[M_N];
    for (int m = 0; m < M_N; m++) rr[m] = run_mode(m);

    const ModeResult* bug = &rr[M_BUGGY_FULL];
    const ModeResult* to  = &rr[M_TIME_ONLY];
    const ModeResult* eo  = &rr[M_EMIT_ONLY];
    const ModeResult* fix = &rr[M_FIXED];
    const ModeResult* sch = &rr[M_SCHEDULED];

    float j0 = bug->jump.pre_jump_max;
    float x_full = bug->jump.boundary_jump - j0;
    float x_time = to->jump.boundary_jump - j0;
    float x_emit = eo->jump.boundary_jump - j0;
    float x_inter = x_full - x_time - x_emit;

    printf("# measurement units: position in world units, dt in seconds\n");
    for (int m = 0; m < M_N; m++) {
        printf("m_%s_boundary_jump=%.6f\n", MODE_NAME[m], rr[m].jump.boundary_jump);
        printf("m_%s_max_jump=%.6f\n", MODE_NAME[m], rr[m].jump.all_jump_max);
        printf("m_%s_resume_spawned=%d\n", MODE_NAME[m], rr[m].resume_spawned);
        printf("m_%s_max_frame_spawned=%d\n", MODE_NAME[m], rr[m].max_frame_spawned);
        printf("m_%s_born_end=%d debt_end=%d debt_peak=%d\n",
               MODE_NAME[m], rr[m].born_end, rr[m].debt_end, rr[m].debt_peak);
    }
    printf("normal_frame_jump=%.6f (speed*H=%.6f)\n", j0, SPEED * (float)DT_H);
    printf("raw_resume_dt=%.6f = H %.6f + pause %.6f\n",
           (RESUME_FRAME - PRE_FRAMES) * DT_H, DT_H, PAUSE_SEC);
    printf("decomp_excess_total=%.6f time_part=%.6f emit_part=%.6f interaction=%.9f\n",
           x_full, x_time, x_emit, x_inter);
    if (x_full > 1e-6f) {
        printf("decomp_share_time_pct=%.2f emit_pct=%.2f\n",
               100.0 * x_time / x_full, 100.0 * x_emit / x_full);
    }
    printf("pause_gap_distance_G=%.6f (speed*pause)\n", SPEED * PAUSE_SEC);

    /* ---- baseline bug reproduced, with hard numbers ------------------- */
    check("normal_step_equals_speed_dt", fabsf(j0 - SPEED * (float)DT_H) < 1e-5f);
    check("baseline_teleport_gt_1u", bug->jump.boundary_jump > 1.0f);
    check("baseline_jump_equals_speed_rawdt",
          fabsf(bug->jump.boundary_jump - SPEED * (float)((RESUME_FRAME - PRE_FRAMES) * DT_H)) < 1e-4f);
    check("baseline_dumps_61_in_resume_frame", bug->resume_spawned == 61);

    /* ---- decomposition: time compensation owns the positional jump ----- */
    check("time_only_reproduces_full_jump",
          fabsf(to->jump.boundary_jump - bug->jump.boundary_jump) < 1e-5f);
    check("emit_only_has_no_extra_position_jump",
          eo->jump.boundary_jump <= j0 * (1.0f + 1e-3f) + 1e-6f);
    check("decomp_additive", fabsf(x_inter) < 1e-4f);

    /* ---- fixed: continuous trajectory --------------------------------- */
    check("fixed_boundary_jump_le_normal_frame",
          fix->jump.boundary_jump <= j0 * (1.0f + 1e-3f) + 1e-6f);
    check("fixed_max_jump_le_normal_frame",
          fix->jump.all_jump_max <= j0 * (1.0f + 1e-3f) + 1e-6f);
    check("fixed_boundary_equals_normal_step",
          fabsf(fix->jump.boundary_jump - j0) < 1e-5f);

    /* ---- fixed: emission count preserved, never dumped ---------------- */
    check("fixed_debt_booked_60", fix->debt_peak == 60);
    check("fixed_resume_spawn_cap_2", fix->resume_spawned == 1 + PAY_CAP);
    check("fixed_max_frame_spawn_le_2", fix->max_frame_spawned <= 1 + PAY_CAP);
    check("fixed_debt_fully_repaid", fix->debt_end == 0);
    check("fixed_emission_count_matches_schedule", fix->born_end == sch->born_end);
    check("no_capacity_drops", bug->dropped == 0 && fix->dropped == 0);

    /* ---- argument validation in the house style ----------------------- */
    Sim z; sim_init(&z, PAY_CAP);
    JumpMetrics zm = {0};
    check("bad_dt_rejected", sim_step(&z, -1.0f, 0.0, 0.0, PH_OTHER, &zm) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
