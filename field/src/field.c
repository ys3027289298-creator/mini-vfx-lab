
/* field: gravity, linear drag and swirl forces integrated per step. */
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, vx, vy; } Part;

static int step(Part* p, float dt, float gx, float gy, float drag, float swirl) {
    if (dt < 0.0f || drag < 0.0f) return -1;
    /* drag applied as exponential decay */
    float k = expf(-drag * dt);
    p->vx *= k; p->vy *= k;
    /* swirl: tangential acceleration around origin */
    float r = sqrtf(p->x * p->x + p->y * p->y);
    if (r > 1e-6f && swirl != 0.0f) {
        float tx = -p->y / r, ty = p->x / r;
        p->vx += tx * swirl * dt;
        p->vy += ty * swirl * dt;
    }
    p->vx += gx * dt;
    p->vy += gy * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    return 0;
}

/* velocity-Verlet: for constant acceleration (drag=0, swirl=0) the 1s
   free-fall displacement is exact up to float rounding, which the Euler
   step in step() cannot achieve at 60Hz. Drag keeps the exponential
   convention, split into two half-kicks around the position update. */
static int step_verlet(Part* p, float dt, float gx, float gy, float drag, float swirl) {
    if (dt < 0.0f || drag < 0.0f) return -1;
    float h = expf(-0.5f * drag * dt);
    p->vx *= h; p->vy *= h;
    float ax1 = gx, ay1 = gy;
    float r = sqrtf(p->x * p->x + p->y * p->y);
    if (r > 1e-6f && swirl != 0.0f) {
        ax1 += -p->y / r * swirl;
        ay1 +=  p->x / r * swirl;
    }
    p->x += p->vx * dt + 0.5f * ax1 * dt * dt;
    p->y += p->vy * dt + 0.5f * ay1 * dt * dt;
    float r2 = sqrtf(p->x * p->x + p->y * p->y);
    float ax2 = gx, ay2 = gy;
    if (r2 > 1e-6f && swirl != 0.0f) {
        ax2 += -p->y / r2 * swirl;
        ay2 +=  p->x / r2 * swirl;
    }
    p->vx += 0.5f * (ax1 + ax2) * dt;
    p->vy += 0.5f * (ay1 + ay2) * dt;
    p->vx *= h; p->vy *= h;
    return 0;
}

static float speed(const Part* p) { return sqrtf(p->vx * p->vx + p->vy * p->vy); }

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Part a = { 0, 10, 0, 0 };
    step(&a, 1.0f, 0, -9.8f, 0, 0);
    check("gravity_pulls", a.vy < 0.0f);
    check("gravity_magnitude", fabsf(a.vy + 9.8f) < 1e-3f);

    Part b = { 0, 0, 10, 0 };
    for (int i = 0; i < 10; i++) step(&b, 0.1f, 0, 0, 5.0f, 0);
    check("drag_slows", speed(&b) < 10.0f * 0.5f);

    Part c = { 1, 0, 0, 0 };
    step(&c, 0.1f, 0, 0, 0, 10.0f);
    check("swirl_perpendicular", c.vx < 0.01f && c.vy > 0.5f);

    Part d = { 0, 0, 1, 0 };
    float before = speed(&d);
    for (int i = 0; i < 5; i++) step(&d, 0.1f, 0, 0, 0, 0);
    check("free_flight", fabsf(speed(&d) - before) < 1e-5f);

    check("bad_dt", step(&d, -1.0f, 0, 0, 0, 0) == -1);
    check("bad_drag", step(&d, 0.1f, 0, 0, -1.0f, 0) == -1);

    Part v = { 0, 0, 0, 0 };
    for (int i = 0; i < 60; i++) step_verlet(&v, 1.0f / 60.0f, 0, -9.8f, 0, 0);
    check("verlet_freefall_1s", fabsf(v.y - (-4.9f)) < 0.05f);
    check("verlet_freefall_tight", fabsf(v.y - (-4.9f)) < 1e-3f);
    check("verlet_velocity", fabsf(v.vy + 9.8f) < 1e-3f);
    check("verlet_bad_dt", step_verlet(&v, -1.0f, 0, 0, 0, 0) == -1);

    Part vf = { 0, 0, 10, 0 };
    for (int i = 0; i < 10; i++) step_verlet(&vf, 0.1f, 0, 0, 5.0f, 0);
    check("verlet_drag_slows", speed(&vf) < 10.0f * 0.5f);

    Part vs = { 1, 0, 0, 0 };
    step_verlet(&vs, 0.1f, 0, 0, 0, 10.0f);
    check("verlet_swirl_turns", vs.vy > 0.5f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
