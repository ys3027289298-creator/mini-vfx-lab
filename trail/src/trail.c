
/* trail: age-sorted ribbon points with width taper. */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAXT 32

typedef struct { float x, y, age; } TP;
typedef struct { TP p[MAXT]; int n; } Trail;

static int tr_push(Trail* t, float x, float y) {
    if (t->n >= MAXT) {
        memmove(&t->p[0], &t->p[1], sizeof(TP) * (MAXT - 1));
        t->n = MAXT - 1;
    }
    t->p[t->n].x = x; t->p[t->n].y = y; t->p[t->n].age = 0.0f;
    return t->n++;
}

static int tr_age(Trail* t, float dt, float max_age) {
    if (dt < 0.0f || max_age <= 0.0f) return -1;
    for (int i = 0; i < t->n; i++) t->p[i].age += dt;
    int w = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->p[i].age < max_age) t->p[w++] = t->p[i];
    }
    t->n = w;
    return w;
}

/* width falls linearly from head to tail, normalized by max_age */
static float tr_width(const Trail* t, int i, float base, float max_age) {
    if (t->n <= 0 || i < 0 || i >= t->n) return -1.0f;
    float k = t->p[i].age / max_age;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    return base * (1.0f - k);
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Trail t; memset(&t, 0, sizeof(t));
    for (int i = 0; i < 5; i++) tr_push(&t, (float)i, 0);
    check("push_count", t.n == 5);

    check("age_ok", tr_age(&t, 0.1f, 1.0f) > 0);
    check("age_accumulated", fabsf(t.p[0].age - 0.1f) < 1e-5f);

    check("expire", tr_age(&t, 2.0f, 1.0f) == 0 && t.n == 0);

    Trail w; memset(&w, 0, sizeof(w));
    tr_push(&w, 0, 0);
    tr_age(&w, 0.5f, 1.0f);
    check("width_head", fabsf(tr_width(&w, 0, 4.0f, 1.0f) - 2.0f) < 1e-5f);

    Trail o; memset(&o, 0, sizeof(o));
    for (int i = 0; i < MAXT + 3; i++) tr_push(&o, (float)i, 0);
    check("ring_capped", o.n == MAXT);

    check("bad_dt", tr_age(&o, -1.0f, 1.0f) == -1);
    check("bad_age", tr_age(&o, 1.0f, 0.0f) == -1);
    check("bad_index", tr_width(&o, -1, 1.0f, 1.0f) < 0.0f);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
