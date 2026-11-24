
/* splash: ground-impact detection, secondary splash emission and a capped
 * shared particle pool. Modes (argv[1]):
 *   (none) / selftest        - unit checks
 *   diag_baseline <dt>       - no ground check (reproduces the bug)
 *   diag_fixed <dt>          - ground check + clamped impact point
 *   run <dt> <frames> <rate> <seed>
 *   stress <dt> <frames> <rate> <seed>
 *   single <dt> [seed]
 * Every simulation mode writes CSV logs under the path given by SPLASH_OUT
 * (default: ./): rain_trace.csv, splash_trace.csv, series.csv, density.csv.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define POOL_MAX   256   /* hard cap on live particles, rain + splash shared */
#define GROUND_Y   0.0f
#define SPLASH_PER_HIT 6
#define SPLASH_BUDGET   48    /* max splash particles alive at any instant */
#define RAIN_SPAWN_X 10.0f
#define RAIN_LIFE    4.0f
#define RAIN_VY     -18.0f
#define SPLASH_LIFE  0.9f
#define SPLASH_SPEED 5.0f
#define SPLASH_UP    3.5f
#define GRAVITY      9.8f
#define SPLASH_DRAG  1.2f

typedef struct {
    float x, y, vx, vy;
    float age, life;
    int   kind;   /* 0 = rain drop, 1 = splash particle */
    int   alive;
    int   fresh;  /* spawned this step: held until the next frame */
} P;

typedef struct {
    P p[POOL_MAX];
    int n;                /* high-water slot index in use */
    int emitted, dropped; /* lifetime spawn attempts */
    int splash_emitted, splash_dropped;
    int splash_alive;
    int rain_dropped;
    int impact_events;
    int collision;        /* ground-impact detection enabled */
    int splash_chain;     /* splash hits spawn further splash (stress only) */
} Pool;

static unsigned rng_state;
static float rngf(void) {                 /* deterministic LCG, [0,1) */
    rng_state = rng_state * 1103515245u + 12345u;
    return (float)((rng_state >> 8) & 0xffffff) / (float)0x1000000;
}

static void pool_init(Pool* s) { memset(s, 0, sizeof(*s)); }

/* returns slot index, -1 bad args, -2 pool full / budget exceeded */
static int spawn(Pool* s, int kind, float x, float y, float vx, float vy,
                 float life) {
    if (life <= 0.0f) return -1;
    if (kind == 1 && s->splash_alive >= SPLASH_BUDGET) {
        s->dropped++; s->splash_dropped++; return -2;
    }
    for (int i = 0; i < s->n; i++) {
        if (!s->p[i].alive) {
            s->p[i] = (P){ x, y, vx, vy, 0.0f, life, kind, 1, 1 };
            s->emitted++;
            if (kind == 1) { s->splash_emitted++; s->splash_alive++; }
            return i;
        }
    }
    if (s->n >= POOL_MAX) {
        s->dropped++;
        if (kind == 1) s->splash_dropped++; else s->rain_dropped++;
        return -2;
    }
    s->p[s->n] = (P){ x, y, vx, vy, 0.0f, life, kind, 1, 1 };
    s->emitted++;
    if (kind == 1) { s->splash_emitted++; s->splash_alive++; }
    return s->n++;
}

static void kill_p(Pool* s, int i) {
    if (!s->p[i].alive) return;
    if (s->p[i].kind == 1) s->splash_alive--;
    s->p[i].alive = 0;
}

/* segment / ground-plane crossing: fraction of dt at which y hits GROUND_Y.
 * returns 1 and writes t in [0,1]; 0 when the segment stays above ground. */
static int cross_fraction(const P* p, float ny, float* t) {
    if (p->y <= GROUND_Y) { *t = 0.0f; return 1; }
    if (ny >= p->y || ny > GROUND_Y) return 0;
    *t = (p->y - GROUND_Y) / (p->y - ny);
    return 1;
}

/* emit one splash burst at (x, GROUND_Y); counts attempts and rejects */
static void emit_splash(Pool* s, float x) {
    s->impact_events++;
    for (int k = 0; k < SPLASH_PER_HIT; k++) {
        float ang = (float)M_PI * rngf();                 /* 0..pi, upward */
        float spd = SPLASH_SPEED * (0.6f + 0.8f * rngf());
        float vx = cosf(ang) * spd;
        float vy = fabsf(sinf(ang)) * spd + SPLASH_UP;
        spawn(s, 1, x, GROUND_Y + 0.02f, vx, vy, SPLASH_LIFE);
    }
}

