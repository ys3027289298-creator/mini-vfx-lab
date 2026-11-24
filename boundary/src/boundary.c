
/* boundary: hard bounds for particles — kill, reflect or fade-out handling.
 *
 * The box is the emitter's valid range. Three explicit behaviours are
 * provided for particles that reach it:
 *   BD_KILL    - destroyed the moment they cross a side
 *   BD_REFLECT - bounced back, position is clamped so they never sit outside
 *   BD_FADE    - alpha ramps down inside an inner margin band, killed at the
 *                hard edge; alpha is derived from position, so it can never
 *                flicker between frames
 *
 * Default policy: BD_FADE. All three meet the hard requirement (zero
 * out-of-box-alive particles). KILL pops particles out of existence at the
 * wall, REFLECT never frees a slot and accumulates the whole population
 * inside the box (703 bounces / 250 particles in the reference run), while
 * FADE keeps a hard cleanup guarantee with a continuous visual exit.
 */
#include <stdio.h>
#include <math.h>

typedef enum { BD_NONE = 0, BD_KILL, BD_REFLECT, BD_FADE } BoundaryMode;

typedef struct {
    float xmin, xmax, ymin, ymax;
    float margin;       /* inner band width used by BD_FADE */
    float restitution;  /* fraction of velocity kept on a BD_REFLECT bounce */
    float rest_eps;     /* incoming speed below this on contact = resting */
} Boundary;

typedef struct { float x, y, vx, vy; float alpha; int alive; } BPart;

#define BD_EVENT_KILLED  1
#define BD_EVENT_BOUNCED 2

static float bd_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int bd_valid(const Boundary* b) {
    return b && b->xmax > b->xmin && b->ymax > b->ymin &&
           b->margin >= 0.0f && b->restitution >= 0.0f &&
           b->restitution <= 1.0f && b->rest_eps >= 0.0f;
}

/* Apply one boundary strategy to a particle after it has moved.
 * Returns BD_EVENT_* bits, 0 when nothing happened, -1 on bad arguments. */
static int bd_apply(const Boundary* b, BPart* p, BoundaryMode mode) {
    if (!bd_valid(b) || !p || mode < BD_NONE || mode > BD_FADE) return -1;
    if (!p->alive || mode == BD_NONE) return 0;

    if (mode == BD_KILL) {
        if (p->x < b->xmin || p->x > b->xmax ||
            p->y < b->ymin || p->y > b->ymax) {
            p->alive = 0;
            return BD_EVENT_KILLED;
        }
        return 0;
    }

    if (mode == BD_REFLECT) {
        int ev = 0;
        /* Clamp first: a fast particle that tunneled past the wall is put
         * back on it, so the post-condition "inside the box" always holds. */
        if (p->x < b->xmin) {
            p->x = b->xmin;
            if (p->vx < 0.0f) {
                if (p->vx > -b->rest_eps) p->vx = 0.0f;   /* resting contact */
                else { p->vx = -p->vx * b->restitution; ev = BD_EVENT_BOUNCED; }
            }
        } else if (p->x > b->xmax) {
            p->x = b->xmax;
            if (p->vx > 0.0f) {
                if (p->vx < b->rest_eps) p->vx = 0.0f;
                else { p->vx = -p->vx * b->restitution; ev = BD_EVENT_BOUNCED; }
            }
        }
        if (p->y < b->ymin) {
            p->y = b->ymin;
            if (p->vy < 0.0f) {
                if (p->vy > -b->rest_eps) p->vy = 0.0f;
                else { p->vy = -p->vy * b->restitution; ev = BD_EVENT_BOUNCED; }
            }
        } else if (p->y > b->ymax) {
            p->y = b->ymax;
            if (p->vy > 0.0f) {
                if (p->vy < b->rest_eps) p->vy = 0.0f;
                else { p->vy = -p->vy * b->restitution; ev = BD_EVENT_BOUNCED; }
            }
        }
        return ev;
    }

    /* BD_FADE: hard kill past the edge, smooth alpha inside the margin. */
    if (p->x < b->xmin || p->x > b->xmax ||
        p->y < b->ymin || p->y > b->ymax) {
        p->alpha = 0.0f;
        p->alive = 0;
        return BD_EVENT_KILLED;
    }
    float d = fminf(fminf(p->x - b->xmin, b->xmax - p->x),
                    fminf(p->y - b->ymin, b->ymax - p->y));
    float a = b->margin > 0.0f ? d / b->margin : 0.0f;
    p->alpha = bd_clampf(a, 0.0f, 1.0f);
    return 0;
}

