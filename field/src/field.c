
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

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
