
/* sim: end-to-end particle run comparing the four boundary strategies.
 *
 * Same emitter, forces, dt and RNG seed for every mode — only the boundary
 * behaviour changes. Emission/lifetime follow emitter.c, force integration
 * follows field.c (exponential drag + gravity), boundaries come from
 * boundary.c. Outputs:
 *   - per-frame stats CSV on stdout (frame,time,alive,out_of_box,...)
 *   - <prefix>_final.csv : final-frame particles x,y,alpha
 *
 * usage: sim <none|kill|reflect|fade> <prefix>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BOUNDARY_NO_MAIN
#include "boundary.c"

#define MAXP   400
#define DT     0.02f
#define RUNT   10.0f
#define RATE   25.0f          /* particles per second */
#define LIFE   12.0f          /* longer than the run: lifetime cannot mask
                                 the boundary bug — only the boundary kills */

typedef struct {
    float x, y, vx, vy, age, life, alpha;
    int alive;
    unsigned id;
} SP;

static SP pool[MAXP];
static int n_used = 0;
static float rate_acc = 0.0f;
static unsigned emitted_total = 0, killed_by_boundary = 0,
                bounce_total = 0, dropped = 0;

/* xorshift32 — fixed seed, identical spawns across the four runs. */
static unsigned rng_state = 0x12345678u;
static float rng_f(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state >> 8) * (1.0f / 16777216.0f);   /* [0,1), 24-bit */
}

static void spawn(void) {
    int slot = -1;
    for (int i = 0; i < n_used; i++) if (!pool[i].alive) { slot = i; break; }
    if (slot < 0) {
        if (n_used >= MAXP) { dropped++; return; }
        slot = n_used++;
    }
    /* radial burst from the origin, speed 2.5..4, full 360° */
    float ang = rng_f() * 6.2831853f;
    float spd = 2.5f + rng_f() * 1.5f;
    SP* p = &pool[slot];
    p->x = 0.0f; p->y = 0.0f;
    p->vx = cosf(ang) * spd;
    p->vy = sinf(ang) * spd;
    p->age = 0.0f; p->life = LIFE; p->alpha = 1.0f;
    p->alive = 1; p->id = emitted_total++;
}