typedef struct {
    FILE* rain;     /* per-frame positions of tracked drops + disappearance */
    FILE* splash;
    FILE* series;
    FILE* density;
    int   frame;
    int   traced;   /* number of rain drops fully traced so far */
} Logs;

static FILE* out_open(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    return f;
}

static void logs_open(Logs* L, const char* dir) {
    L->rain = out_open(dir, "rain_trace.csv");
    fprintf(L->rain, "frame,drop_id,kind,x,y,vy,event,height_above_ground\n");
    L->splash = out_open(dir, "splash_trace.csv");
    fprintf(L->splash, "frame,impact_id,x,y\n");
    L->series = out_open(dir, "series.csv");
    fprintf(L->series,
            "frame,alive,rain_alive,splash_alive,emitted,dropped,"
            "splash_emitted,splash_dropped,impact_events\n");
    L->density = out_open(dir, "density.csv");
    fprintf(L->density, "frame,kind,x,y,source_impact\n");
    L->frame = 0; L->traced = 0;
}

static void logs_close(Logs* L) {
    fclose(L->rain); fclose(L->splash); fclose(L->series);
    fclose(L->density);
}

static int alive_count(const Pool* s, int kind) {
    int c = 0;
    for (int i = 0; i < s->n; i++)
        if (s->p[i].alive && s->p[i].kind == kind) c++;
    return c;
}

/* one simulation step. dt is the frame step; physics is sub-stepped at
 * PHYS_H so fast drops never tunnel through the ground plane. */
#define PHYS_H 0.005f
static void step(Pool* s, float dt, Logs* L) {
    int tracked_at_entry[POOL_MAX];
    for (int i = 0; i < s->n; i++) {
        tracked_at_entry[i] = s->p[i].alive;
        s->p[i].fresh = 0;   /* particles existing at frame start integrate */
    }

    float h = dt;
    int nsub = 1;
    if (dt > PHYS_H) { nsub = (int)ceilf(dt / PHYS_H); h = dt / nsub; }

    for (int sub = 0; sub < nsub; sub++) {
        for (int i = 0; i < s->n; i++) {
            P* p = &s->p[i];
            if (!p->alive) continue;
            if (p->fresh) continue;   /* born this frame: hold until next */
            p->age += h;
            if (p->age >= p->life) {
                if (L && tracked_at_entry[i] && p->kind == 0 &&
                    L->traced < 12)
                    fprintf(L->rain,
                        "%d,%d,expire,%.4f,%.4f,%.4f,expired,%.4f\n",
                        L->frame, i, p->x, p->y, p->vy,
                        p->y - GROUND_Y);
                kill_p(s, i);
                continue;
            }
            float k = (p->kind == 1) ? expf(-SPLASH_DRAG * h) : 1.0f;
            float nvx = p->vx * k;
            float nvy = p->vy * k - GRAVITY * h;
            float nx = p->x + nvx * h;
            float ny = p->y + nvy * h;

            if (s->collision) {
                float t;
                if (cross_fraction(p, ny, &t)) {
                    float ix = p->x + (nx - p->x) * t;
                    if (L && tracked_at_entry[i] && p->kind == 0 &&
                        L->traced < 12)
                        fprintf(L->rain,
                            "%d,%d,impact,%.4f,%.4f,%.4f,ground_hit,%.4f\n",
                            L->frame, i, ix, GROUND_Y, nvy, 0.0f);
                    if (p->kind == 0 || s->splash_chain) {
                        if (p->kind == 0 && L) {
                            fprintf(L->splash, "%d,%d,%.4f,%.4f\n",
                                    L->frame, s->impact_events, ix, GROUND_Y);
                            L->traced++;
                        }
                        emit_splash(s, ix);
                    }
                    kill_p(s, i);
                    continue;
                }
            }
            p->vx = nvx; p->vy = nvy; p->x = nx; p->y = ny;
        }
    }

    if (L) {
        /* record surviving tracked rain drops for frame-by-frame tracing */
        for (int i = 0; i < s->n && L->traced < 12; i++) {
            P* p = &s->p[i];
            if (p->alive && p->kind == 0 && tracked_at_entry[i])
                fprintf(L->rain,
                    "%d,%d,track,%.4f,%.4f,%.4f,moving,%.4f\n",
                    L->frame, i, p->x, p->y, p->vy, p->y - GROUND_Y);
        }
        for (int i = 0; i < s->n; i++) {
            P* p = &s->p[i];
            if (p->alive)
                fprintf(L->density, "%d,%d,%.4f,%.4f,%d\n",
                        L->frame, p->kind, p->x, p->y,
                        p->kind == 1 ? s->impact_events - 1 : -1);
        }
        int ra = alive_count(s, 0), sa = alive_count(s, 1);
        fprintf(L->series, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n", L->frame,
                ra + sa, ra, sa, s->emitted, s->dropped,
                s->splash_emitted, s->splash_dropped, s->impact_events);
        L->frame++;
    }
}

