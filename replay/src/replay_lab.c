/*
 * replay_lab.c — parameter/state classification and 3-tier archive replay lab.
 *
 * The REAL emitter.c / field.c / trail.c are unity-built into this translation
 * unit (only their selftest main/check symbols are renamed), so every replay
 * runs the exact update/integration/ring-buffer semantics shipped in the repo.
 *
 * Experiment protocol
 * -------------------
 * A deterministic corpus of clips is generated. Each clip is warmed up for a
 * random number of frames to reach a non-canonical boundary S0. From there
 * NFRAME frames are simulated into an immutable truth trace. Archives at three
 * size tiers are written and parsed back; the parsed archive alone reconstructs
 * the replay. A field-level diff names every quantity that the archived
 * parameter set fails to determine uniquely at S0, and the first observable
 * leaf that diverges during replay.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- unity-build the real modules, renaming selftest-only symbols ---- */
#define main    emitter_selftest_main
#define failures emitter_failures
#define check   emitter_check
#include "../../emitter/src/emitter.c"
#undef main
#undef failures
#undef check

#define main    field_selftest_main
#define failures field_failures
#define check   field_check
#include "../../field/src/field.c"
#undef main
#undef failures
#undef check

#define main    trail_selftest_main
#define failures trail_failures
#define check   trail_check
#include "../../trail/src/trail.c"
#undef main
#undef failures
#undef check

/* ============================== corpus ============================== */

#define NCLIPS 24
#define NFIELD 4
#define NFRAME 80
#define WARMMAX 40

/* Deterministic corpus generator. This RNG is test scaffolding, NOT runtime
 * state of the VFX effect, so no seed needs to live in a replay archive. */
typedef struct { uint64_t s; } Rng;