static int in_box(const SP* p, const Boundary* b) {
    return p->x >= b->xmin && p->x <= b->xmax &&
           p->y >= b->ymin && p->y <= b->ymax;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: sim <mode> <prefix>\n"); return 2; }
    BoundaryMode mode;
    if      (!strcmp(argv[1], "none"))    mode = BD_NONE;
    else if (!strcmp(argv[1], "kill"))    mode = BD_KILL;
    else if (!strcmp(argv[1], "reflect")) mode = BD_REFLECT;
    else if (!strcmp(argv[1], "fade"))    mode = BD_FADE;
    else { fprintf(stderr, "unknown mode %s\n", argv[1]); return 2; }

    Boundary b = { -4, 4, -4, 4, 1.0f, 0.7f, 0.25f };
    const float gx = 0.0f, gy = -2.0f, drag = 0.12f, swirl = 0.0f;
    const int frames = (int)(RUNT / DT);

    /* Fade needs alpha initialised before its first boundary pass. */
    for (int i = 0; i < MAXP; i++) pool[i].alpha = 1.0f;

    char logname[256];
    snprintf(logname, sizeof(logname), "%s_log.csv", argv[2]);
    FILE* log = fopen(logname, "w");
    fprintf(log, "frame,time,alive,out_of_box,out_share,killed_cum,"
                 "bounces_cum,max_abs_alpha_jump,max_x,max_y\n");

    /* previous-frame alpha per particle (-1 = not alive last frame), so the
     * flicker metric ignores the birth frame and only compares consecutive
     * frames of the same living particle */
    float* prev_alpha = malloc(MAXP * sizeof(float));
    for (int i = 0; i < MAXP; i++) prev_alpha[i] = -1.0f;
    float max_jump_seen = 0.0f;
    float max_x = 0.0f, max_y = 0.0f;

    for (int f = 0; f < frames; f++) {
        /* 1. age + integrate (emitter.c ordering: move existing, then spawn) */
        for (int i = 0; i < n_used; i++) {
            SP* p = &pool[i];
            if (!p->alive) continue;
            p->age += DT;
            if (p->age >= p->life) {
                p->alive = 0; prev_alpha[i] = -1.0f; continue;
            }
            float k = expf(-drag * DT);
            p->vx *= k; p->vy *= k;
            p->vx += gx * DT; p->vy += gy * DT;
            (void)swirl;
            p->x += p->vx * DT;
            p->y += p->vy * DT;

            /* 2. boundary strategy */
            BPart bp = { p->x, p->y, p->vx, p->vy, p->alpha, p->alive };
            int ev = bd_apply(&b, &bp, mode);
            if (ev < 0) { fprintf(stderr, "bd_apply error\n"); return 3; }
            p->x = bp.x; p->y = bp.y; p->vx = bp.vx; p->vy = bp.vy;
            p->alpha = bp.alpha;
            if (ev & BD_EVENT_KILLED) { p->alive = 0; killed_by_boundary++; }
            if (ev & BD_EVENT_BOUNCED) bounce_total++;
            if (!p->alive) { prev_alpha[i] = -1.0f; continue; }

            /* 3. flicker measurement: |delta alpha| between consecutive
             *    frames of the same living particle */
            if (prev_alpha[i] >= 0.0f) {
                float jump = fabsf(p->alpha - prev_alpha[i]);
                if (jump > max_jump_seen) max_jump_seen = jump;
            }
            prev_alpha[i] = p->alpha;
            if (fabsf(p->x) > max_x) max_x = fabsf(p->x);
            if (fabsf(p->y) > max_y) max_y = fabsf(p->y);
        }

        /* 4. rate emission (rate accumulator, same scheme as emitter.c) */
        rate_acc += RATE * DT;
        int want = (int)rate_acc;
        rate_acc -= (float)want;
        for (int q = 0; q < want; q++) spawn();

        int alive = 0, out = 0;
        for (int i = 0; i < n_used; i++) {
            if (!pool[i].alive) continue;
            alive++;
            if (!in_box(&pool[i], &b)) out++;
        }
        fprintf(log, "%d,%.3f,%d,%d,%.4f,%u,%u,%.4f,%.3f,%.3f\n",
                f, (f + 1) * DT, alive, out,
                alive ? (float)out / (float)alive : 0.0f,
                killed_by_boundary, bounce_total, max_jump_seen, max_x, max_y);
    }
    fclose(log);

    /* final-frame particle snapshot */
    char fn[256];
    snprintf(fn, sizeof(fn), "%s_final.csv", argv[2]);
    FILE* out = fopen(fn, "w");
    fprintf(out, "id,x,y,alpha,alive\n");
    int alive = 0, oob = 0;
    for (int i = 0; i < n_used; i++) {
        if (!pool[i].alive) continue;
        alive++;
        if (!in_box(&pool[i], &b)) oob++;
        fprintf(out, "%u,%.5f,%.5f,%.4f,1\n", pool[i].id, pool[i].x,
                pool[i].y, pool[i].alpha);
    }
    fclose(out);

    printf("mode=%s alive=%d out_of_box=%d share=%.4f "
           "emitted=%u boundary_killed=%u bounces=%u dropped=%u "
           "max_alpha_jump=%.4f max_abs_coord=%.2f\n",
           argv[1], alive, oob,
           alive ? (float)oob / (float)alive : 0.0f,
           emitted_total, killed_by_boundary, bounce_total, dropped,
           max_jump_seen, fmaxf(max_x, max_y));
    free(prev_alpha);
    return 0;
}