static void spawn_rain(Pool* s, int count) {
    for (int i = 0; i < count; i++) {
        float x = (rngf() - 0.5f) * 2.0f * RAIN_SPAWN_X;
        spawn(s, 0, x, 16.0f + rngf() * 2.0f, 0.0f,
              RAIN_VY * (0.85f + 0.3f * rngf()), RAIN_LIFE);
    }
}

static void run_sim(const char* outdir, int collision, int chain,
                    float dt, int frames, float rate, unsigned seed) {
    Pool s; pool_init(&s);
    s.collision = collision;
    s.splash_chain = chain;
    Logs L; logs_open(&L, outdir);
    rng_state = seed ? seed : 1u;
    float acc = 0.0f;
    for (int f = 0; f < frames; f++) {
        acc += rate * dt;
        int want = (int)acc; acc -= (float)want;
        spawn_rain(&s, want);
        step(&s, dt, &L);
    }
    logs_close(&L);
    printf("emitted=%d dropped=%d splash_emitted=%d splash_dropped=%d "
           "impacts=%d maxpool=%d splash_budget=%d\n",
           s.emitted, s.dropped, s.splash_emitted, s.splash_dropped,
           s.impact_events, POOL_MAX, SPLASH_BUDGET);
}

/* n independent raindrops, each followed until its splash burst fully dies:
 * clean, non-overlapping input for radial-density analysis. */
static void run_single(const char* outdir, float dt, unsigned seed,
                       int impacts) {
    Pool s; pool_init(&s);
    s.collision = 1;
    Logs L; logs_open(&L, outdir);
    rng_state = seed ? seed : 1u;
    if (impacts < 1) impacts = 1;
    for (int k = 0; k < impacts; k++) {
        spawn(&s, 0, 0.0f, 10.0f, 0.5f, RAIN_VY, RAIN_LIFE);
        for (int f = 0; f < 400; f++) {
            step(&s, dt, &L);
            int any = 0;
            for (int i = 0; i < s.n; i++)
                if (s.p[i].alive) { any = 1; break; }
            if (!any) break;
        }
    }
    logs_close(&L);
    printf("single: impacts=%d splash_emitted=%d splash_dropped=%d\n",
           s.impact_events, s.splash_emitted, s.splash_dropped);
}