static uint32_t rng_u32(Rng *r) {
    uint64_t z = (r->s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (uint32_t)(z ^ (z >> 31));
}
static float rng_f(Rng *r, float lo, float hi) {
    return lo + (hi - lo) * ((float)rng_u32(r) / (float)UINT32_MAX);
}
static int rng_i(Rng *r, int lo, int hi) {
    return lo + (int)(rng_u32(r) % (uint32_t)(hi - lo + 1));
}

/* Replayable input: clip-level parameter block. Every value the update
 * functions read is pinned here and serialized into every archive. */
typedef struct {
    float rate;
    float gx, gy, drag, swirl;
    float max_age, width_base;
    /* implicit call-site spawn descriptor / build constants in em_update() */
    float sx, sy, svx, svy, slife;
    uint32_t maxp, maxt;          /* compile-time capacities */
} Params;

typedef struct { float dt; int burst; } FIn;

typedef struct {
    int id, warm;
    Params q;
    FIn win[WARMMAX]; /* pre-boundary input stream, needed by origin replay */
    FIn in[NFRAME];
    Part    fp_init[NFIELD]; /* t=0 initial conditions (promoted parameters) */
    Emitter em0;
    Trail   tr0;
    Part    fp0[NFIELD];
} Clip;

typedef struct {
    Emitter em;
    Trail   tr;
    Part    fp[NFIELD];
} World;

static Clip  clips[NCLIPS];
static World truth[NCLIPS][NFRAME + 1];

/* One frame of the bundled showreel world. All three calls are the real
 * module functions. The trail follows field particle 0. */
static void world_frame(World *w, const FIn *in, const Params *q) {
    em_update(&w->em, in->dt, q->rate, in->burst);
    for (int i = 0; i < NFIELD; i++)
        step(&w->fp[i], in->dt, q->gx, q->gy, q->drag, q->swirl);
    tr_push(&w->tr, w->fp[0].x, w->fp[0].y);
    tr_age(&w->tr, in->dt, q->max_age);
}

static const float DT_CHOICES[4] = { 1.0f / 60.0f, 1.0f / 30.0f, 0.02f, 0.05f };

static void make_clip(Clip *c, int id) {
    Rng r = { 0x51a7beef1234ULL ^ (uint64_t)(id + 1) * 0x9e3779b97f4a7c15ULL };
    memset(c, 0, sizeof(*c));
    c->id = id;
    /* Every 6th clip starts at the canonical zero boundary: parameters then
     * do uniquely determine S0 without any archived runtime state. */
    c->warm = (id % 6 == 0) ? 0 : rng_i(&r, 4, 34);

    Params *q = &c->q;
    q->rate       = rng_f(&r, 2.0f, 18.0f);
    q->gx         = rng_f(&r, -2.0f, 2.0f);
    q->gy         = rng_f(&r, -12.0f, -1.0f);
    q->drag       = rng_f(&r, 0.0f, 3.0f);
    q->swirl      = rng_f(&r, -8.0f, 8.0f);
    q->max_age    = rng_f(&r, 0.5f, 2.5f);
    q->width_base = rng_f(&r, 1.0f, 6.0f);
    /* exact constants hard-coded in the shipped em_update() */
    q->sx = 0.0f; q->sy = 0.0f; q->svx = 0.0f; q->svy = 1.0f; q->slife = 1.0f;
    q->maxp = MAXP; q->maxt = MAXT;

    for (int f = 0; f < NFRAME; f++) {
        c->in[f].dt = DT_CHOICES[rng_i(&r, 0, 3)];
        int z = rng_i(&r, 0, 99);
        c->in[f].burst = (z < 62) ? 0 : (z < 92) ? rng_i(&r, 1, 5)
                                                : rng_i(&r, 8, 24);
    }

    World w;
    memset(&w, 0, sizeof(w));
    em_init(&w.em);
    if (c->warm > 0) {
        for (int i = 0; i < NFIELD; i++) {
            w.fp[i].x  = rng_f(&r, -8.0f, 8.0f);
            w.fp[i].y  = rng_f(&r, -8.0f, 8.0f);
            w.fp[i].vx = rng_f(&r, -3.0f, 3.0f);
            w.fp[i].vy = rng_f(&r, -3.0f, 3.0f);
            c->fp_init[i] = w.fp[i];
        }
        for (int h = 0; h < c->warm; h++) {
            c->win[h].dt = DT_CHOICES[rng_i(&r, 0, 3)];
            c->win[h].burst = (rng_i(&r, 0, 99) < 80) ? 0 : rng_i(&r, 1, 8);
            world_frame(&w, &c->win[h], q);
        }
    }

    c->em0 = w.em;
    c->tr0 = w.tr;
    memcpy(c->fp0, w.fp, sizeof(c->fp0));

    truth[id][0] = w;
    for (int f = 0; f < NFRAME; f++) {
        world_frame(&w, &c->in[f], q);
        truth[id][f + 1] = w;
    }
}

/* ====================== explicit byte archive IO ====================== */

typedef struct { uint8_t *d; size_t len, cap; } Buf;

static void buf_init(Buf *b) {
    b->cap = 4096; b->len = 0;
    b->d = (uint8_t *)malloc(b->cap);
    if (!b->d) { fprintf(stderr, "oom\n"); exit(2); }
}
static void buf_reset(Buf *b) { b->len = 0; }
static void buf_free(Buf *b) { free(b->d); b->d = NULL; }

static void buf_need(Buf *b, size_t n) {
    if (b->len + n <= b->cap) return;
    while (b->len + n > b->cap) b->cap *= 2;
    b->d = (uint8_t *)realloc(b->d, b->cap);
    if (!b->d) { fprintf(stderr, "oom\n"); exit(2); }
}
static void put_raw(Buf *b, const void *p, size_t n) {
    buf_need(b, n);
    memcpy(b->d + b->len, p, n);
    b->len += n;
}
static void put_u8(Buf *b, unsigned v)  { uint8_t x = (uint8_t)v; put_raw(b, &x, 1); }
static void put_u16(Buf *b, unsigned v) {
    uint8_t x[2] = { (uint8_t)(v & 255), (uint8_t)((v >> 8) & 255) };
    put_raw(b, x, 2);
}
static void put_u32(Buf *b, uint32_t v) {
    uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_raw(b, x, 4);
}
static void put_f32(Buf *b, float f) {
    uint32_t u; memcpy(&u, &f, 4); put_u32(b, u);
}

typedef struct { const uint8_t *d; size_t len, pos; int bad; } Rd;

static unsigned rd_raw(Rd *r, void *out, size_t n) {
    if (r->bad || r->pos + n > r->len) { r->bad = 1; memset(out, 0, n); return 0; }
    memcpy(out, r->d + r->pos, n);
    r->pos += n;
    return 1;
}
static unsigned rd_u8(Rd *r)  { uint8_t x = 0; rd_raw(r, &x, 1); return x; }
static uint16_t rd_u16(Rd *r) {
    uint8_t x[2] = {0,0}; rd_raw(r, x, 2);
    return (uint16_t)(x[0] | (x[1] << 8));
}
static uint32_t rd_u32(Rd *r) {
    uint8_t x[4] = {0,0,0,0}; rd_raw(r, x, 4);
    return (uint32_t)x[0] | ((uint32_t)x[1] << 8) |
           ((uint32_t)x[2] << 16) | ((uint32_t)x[3] << 24);
}
static float rd_f32(Rd *r) { uint32_t u = rd_u32(r); float f; memcpy(&f, &u, 4); return f; }

/* ===================== archive schema / classification ===================== */

#define ARC_MAGIC0 'V'
#define ARC_MAGIC1 'F'
#define ARC_MAGIC2 'X'
#define ARC_MAGIC3 'A'
#define ARC_VER 1
#define KIND_MASK 1   /* only groups named in mask are present */
#define KIND_FULL 2   /* fixed-capacity raw leaf snapshot */
#define KIND_ORIGIN 3 /* no state: full input stream from canonical zero S0 */

/* Promotable boundary-state groups. Every bit is a quantity (or tight group)
 * that can either stay "state" (re-derived / zeroed) or be promoted into the
 * archived parameter set. */
#define G_RACC  (1u << 0)  /* em.rate_acc fractional spawn accumulator */
#define G_N     (1u << 1)  /* em.n high-water slot count */
#define G_CTR   (1u << 2)  /* em.emitted, em.dropped monotonic counters */
#define G_SLOT  (1u << 3)  /* em.p[0..n): full particle slot records */
#define G_TRL   (1u << 4)  /* tr.n + tr.p[] ribbon history */
#define G_FLD   (1u << 5)  /* fp[].x/y/vx/vy field particle states */

/* Reverse-solved minimum that satisfies the uniqueness criterion. G_N is
 * omitted because G_SLOT's record count self-describes em.n. */
#define MASK_STAR (G_RACC | G_CTR | G_SLOT | G_TRL | G_FLD)

static void write_params(Buf *b, const Params *q) {
    put_f32(b, q->rate);
    put_f32(b, q->gx); put_f32(b, q->gy);
    put_f32(b, q->drag); put_f32(b, q->swirl);
    put_f32(b, q->max_age); put_f32(b, q->width_base);
    put_f32(b, q->sx); put_f32(b, q->sy);
    put_f32(b, q->svx); put_f32(b, q->svy); put_f32(b, q->slife);
    put_u32(b, q->maxp); put_u32(b, q->maxt);
}
static void read_params(Rd *r, Params *q) {
    q->rate = rd_f32(r);
    q->gx = rd_f32(r); q->gy = rd_f32(r);
    q->drag = rd_f32(r); q->swirl = rd_f32(r);
    q->max_age = rd_f32(r); q->width_base = rd_f32(r);
    q->sx = rd_f32(r); q->sy = rd_f32(r);
    q->svx = rd_f32(r); q->svy = rd_f32(r); q->slife = rd_f32(r);
    q->maxp = rd_u32(r); q->maxt = rd_u32(r);
}

static void write_particle(Buf *b, const P *p) {
    put_f32(b, p->x); put_f32(b, p->y);
    put_f32(b, p->vx); put_f32(b, p->vy);
    put_f32(b, p->age); put_f32(b, p->life);
    put_u8(b, (unsigned)p->alive);
}
static void read_particle(Rd *r, P *p) {
    p->x = rd_f32(r); p->y = rd_f32(r);
    p->vx = rd_f32(r); p->vy = rd_f32(r);
    p->age = rd_f32(r); p->life = rd_f32(r);
    p->alive = (int)rd_u8(r);
}
static void write_tpoint(Buf *b, const TP *t) {
    put_f32(b, t->x); put_f32(b, t->y); put_f32(b, t->age);
}
static void read_tpoint(Rd *r, TP *t) {
    t->x = rd_f32(r); t->y = rd_f32(r); t->age = rd_f32(r);
}
static void write_part(Buf *b, const Part *p) {
    put_f32(b, p->x); put_f32(b, p->y);
    put_f32(b, p->vx); put_f32(b, p->vy);
}
static void read_part(Rd *r, Part *p) {
    p->x = rd_f32(r); p->y = rd_f32(r);
    p->vx = rd_f32(r); p->vy = rd_f32(r);
}

static size_t write_archive(Buf *b, const Clip *c, int kind, uint32_t mask) {
    buf_reset(b);
    put_u8(b, ARC_MAGIC0); put_u8(b, ARC_MAGIC1);
    put_u8(b, ARC_MAGIC2); put_u8(b, ARC_MAGIC3);
    put_u16(b, ARC_VER);
    put_u8(b, (unsigned)kind);
    put_u32(b, mask);
    put_u32(b, (uint32_t)c->id);

    write_params(b, &c->q);

    int pre = (kind == KIND_ORIGIN) ? c->warm : 0;
    put_u16(b, (unsigned)(pre + NFRAME));
    for (int h = 0; h < pre; h++) {
        put_f32(b, c->win[h].dt);
        put_u8(b, (unsigned)c->win[h].burst);
    }
    for (int f = 0; f < NFRAME; f++) {
        put_f32(b, c->in[f].dt);
        put_u8(b, (unsigned)c->in[f].burst);
    }

    const Emitter *e = &c->em0;
    const Trail   *t = &c->tr0;

    if (kind == KIND_FULL) {
        put_u32(b, (uint32_t)e->n);
        put_f32(b, e->rate_acc);
        put_u32(b, (uint32_t)e->emitted);
        put_u32(b, (uint32_t)e->dropped);
        for (int i = 0; i < MAXP; i++) write_particle(b, &e->p[i]);
        put_u32(b, (uint32_t)t->n);
        for (int i = 0; i < MAXT; i++) write_tpoint(b, &t->p[i]);
        for (int i = 0; i < NFIELD; i++) write_part(b, &c->fp0[i]);
    } else if (kind == KIND_ORIGIN) {
        /* the only non-canonical t=0 quantities: field initial conditions */
        for (int i = 0; i < NFIELD; i++) write_part(b, &c->fp_init[i]);
    } else {
        if (mask & G_RACC) put_f32(b, e->rate_acc);
        if (mask & G_N)    put_u32(b, (uint32_t)e->n);
        if (mask & G_CTR) {
            put_u32(b, (uint32_t)e->emitted);
            put_u32(b, (uint32_t)e->dropped);
        }
        if (mask & G_SLOT) {
            put_u16(b, (unsigned)e->n);
            for (int i = 0; i < e->n; i++) write_particle(b, &e->p[i]);
        }
        if (mask & G_TRL) {
            put_u16(b, (unsigned)t->n);
            for (int i = 0; i < t->n; i++) write_tpoint(b, &t->p[i]);
        }
        if (mask & G_FLD)
            for (int i = 0; i < NFIELD; i++) write_part(b, &c->fp0[i]);
    }
    return b->len;
}

/* Parses an archive into the replay inputs and the reconstructed S0.
 * Returns 1 on a complete, schema-valid archive. */
static int read_archive(const uint8_t *bytes, size_t len,
                       int *out_id, int *out_kind, uint32_t *out_mask,
                       Params *q, FIn *in, int *out_nall, int *out_pre,
                       World *s0) {
    Rd rstore = { bytes, len, 0, 0 };
    Rd *r = &rstore;
    if (len < 4 || bytes[0] != ARC_MAGIC0 || bytes[1] != ARC_MAGIC1 ||
        bytes[2] != ARC_MAGIC2 || bytes[3] != ARC_MAGIC3)
        return 0;
    r->pos = 4;
    unsigned ver = rd_u16(r);
    int kind = (int)rd_u8(r);
    uint32_t mask = rd_u32(r);
    int id = (int)rd_u32(r);
    if (ver != ARC_VER || r->bad) return 0;
    if (out_id) *out_id = id;
    if (out_kind) *out_kind = kind;
    if (out_mask) *out_mask = mask;

    read_params(r, q);
    unsigned nf = rd_u16(r);
    int maxall = WARMMAX + NFRAME;
    if (nf < (unsigned)NFRAME || nf > (unsigned)maxall) r->bad = 1;
    int pre = 0;
    if (kind == KIND_ORIGIN) {
        if (nf >= (unsigned)NFRAME) pre = (int)nf - NFRAME;
    } else if (nf != (unsigned)NFRAME) {
        r->bad = 1;
    }
    for (unsigned f = 0; f < nf && f < (unsigned)maxall; f++) {
        in[f].dt = rd_f32(r);
        in[f].burst = (int)rd_u8(r);
    }
    if (out_nall) *out_nall = (int)nf;
    if (out_pre) *out_pre = pre;

    memset(s0, 0, sizeof(*s0));
    em_init(&s0->em);

    if (kind == KIND_ORIGIN) {
        /* canonical zero Emitter/Trail; archived field initial conditions */
        for (int i = 0; i < NFIELD; i++) read_part(r, &s0->fp[i]);
    } else if (kind == KIND_FULL) {
        s0->em.n = (int)rd_u32(r);
        s0->em.rate_acc = rd_f32(r);
        s0->em.emitted = (int)rd_u32(r);
        s0->em.dropped = (int)rd_u32(r);
        if (s0->em.n < 0 || s0->em.n > MAXP) r->bad = 1;
        for (int i = 0; i < MAXP; i++) read_particle(r, &s0->em.p[i]);
        s0->tr.n = (int)rd_u32(r);
        if (s0->tr.n < 0 || s0->tr.n > MAXT) r->bad = 1;
        for (int i = 0; i < MAXT; i++) read_tpoint(r, &s0->tr.p[i]);
        for (int i = 0; i < NFIELD; i++) read_part(r, &s0->fp[i]);
    } else if (kind == KIND_MASK) {
        if (mask & G_RACC) s0->em.rate_acc = rd_f32(r);
        if (mask & G_N)    s0->em.n = (int)rd_u32(r);
        if (mask & G_CTR) {
            s0->em.emitted = (int)rd_u32(r);
            s0->em.dropped = (int)rd_u32(r);
        }
        if (mask & G_SLOT) {
            int m = (int)rd_u16(r);
            if (m > MAXP) { r->bad = 1; m = 0; }
            for (int i = 0; i < m; i++) read_particle(r, &s0->em.p[i]);
            s0->em.n = m; /* SLOT self-describes the high-water count */
        }
        if (mask & G_TRL) {
            int m = (int)rd_u16(r);
            if (m > MAXT) { r->bad = 1; m = 0; }
            s0->tr.n = m;
            for (int i = 0; i < m; i++) read_tpoint(r, &s0->tr.p[i]);
        }
        if (mask & G_FLD)
            for (int i = 0; i < NFIELD; i++) read_part(r, &s0->fp[i]);
        if (s0->em.n < 0 || s0->em.n > MAXP || s0->tr.n < 0 || s0->tr.n > MAXT)
            r->bad = 1;
    } else {
        r->bad = 1;
    }
    return !r->bad;
}

/* ===================== field-level divergence naming ===================== */

enum {
    Q_RACC = 0, Q_N, Q_EMIT, Q_DROP,
    Q_PX, Q_PY, Q_PVX, Q_PVY, Q_PAGE, Q_PLIFE, Q_PALIVE,
    Q_TN, Q_TX, Q_TY, Q_TAGE, Q_TWIDTH,
    Q_FX, Q_FY, Q_FVX, Q_FVY,
    Q_NQ
};

static const char *QNAME[Q_NQ] = {
    "em.rate_acc", "em.n", "em.emitted", "em.dropped",
    "em.p[].x", "em.p[].y", "em.p[].vx", "em.p[].vy",
    "em.p[].age", "em.p[].life", "em.p[].alive",
    "tr.n", "tr.p[].x", "tr.p[].y", "tr.p[].age", "tr.width[]",
    "fp[].x", "fp[].y", "fp[].vx", "fp[].vy"
};

typedef struct {
    long  cnt[Q_NQ];
    char  ex[Q_NQ][48];
    int   first_q;
    char  first_leaf[48];
} Diff;

static int neqf(float a, float b) {
    float d = fabsf(a - b);
    float scale = 1.0f + fmaxf(fabsf(a), fabsf(b));
    return d > 1e-6f * scale;
}

static void diff_add(Diff *d, int q, const char *leaf) {
    d->cnt[q]++;
    if (d->ex[q][0] == 0) snprintf(d->ex[q], sizeof(d->ex[q]), "%s", leaf);
    if (d->first_q == -1) {
        d->first_q = q;
        snprintf(d->first_leaf, sizeof(d->first_leaf), "%s", leaf);
    }
}

/* Compares recorded truth `r` against reconstructed/replayed world `g`.
 * hidden=1 audits STRICT state uniqueness (every leaf, dead-slot floats
 * included). hidden=0 compares the observable effect trace: alive particles,
 * counters, trail points/rendered widths and field bodies; dead slot contents
 * are don't-care because em_spawn overwrites a slot before reuse. */
static void diff_world(const World *r, const World *g,
                       const Params *rq, const Params *gq,
                       Diff *d, int hidden) {
    memset(d, 0, sizeof(*d));
    d->first_q = -1;
    char leaf[64];

    if (hidden && neqf(r->em.rate_acc, g->em.rate_acc))
        diff_add(d, Q_RACC, "em.rate_acc");
    if (r->em.n != g->em.n)
        diff_add(d, Q_N, "em.n");
    if (r->em.emitted != g->em.emitted)
        diff_add(d, Q_EMIT, "em.emitted");
    if (r->em.dropped != g->em.dropped)
        diff_add(d, Q_DROP, "em.dropped");

    int mn = r->em.n > g->em.n ? r->em.n : g->em.n;
    if (mn > MAXP) mn = MAXP;
    for (int i = 0; i < mn; i++) {
        const P *a = &r->em.p[i], *b = &g->em.p[i];
        if (a->alive != b->alive) {
            snprintf(leaf, sizeof(leaf), "em.p[%d].alive", i);
            diff_add(d, Q_PALIVE, leaf);
        }
        int both_alive = a->alive && b->alive;
        if (both_alive || hidden) {
            if (neqf(a->x, b->x))  { snprintf(leaf, sizeof(leaf), "em.p[%d].x", i);    diff_add(d, Q_PX, leaf); }
            if (neqf(a->y, b->y))  { snprintf(leaf, sizeof(leaf), "em.p[%d].y", i);    diff_add(d, Q_PY, leaf); }
            if (neqf(a->vx, b->vx)){ snprintf(leaf, sizeof(leaf), "em.p[%d].vx", i);   diff_add(d, Q_PVX, leaf); }
            if (neqf(a->vy, b->vy)){ snprintf(leaf, sizeof(leaf), "em.p[%d].vy", i);   diff_add(d, Q_PVY, leaf); }
            if (neqf(a->age, b->age))  { snprintf(leaf, sizeof(leaf), "em.p[%d].age", i);  diff_add(d, Q_PAGE, leaf); }
            if (neqf(a->life, b->life)){ snprintf(leaf, sizeof(leaf), "em.p[%d].life", i); diff_add(d, Q_PLIFE, leaf); }
        }
    }

    if (r->tr.n != g->tr.n)
        diff_add(d, Q_TN, "tr.n");
    int mt = r->tr.n > g->tr.n ? r->tr.n : g->tr.n;
    if (mt > MAXT) mt = MAXT;
    for (int i = 0; i < mt; i++) {
        const TP *a = &r->tr.p[i], *b = &g->tr.p[i];
        if (neqf(a->x, b->x))   { snprintf(leaf, sizeof(leaf), "tr.p[%d].x", i);   diff_add(d, Q_TX, leaf); }
        if (neqf(a->y, b->y))   { snprintf(leaf, sizeof(leaf), "tr.p[%d].y", i);   diff_add(d, Q_TY, leaf); }
        if (neqf(a->age, b->age)){ snprintf(leaf, sizeof(leaf), "tr.p[%d].age", i); diff_add(d, Q_TAGE, leaf); }
    }
    int mw = r->tr.n < g->tr.n ? r->tr.n : g->tr.n;
    for (int i = 0; i < mw; i++) {
        float wr = tr_width(&r->tr, i, rq->width_base, rq->max_age);
        float wg = tr_width(&g->tr, i, gq->width_base, gq->max_age);
        if (wr >= 0.0f && wg >= 0.0f && neqf(wr, wg)) {
            snprintf(leaf, sizeof(leaf), "tr.width[%d]", i);
            diff_add(d, Q_TWIDTH, leaf);
        }
    }

    for (int i = 0; i < NFIELD; i++) {
        const Part *a = &r->fp[i], *b = &g->fp[i];
        if (neqf(a->x, b->x))  { snprintf(leaf, sizeof(leaf), "fp[%d].x", i);  diff_add(d, Q_FX, leaf); }
        if (neqf(a->y, b->y))  { snprintf(leaf, sizeof(leaf), "fp[%d].y", i);  diff_add(d, Q_FY, leaf); }
        if (neqf(a->vx, b->vx)){ snprintf(leaf, sizeof(leaf), "fp[%d].vx", i); diff_add(d, Q_FVX, leaf); }
        if (neqf(a->vy, b->vy)){ snprintf(leaf, sizeof(leaf), "fp[%d].vy", i); diff_add(d, Q_FVY, leaf); }
    }
}

/* Parses + replays one archive against its truth trace.
 * qoverride is used by the parameter-drift probe. `boundary_out` receives the
 * S0 actually compared at trace frame 0 (after origin-archive pre-roll). */
static int eval_archive(const uint8_t *bytes, size_t len, const Clip *c,
                        const Params *qoverride,
                        int *frames_pass, int *first_frame,
                        Diff *first_diff, World *boundary_out) {
    int id, kind; uint32_t mask;
    Params aq; FIn in[WARMMAX + NFRAME]; World w;
    int nall = 0, pre = 0;
    if (!read_archive(bytes, len, &id, &kind, &mask, &aq, in, &nall, &pre, &w)
        || id != c->id)
        return -1;
    const Params *uq = qoverride ? qoverride : &aq;

    /* origin archive: derive the cut boundary by re-simulating pre-history */
    for (int h = 0; h < pre; h++) world_frame(&w, &in[h], uq);
    if (boundary_out) *boundary_out = w;

    int fp = 0, ff = -1;
    Diff fd; memset(&fd, 0, sizeof(fd)); fd.first_q = -1;
    for (int f = 0; f <= NFRAME; f++) {
        Diff d;
        diff_world(&truth[c->id][f], &w, &c->q, uq, &d, 0);
        if (d.first_q == -1) {
            fp++;
        } else if (ff == -1) {
            ff = f; fd = d;
        }
        if (f < NFRAME) world_frame(&w, &in[pre + f], uq);
    }
    if (frames_pass) *frames_pass = fp;
    if (first_frame) *first_frame = ff;
    if (first_diff && ff != -1) *first_diff = fd;
    return ff == -1 ? 1 : 0;
}

/* ============================== reporting ============================== */

static void ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "mkdir %s: %s\n", path, strerror(errno));
}

