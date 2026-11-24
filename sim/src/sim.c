
/* sim: frame-loop composition of emitter + field + trail.
 *
 * Time-unit chain (see audit/dim_audit.py):
 *   frame clock (ms tick) --MS_TO_SEC--> dt_s --stage scales--> integrator
 *
 * Every module contract states dt is in SECONDS (m/s gravity, s^-1 drag,
 * age/life in s). The frame clock emits milliseconds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The three modules expose static helpers; include them in this translation
 * unit with file-scope identifiers renamed to avoid collisions. */
#define Part         FieldPart
#define step         field_step
#define step_verlet  field_step_verlet
#define speed        field_speed
#define check        field_check
#define failures     field_failures
#define main         field_main
#include "../../field/src/field.c"
#undef main
#undef failures
#undef check
#undef speed
#undef step_verlet
#undef step
#undef Part

#define P             EmP
#define Emitter       EmEmitter
#define em_init       emm_init
#define em_spawn      emm_spawn
#define em_update     emm_update
#define check         em_check
#define failures      em_failures
#define main          em_main
#include "../../emitter/src/emitter.c"
#undef main
#undef failures
#undef check
#undef em_update
#undef em_spawn
#undef em_init
#undef Emitter
#undef P

#define TP            TrTP
#define Trail         TrTrail
#define tr_push       trr_push
#define tr_age        trr_age
#define tr_width      trr_width
#define check         tr_check
#define failures      tr_failures
#define main          tr_main
#include "../../trail/src/trail.c"
#undef main
#undef failures
#undef check
#undef tr_width
#undef tr_age
#undef tr_push
#undef Trail
#undef TP

/* Frame clock: platform timer ticks in milliseconds; every module contract
 * is in seconds, so one tick is 1e-3 s. A wrong factor here rescales every
 * downstream force (g=9.8 m/s^2 looked like 0.98 with 1e-4). The audit's
 * clock sweep re-derives this constant from the 4.9 m free-fall contract. */
#ifndef SIM_MS_TO_SEC
#define SIM_MS_TO_SEC 0.001f
#endif

/* Per-stage dt knobs. 1.0 = module contract honored; used by the audit to
 * localize which stage breaks the seconds contract. */
static float env_scale(const char* name) {
    const char* v = getenv(name);
    return v ? strtof(v, NULL) : 1.0f;
}

static float clock_dt_s(int fps) {
    float frame_period_ms = 1000.0f / (float)fps;
    /* runtime override lets the audit sweep the conversion constant without
     * rebuilding; the compile-time macro is the shipped default */
    const char* override = getenv("SIM_MS_TO_SEC");
    float ms_to_sec = override ? strtof(override, NULL) : SIM_MS_TO_SEC;
    return frame_period_ms * ms_to_sec;
}

static void usage(void) {
    fprintf(stderr,
        "usage: sim <cmd> [args]\n"
        "  fall <fps> <frames> <g> <euler|verlet>\n"
        "  probe-clock <fps> <frames>\n"
        "  probe-emitter\n"
        "  probe-trail\n"
        "  probe-field <euler|verlet>\n");
}

/* End-to-end free fall: one particle seeded at (0,0), zero velocity,
 * measured after `frames` frames. Runs through every stage in the chain. */
static int cmd_fall(int argc, char** argv) {
    if (argc < 6) { usage(); return 2; }
    int fps = atoi(argv[2]);
    int frames = atoi(argv[3]);
    float g = strtof(argv[4], NULL);
    int use_verlet = strcmp(argv[5], "verlet") == 0;
    float dt0 = clock_dt_s(fps);

    float s_em = env_scale("SIM_DT_SCALE_EMITTER");
    float s_fd = env_scale("SIM_DT_SCALE_FIELD");
    float s_tr = env_scale("SIM_DT_SCALE_TRAIL");

    EmEmitter e; emm_init(&e);
    TrTrail t; memset(&t, 0, sizeof(t));
    emm_spawn(&e, 0.0f, 0.0f, 0.0f, 0.0f, 10.0f);

    float sim_time = 0.0f;
    for (int i = 0; i < frames; i++) {
        float dt = dt0;
        /* emitter advances age/life with its own seconds-contract dt;
         * zero its velocity so its constant-velocity position move does
         * not double-count the field integration below. */
        float saved_vy = e.p[0].vy, saved_vx = e.p[0].vx;
        e.p[0].vx = e.p[0].vy = 0.0f;
        emm_update(&e, dt * s_em, 0.0f, 0);
        e.p[0].vx = saved_vx; e.p[0].vy = saved_vy;

        FieldPart fp = { e.p[0].x, e.p[0].y, e.p[0].vx, e.p[0].vy };
        if (use_verlet) field_step_verlet(&fp, dt * s_fd, 0.0f, -g, 0.0f, 0.0f);
        else            field_step(&fp, dt * s_fd, 0.0f, -g, 0.0f, 0.0f);
        e.p[0].x = fp.x; e.p[0].y = fp.y; e.p[0].vx = fp.vx; e.p[0].vy = fp.vy;

        trr_push(&t, fp.x, fp.y);
        trr_age(&t, dt * s_tr, 100.0f);
        sim_time += dt;
    }
    float y = e.p[0].y;
    float analytic = 0.5f * g * sim_time * sim_time;
    float relerr = fabsf((-y) - analytic) / analytic;
    printf("fps=%d\n", fps);
    printf("frames=%d\n", frames);
    printf("g=%g\n", (double)g);
    printf("frame_period_ms=%g\n", 1000.0 / fps);
    printf("clock_dt_s=%g\n", (double)dt0);
    printf("sim_time_s=%g\n", (double)sim_time);
    printf("y_m=%g\n", (double)y);
    printf("displacement_m=%g\n", (double)(-y));
    printf("analytic_4p9_m=%g\n", (double)(0.5f * g * 1.0f * 1.0f));
    printf("relerr_vs_4p9=%g\n", (double)(fabsf((-y) - 4.9f) / 4.9f));
    printf("integrator=%s\n", use_verlet ? "verlet" : "euler");
    printf("alive=%d\n", e.p[0].alive);
    printf("trail_points=%d\n", t.n);
    (void)analytic; (void)relerr;
    return 0;
}