/* ---------------- self tests ---------------- */
static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "selftest") != 0) {
        const char* dir = getenv("SPLASH_OUT");
        if (!dir) dir = ".";
        if (strcmp(argv[1], "diag_baseline") == 0 && argc >= 3) {
            run_sim(dir, 0, 0, atof(argv[2]), 160, 14.0f, 42u);
            return 0;
        }
        if (strcmp(argv[1], "diag_fixed") == 0 && argc >= 3) {
            run_sim(dir, 1, 0, atof(argv[2]), 160, 14.0f, 42u);
            return 0;
        }
        if (strcmp(argv[1], "run") == 0 && argc >= 6) {
            run_sim(dir, 1, 0, atof(argv[2]), atoi(argv[3]),
                    atof(argv[4]), (unsigned)atoi(argv[5]));
            return 0;
        }
        if (strcmp(argv[1], "stress") == 0 && argc >= 6) {
            run_sim(dir, 1, 1, atof(argv[2]), atoi(argv[3]),
                    atof(argv[4]), (unsigned)atoi(argv[5]));
            return 0;
        }
        if (strcmp(argv[1], "single") == 0 && argc >= 3) {
            run_single(dir, atof(argv[2]),
                       argc >= 4 ? (unsigned)atoi(argv[3]) : 42u,
                       argc >= 5 ? atoi(argv[4]) : 1);
            return 0;
        }
        fprintf(stderr,
            "usage: %s [selftest | diag_baseline dt | diag_fixed dt | "
            "run dt frames rate seed | stress dt frames rate seed | "
            "single dt [seed]]\n",
            argv[0]);
        return 2;
    }

    rng_state = 7u;

    /* crossing fraction: drop moving from y=2.0 toward below ground */
    P q = { 0, 2.0f, 0, -10.0f, 0, 1, 0, 1, 0 };
    float t = -1.0f;
    check("cross_detect", cross_fraction(&q, -1.0f, &t) == 1);
    check("cross_fraction_ok", fabsf(t - 2.0f / 3.0f) < 1e-5f);
    P above = { 0, 5.0f, 0, -1.0f, 0, 1, 0, 1, 0 };
    check("no_cross_when_above", cross_fraction(&above, 4.5f, &t) == 0);

    /* a raindrop starting just above the ground must impact, not tunnel */
    Pool s; pool_init(&s);
    s.collision = 1;
    rng_state = 7u;
    int slot = spawn(&s, 0, 3.0f, 0.5f, 1.0f, -20.0f, RAIN_LIFE);
    check("spawn_rain", slot >= 0);
    int before = s.impact_events;
    step(&s, 0.1f, NULL);            /* sub-stepped at 5 ms internally */
    check("impact_registered", s.impact_events == before + 1);
    check("rain_killed_on_impact", s.p[slot].alive == 0);
    check("splash_burst_count", s.splash_emitted == SPLASH_PER_HIT);
    float ix = 3.0f + 1.0f * 0.025f;   /* crossing inside the first 5 ms sub-step */
    int near = 0;
    for (int i = 0; i < s.n; i++)
        if (s.p[i].alive && s.p[i].kind == 1 &&
            fabsf(s.p[i].x - ix) < 0.02f &&
            s.p[i].y >= GROUND_Y && s.p[i].y <= GROUND_Y + 0.05f)
            near++;
    check("splash_born_at_impact", near == SPLASH_PER_HIT);

    /* splash budget: fill it, next burst must be rejected and counted */
    Pool b; pool_init(&b);
    rng_state = 1u;
    for (int i = 0; i < SPLASH_BUDGET; i++)
        spawn(&b, 1, 0, 0.1f, 0, 1, SPLASH_LIFE);
    check("splash_budget_filled", b.splash_alive == SPLASH_BUDGET);
    int rc = spawn(&b, 1, 0, 0.1f, 0, 1, SPLASH_LIFE);
    check("splash_over_budget_rejected", rc == -2);
    check("splash_drop_counted", b.splash_dropped == 1 && b.dropped == 1);

    /* whole pool cap: splash budget still allows rain until POOL_MAX */
    Pool c; pool_init(&c);
    int drops = 0, rejects = 0;
    for (int i = 0; i < POOL_MAX + 10; i++) {
        if (spawn(&c, 0, 0, 5.0f, 0, -1.0f, 10.0f) >= 0) drops++;
        else rejects++;
    }
    check("pool_capped", drops == POOL_MAX && rejects == 10);
    check("rain_drop_counted", c.rain_dropped == 10 && c.dropped == 10);

    /* dead slots are recycled, so a steady sim cannot overflow the pool */
    Pool d; pool_init(&d);
    spawn(&d, 0, 0, 1.0f, 0, 0, 0.1f);      /* dies on first step */
    step(&d, 0.2f, NULL);
    int reuse = spawn(&d, 0, 0, 1.0f, 0, 0, 1.0f);
    check("slot_recycled", reuse == 0);

    /* splash particles die at the ground without a chain by default */
    Pool e; pool_init(&e);
    e.collision = 1;
    rng_state = 3u;
    spawn(&e, 1, 0, 0.3f, 2.0f, -8.0f, SPLASH_LIFE);
    step(&e, 0.1f, NULL);
    check("splash_ground_death", e.impact_events == 0 &&
          e.p[0].alive == 0);

    /* with a chain enabled, a splash ground hit emits its own burst */
    Pool f; pool_init(&f);
    f.collision = 1; f.splash_chain = 1;
    rng_state = 3u;
    spawn(&f, 1, 0, 0.3f, 2.0f, -8.0f, SPLASH_LIFE);  /* counts as 1 */
    int emitted_before = f.splash_emitted;
    step(&f, 0.1f, NULL);
    check("splash_chain_burst", f.impact_events == 1 &&
          f.splash_emitted - emitted_before == SPLASH_PER_HIT);

    check("zero_life_rejected", spawn(&e, 1, 0, 0, 0, 0, 0.0f) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