typedef struct {
    const char *key, *label;
    int kind; uint32_t mask;
} TierDef;

/* Diagnostic cheap classifications: expected to diverge, used to NAME the
 * quantities that fail the uniqueness criterion. */
static const TierDef PROBES[2] = {
    { "probe_params_only", "naive cut tier: params only, canonical zero S0",
      KIND_MASK, 0 },
    { "probe_scalars", "naive cut tier: params + scalar hidden state",
      KIND_MASK, G_RACC | G_N | G_CTR },
};

/* The three shippable archive-size boundaries. Each is a SUFFICIENT answer to
 * the uniqueness criterion, so each must replay at 100%. */
static const TierDef TIERS[3] = {
    { "S_origin",  "S small:  params + full input stream from canonical zero",
      KIND_ORIGIN, 0 },
    { "M_compact", "M medium: params + compact active boundary state (MASK_STAR)",
      KIND_MASK, MASK_STAR },
    { "L_full",    "L large:  full fixed-capacity leaf snapshot",
      KIND_FULL, 0 },
};

typedef struct {
    size_t total;
    int pass, frames_pass;
    long audit_clips[Q_NQ], audit_hits[Q_NQ], sym_clips[Q_NQ];
    char audit_ex[Q_NQ][64];
} TierStat;

static TierStat execute_tier(const TierDef *td, Buf *arc, const char *packpath) {
    TierStat st;
    memset(&st, 0, sizeof(st));
    FILE *pack = packpath ? fopen(packpath, "wb") : NULL;

    for (int i = 0; i < NCLIPS; i++) {
        size_t n = write_archive(arc, &clips[i], td->kind, td->mask);
        st.total += n;
        if (pack) fwrite(arc->d, 1, n, pack);

        int fp = 0, ff = -1; Diff fd; World bnd;
        int ok = eval_archive(arc->d, n, &clips[i], NULL, &fp, &ff, &fd, &bnd);
        if (ok == 1) st.pass++;
        st.frames_pass += fp;
        if (ok == 0 && fd.first_q >= 0) st.sym_clips[fd.first_q]++;

        /* strict uniqueness audit against the compared boundary state
         * (post pre-roll for origin archives) */
        Diff ad;
        diff_world(&truth[i][0], &bnd, &clips[i].q, &clips[i].q, &ad, 1);
        for (int q = 0; q < Q_NQ; q++) {
            if (ad.cnt[q] > 0) {
                st.audit_clips[q]++;
                st.audit_hits[q] += ad.cnt[q];
                if (st.audit_ex[q][0] == 0)
                    snprintf(st.audit_ex[q], sizeof(st.audit_ex[q]), "%s", ad.ex[q]);
            }
        }
    }
    if (pack) fclose(pack);
    return st;
}