/* Stage 1 contract: 60 frames at 60fps must total exactly 1.0 second. */
static int cmd_probe_clock(int argc, char** argv) {
    if (argc < 4) { usage(); return 2; }
    int fps = atoi(argv[2]);
    int frames = atoi(argv[3]);
    float total = 0.0f;
    for (int i = 0; i < frames; i++) total += clock_dt_s(fps);
    printf("stage=clock\n");
    printf("total_sim_time_s=%g\n", (double)total);
    printf("expected_s=%g\n", (double)frames / fps);
    printf("relerr=%g\n", (double)fabsf(total - (float)frames / fps) / ((float)frames / fps));
    return 0;
}

/* Stage 2 contract: rate=10/s over dt=1.0s emits 10; age advances by dt.
 * life must outlast the probe dt or expiry zeroes the observed particle. */
static int cmd_probe_emitter(void) {
    EmEmitter e; emm_init(&e);
    emm_spawn(&e, 0.0f, 0.0f, 0.0f, 1.0f, 10.0f);
    emm_update(&e, 1.0f, 10.0f, 0);
    float age = e.p[0].age;
    float relerr = fabsf(age - 1.0f);
    printf("stage=emitter\n");
    printf("age_s=%g\n", (double)age);
    printf("expected_s=1\n");
    printf("rate10_1s_emitted=%d\n", e.emitted - 1);
    printf("alive=%d\n", e.p[0].alive);
    printf("relerr=%g\n", (double)relerr);
    return 0;
}

/* Stage 4 contract: aging a trail point by dt=0.5s reports age 0.5s. */
static int cmd_probe_trail(void) {
    TrTrail t; memset(&t, 0, sizeof(t));
    trr_push(&t, 0.0f, 0.0f);
    trr_age(&t, 0.5f, 100.0f);
    float age = t.p[0].age;
    printf("stage=trail\n");
    printf("age_s=%g\n", (double)age);
    printf("expected_s=0.5\n");
    printf("relerr=%g\n", (double)(fabsf(age - 0.5f) / 0.5f));
    return 0;
}

/* Stage 3 contract, isolated: g=9.8, dt=1/60, 60 steps -> 4.9 m. */
static int cmd_probe_field(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    int use_verlet = strcmp(argv[2], "verlet") == 0;
    FieldPart p = { 0, 0, 0, 0 };
    for (int i = 0; i < 60; i++) {
        if (use_verlet) field_step_verlet(&p, 1.0f / 60.0f, 0, -9.8f, 0, 0);
        else            field_step(&p, 1.0f / 60.0f, 0, -9.8f, 0, 0);
    }
    printf("stage=field\n");
    printf("integrator=%s\n", use_verlet ? "verlet" : "euler");
    printf("displacement_m=%g\n", (double)(-p.y));
    printf("expected_m=4.9\n");
    printf("relerr=%g\n", (double)(fabsf((-p.y) - 4.9f) / 4.9f));
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "fall") == 0) return cmd_fall(argc, argv);
    if (strcmp(argv[1], "probe-clock") == 0) return cmd_probe_clock(argc, argv);
    if (strcmp(argv[1], "probe-emitter") == 0) return cmd_probe_emitter();
    if (strcmp(argv[1], "probe-trail") == 0) return cmd_probe_trail();
    if (strcmp(argv[1], "probe-field") == 0) return cmd_probe_field(argc, argv);
    usage();
    return 2;
}