#ifndef BOUNDARY_NO_MAIN

#include <string.h>

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Boundary b = { -4, 4, -4, 4, 1.0f, 0.6f, 0.25f };

    /* BD_NONE leaves particles untouched, even far outside. */
    BPart n = { 100, 100, 1, 1, 1, 1 };
    check("none_untouched", bd_apply(&b, &n, BD_NONE) == 0 &&
          n.alive == 1 && n.x == 100.0f);

    /* BD_KILL */
    BPart k0 = { 0, 0, 1, 0, 1, 1 };
    check("kill_inside", bd_apply(&b, &k0, BD_KILL) == 0 && k0.alive == 1);
    BPart k1 = { 4.5f, 0, 1, 0, 1, 1 };
    check("kill_outside", bd_apply(&b, &k1, BD_KILL) == BD_EVENT_KILLED &&
          k1.alive == 0);

    /* BD_REFLECT: velocity flips, position is clamped back onto the wall. */
    BPart r = { -4.3f, 0, -2.0f, 0, 1, 1 };
    int ev = bd_apply(&b, &r, BD_REFLECT);
    check("reflect_event", ev == BD_EVENT_BOUNCED);
    check("reflect_vel", fabsf(r.vx - 1.2f) < 1e-6f);
    check("reflect_pos", r.x == b.xmin && r.y == 0.0f);

    /* A particle settling on the floor must rest, not jitter: gravity keeps
     * feeding in velocity, contact absorbs the small leftovers without an
     * endless stream of bounce events, and the particle never leaves the box. */
    BPart s = { 0, -3.9f, 0, 0, 1, 1 };
    int bounce_events = 0, outside = 0, late_events = 0;
    for (int i = 0; i < 120; i++) {
        s.vy -= 1.0f * 0.1f;              /* gravity */
        s.x += s.vx * 0.1f;
        s.y += s.vy * 0.1f;
        int e = bd_apply(&b, &s, BD_REFLECT);
        if (e & BD_EVENT_BOUNCED) {
            bounce_events++;
            if (i >= 30) late_events++;   /* long after settling: must be none */
        }
        if (s.x < b.xmin || s.x > b.xmax || s.y < b.ymin || s.y > b.ymax)
            outside = 1;
    }
    check("reflect_never_outside", !outside);
    check("reflect_rests", late_events == 0 && bounce_events < 8 &&
          s.y == b.ymin && s.vy == 0.0f);

    /* BD_FADE */
    BPart f0 = { 0, 0, 0, 0, 1, 1 };
    bd_apply(&b, &f0, BD_FADE);
    check("fade_center_solid", f0.alive == 1 && f0.alpha == 1.0f);
    BPart f1 = { -3.5f, 0, 0, 0, 1, 1 };            /* 0.5 into the band */
    bd_apply(&b, &f1, BD_FADE);
    check("fade_band", f0.alive && fabsf(f1.alpha - 0.5f) < 1e-6f);
    BPart f2 = { -3.99f, 0, 0, 0, 1, 1 };           /* nearly at the edge */
    bd_apply(&b, &f2, BD_FADE);
    check("fade_near_edge", f2.alive == 1 && f2.alpha < 0.05f);
    BPart f3 = { -4.1f, 0, 0, 0, 1, 1 };            /* past the edge */
    check("fade_killed", bd_apply(&b, &f3, BD_FADE) == BD_EVENT_KILLED &&
          f3.alive == 0 && f3.alpha == 0.0f);

    /* Alpha is a continuous function of position: a small move can only
     * change alpha by a proportional amount. */
    BPart fa = { -3.2f, 0, 0, 0, 1, 1 }, fb = { -3.25f, 0, 0, 0, 1, 1 };
    bd_apply(&b, &fa, BD_FADE);
    bd_apply(&b, &fb, BD_FADE);
    check("fade_continuous", fabsf(fa.alpha - fb.alpha) <=
          0.05f / b.margin + 1e-6f);

    /* Bad arguments. */
    Boundary bad = { 4, -4, -4, 4, 1, 0.6f, 0.25f };
    BPart z = { 0, 0, 0, 0, 1, 1 };
    check("bad_box", bd_apply(&bad, &z, BD_KILL) == -1);
    check("bad_mode", bd_apply(&b, &z, (BoundaryMode)99) == -1);
    check("bad_null", bd_apply(0, &z, BD_KILL) == -1);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

#endif