static void print_culprits(const TierDef *td, const TierStat *st, FILE *cc) {
    if (st->pass < NCLIPS) {
        printf("  non-unique quantities at S0 (criterion audit):\n");
        for (int q = 0; q < Q_NQ; q++) if (st->audit_clips[q]) {
            printf("    - %-14s in %2ld/%d clips, %4ld leaves e.g. %s\n",
                   QNAME[q], st->audit_clips[q], NCLIPS, st->audit_hits[q], st->audit_ex[q]);
            if (cc) fprintf(cc, "%s,t0_undetermined,%s,%ld,%ld,%s\n",
                            td->key, QNAME[q], st->audit_clips[q],
                            st->audit_hits[q], st->audit_ex[q]);
        }
        printf("  first replay-divergence symptom:\n");
        for (int q = 0; q < Q_NQ; q++) if (st->sym_clips[q]) {
            printf("    * %-14s first-diverges in %2ld/%d failing clips\n",
                   QNAME[q], st->sym_clips[q], NCLIPS - st->pass);
            if (cc) fprintf(cc, "%s,first_symptom,%s,%ld,0,-\n",
                            td->key, QNAME[q], st->sym_clips[q]);
        }
    } else {
        printf("  S0 uniquely determined: yes (all leaves exact), zero divergence.\n");
    }
}

int main(int argc, char **argv) {
    const char *outdir = argc > 1 ? argv[1] : "artifacts";
    char path[1024];
    ensure_dir(outdir);
    snprintf(path, sizeof(path), "%s/archives", outdir);
    ensure_dir(path);

    for (int i = 0; i < NCLIPS; i++) make_clip(&clips[i], i);

    Buf arc; buf_init(&arc);

    /* ---------- sanity: full archive must be 100% before any claim ---------- */
    for (int i = 0; i < NCLIPS; i++) {
        size_t n = write_archive(&arc, &clips[i], KIND_FULL, 0);
        int ok = eval_archive(arc.d, n, &clips[i], NULL, NULL, NULL, NULL, NULL);
        if (ok != 1) { fprintf(stderr, "FATAL: full archive failed clip %d\n", i); return 3; }
    }

    /* ===================== 0. parameter-drift probe ===================== */
    enum { PP_RATE = 0, PP_GX, PP_GY, PP_DRAG, PP_SWIRL, PP_MAXAGE, PP_BASE, PP_N };
    static const char *PPNAME[PP_N] = {
        "rate", "gx", "gy", "drag", "swirl", "max_age", "width_base"
    };
    const float changed[PP_N] = { 26.0f, 6.0f, 3.5f, 9.0f, -22.0f, 0.22f, 9.0f };

    FILE *dc = NULL;
    snprintf(path, sizeof(path), "%s/drift.csv", outdir);
    dc = fopen(path, "w");
    if (dc) fprintf(dc, "parameter,changed_to,clips_pass,clip_rate_pct,first_frame,symptom\n");

    printf("== experiment 0: old archive left parameters to current panel ==\n");
    printf("single-variable drift from recorded value; FULL state archived.\n");
    /* -1 = pinned sanity, -2 = every parameter unresolved, 0..PP_N-1 = one drift */
    int cases[2 + PP_N];
    cases[0] = -1; cases[1] = -2;
    for (int pp = 0; pp < PP_N; pp++) cases[2 + pp] = pp;
    for (int ci = 0; ci < 2 + PP_N; ci++) {
        int pp = cases[ci];
        int pass = 0, first_frame_all = -1;
        int sym_q = -1; char sym_leaf[64] = "-";
        for (int i = 0; i < NCLIPS; i++) {
            size_t n = write_archive(&arc, &clips[i], KIND_FULL, 0);
            Params dq = clips[i].q;
            if (pp >= 0) {
                switch (pp) {
                    case PP_RATE: dq.rate = changed[pp]; break;
                    case PP_GX:   dq.gx = changed[pp]; break;
                    case PP_GY:   dq.gy = changed[pp]; break;
                    case PP_DRAG: dq.drag = changed[pp]; break;
                    case PP_SWIRL:dq.swirl = changed[pp]; break;
                    case PP_MAXAGE:  dq.max_age = changed[pp]; break;
                    case PP_BASE: dq.width_base = changed[pp]; break;
                }
            } else if (pp == -2) {
                dq.rate = changed[PP_RATE]; dq.gx = changed[PP_GX];
                dq.gy = changed[PP_GY]; dq.drag = changed[PP_DRAG];
                dq.swirl = changed[PP_SWIRL]; dq.max_age = changed[PP_MAXAGE];
                dq.width_base = changed[PP_BASE];
            }
            const Params *ov = (pp == -1) ? NULL : &dq;
            int ff; Diff fd;
            int ok = eval_archive(arc.d, n, &clips[i], ov, NULL, &ff, &fd, NULL);
            if (ok == 1) pass++;
            else if (first_frame_all == -1 || ff < first_frame_all) {
                first_frame_all = ff; sym_q = fd.first_q;
                snprintf(sym_leaf, sizeof(sym_leaf), "%s", fd.first_leaf);
            }
        }
        const char *pname = (pp == -2) ? "ALL_UNPINNED" : (pp == -1 ? "(pinned sanity)" : PPNAME[pp]);
        printf("  %-16s pass=%2d/%d (%5.1f%%)", pname, pass, NCLIPS, 100.0 * pass / NCLIPS);
        if (pass < NCLIPS && sym_q >= 0)
            printf("  earliest f=%d symptom=%s (%s)", first_frame_all, QNAME[sym_q], sym_leaf);
        printf("\n");
        if (dc && pp >= 0)
            fprintf(dc, "%s,%g,%d,%.1f,%d,%s\n", PPNAME[pp], changed[pp], pass,
                    100.0 * pass / NCLIPS, first_frame_all, sym_q >= 0 ? QNAME[sym_q] : "-");
        if (dc && pp == -2)
            fprintf(dc, "ALL_UNPINNED,multiple,%d,%.1f,%d,%s\n", pass,
                    100.0 * pass / NCLIPS, first_frame_all, sym_q >= 0 ? QNAME[sym_q] : "-");
    }
    if (dc) fclose(dc);

    /* ============== 1a. diagnostic probes (expected to diverge) ============== */
    FILE *cc = NULL;
    snprintf(path, sizeof(path), "%s/culprits.csv", outdir);
    cc = fopen(path, "w");
    if (cc) fprintf(cc, "tier,kind,quantity,clips,leaf_hits,example\n");

    printf("\n== experiment 1a: naive cheap classifications (culprit naming) ==\n");
    for (int t = 0; t < 2; t++) {
        const TierDef *td = &PROBES[t];
        char packpath[1100];
        snprintf(packpath, sizeof(packpath), "%s/archives/%s.vfxpack", outdir, td->key);
        TierStat st = execute_tier(td, &arc, packpath);
        int total_frames = NCLIPS * (NFRAME + 1);
        printf("\n[%s] %s\n", td->key, td->label);
        printf("  total=%zu B  avg=%.1f B/clip\n", st.total, (double)st.total / NCLIPS);
        printf("  replay consistency: clips %d/%d = %.1f%% ; frames %.1f%%\n",
               st.pass, NCLIPS, 100.0 * st.pass / NCLIPS,
               100.0 * st.frames_pass / total_frames);
        print_culprits(td, &st, cc);
    }

    /* ============== 1b. the three shippable archive-size tiers ============== */
    FILE *tc = NULL;
    snprintf(path, sizeof(path), "%s/tiers.csv", outdir);
    tc = fopen(path, "w");
    if (tc) fprintf(tc, "tier,kind,mask,bytes_total,bytes_avg,clips_pass,clip_rate_pct,frame_rate_pct\n");

    printf("\n== experiment 1b: three archive-size tiers, one pass each; gate=100%% ==\n");
    int gate_ok = 1;
    for (int t = 0; t < 3; t++) {
        const TierDef *td = &TIERS[t];
        char packpath[1100];
        snprintf(packpath, sizeof(packpath), "%s/archives/tier_%s.vfxpack", outdir, td->key);
        TierStat st = execute_tier(td, &arc, packpath);
        int total_frames = NCLIPS * (NFRAME + 1);
        printf("\n[tier %s] %s\n", td->key, td->label);
        printf("  archive=%s\n", packpath);
        printf("  total=%zu B  avg=%.1f B/clip\n", st.total, (double)st.total / NCLIPS);
        printf("  replay consistency: clips %d/%d = %.1f%% ; frames %.2f%%\n",
               st.pass, NCLIPS, 100.0 * st.pass / NCLIPS,
               100.0 * st.frames_pass / total_frames);
        if (tc)
            fprintf(tc, "%s,%s,%u,%zu,%.2f,%d,%.1f,%.2f\n",
                    td->key,
                    td->kind == KIND_FULL ? "full" :
                    td->kind == KIND_ORIGIN ? "origin" : "mask",
                    td->mask, st.total, (double)st.total / NCLIPS, st.pass,
                    100.0 * st.pass / NCLIPS, 100.0 * st.frames_pass / total_frames);
        print_culprits(td, &st, cc);
        if (st.pass != NCLIPS) gate_ok = 0;
    }
    if (tc) fclose(tc);
    if (cc) fclose(cc);
    if (!gate_ok) {
        fprintf(stderr, "FATAL: a shippable tier is below 100%% consistency\n");
        return 4;
    }

    /* ===================== 2. exhaustive boundary sweep ===================== */
    snprintf(path, sizeof(path), "%s/sweep.csv", outdir);
    FILE *sw = fopen(path, "w");
    if (sw) fprintf(sw, "mask,groups,bytes_total,bytes_avg,clips_pass,clip_rate_pct,frame_rate_pct\n");

    size_t best_bytes = (size_t)-1;
    uint32_t best_mask = 0;
    printf("\n== experiment 2: 64 classification boundaries (state-group subsets) ==\n");
    for (uint32_t mask = 0; mask < 64; mask++) {
        size_t total = 0; int pass = 0, frames_pass = 0;
        for (int i = 0; i < NCLIPS; i++) {
            size_t n = write_archive(&arc, &clips[i], KIND_MASK, mask);
            total += n;
            int fp = 0;
            int ok = eval_archive(arc.d, n, &clips[i], NULL, &fp, NULL, NULL, NULL);
            if (ok == 1) pass++;
            frames_pass += fp;
        }
        if (sw) {
            char groups[128]; groups[0] = 0;
            const char *gn[6] = { "racc", "n", "ctr", "slot", "trail", "field" };
            for (int g = 0; g < 6; g++) if (mask & (1u << g)) {
                if (groups[0]) strncat(groups, "|", sizeof(groups) - strlen(groups) - 1);
                strncat(groups, gn[g], sizeof(groups) - strlen(groups) - 1);
            }
            if (!groups[0]) snprintf(groups, sizeof(groups), "-");
            fprintf(sw, "%u,%s,%zu,%.2f,%d,%.1f,%.2f\n", mask, groups, total,
                    (double)total / NCLIPS, pass, 100.0 * pass / NCLIPS,
                    100.0 * frames_pass / (NCLIPS * (NFRAME + 1)));
        }
        if (pass == NCLIPS && total < best_bytes) {
            best_bytes = total; best_mask = mask;
        }
    }
    if (sw) fclose(sw);
    printf("cheapest corpus-verified 100%% boundary: mask=0x%02x total=%zu B avg=%.1f B/clip\n",
           best_mask, best_bytes, best_bytes / (double)NCLIPS);
    printf("criterion boundary MASK_STAR=0x%02x (racc+ctr+slot+trail+field, n implied by slot)\n",
           MASK_STAR);

    /* explicit one-shot verification of MASK_STAR */
    {
        size_t total = 0; int pass = 0;
        for (int i = 0; i < NCLIPS; i++) {
            size_t n = write_archive(&arc, &clips[i], KIND_MASK, MASK_STAR);
            total += n;
            int ok = eval_archive(arc.d, n, &clips[i], NULL, NULL, NULL, NULL, NULL);
            if (ok == 1) pass++;
        }
        printf("MASK_STAR verification: %d/%d clips = %.1f%%, total=%zu B avg=%.1f B/clip\n",
               pass, NCLIPS, 100.0 * pass / NCLIPS, total, total / (double)NCLIPS);
    }

    /* ============== 3. leave-one-group-out necessity proof ============== */
    printf("\n== experiment 3: leave-one-promoted-group-out (full compact base) ==\n");
    const uint32_t LOG[5] = { G_RACC, G_CTR, G_SLOT, G_TRL, G_FLD };
    const char *LOGN[5]  = { "em.rate_acc", "em.emitted/dropped",
                             "em.p[](slots)", "tr.p[](trail)", "fp[](field)" };
    for (int g = 0; g < 5; g++) {
        uint32_t mask = MASK_STAR & ~LOG[g];
        int pass = 0, worst_ff = -1, sym_q = -1; char leaf[64] = "-";
        for (int i = 0; i < NCLIPS; i++) {
            size_t n = write_archive(&arc, &clips[i], KIND_MASK, mask);
            int ff; Diff fd;
            int ok = eval_archive(arc.d, n, &clips[i], NULL, NULL, &ff, &fd, NULL);
            if (ok == 1) pass++;
            else if (worst_ff == -1 || ff < worst_ff) {
                worst_ff = ff; sym_q = fd.first_q;
                snprintf(leaf, sizeof(leaf), "%s", fd.first_leaf);
            }
        }
        printf("  omit %-20s -> %2d/%d (%5.1f%%)", LOGN[g], pass, NCLIPS,
               100.0 * pass / NCLIPS);
        if (pass < NCLIPS && sym_q >= 0)
            printf("  earliest f=%d %s (%s)", worst_ff, QNAME[sym_q], leaf);
        printf("\n");
    }

    buf_free(&arc);
    printf("\nartifacts written under %s\n", outdir);
    return 0;
}
