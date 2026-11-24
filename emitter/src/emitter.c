
/* emitter: rate + burst emission with lifetime expiry and capacity handling.
 *
 * Pool overflow policies:
 *   POLICY_DROP  - legacy: a newcomer arriving at a full pool is dropped.
 *   POLICY_EVICT - every pool member is scored by a 3-term contribution model
 *                  (lifetime / on-screen footprint / brightness); the lowest
 *                  score loses. If the newcomer itself scores lowest it is
 *                  refused instead. Scoring runs at one of NLEVELS precision
 *                  levels, each with its own approximation budget and term
 *                  weights (inverse-variance derived from quantization bins).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "emitter.h"

/* ---------- precision levels ------------------------------------------------
 * bins[] = quantization resolution per term (life, area, bright).
 *   bins <= 1  -> term disabled (no resolution at this precision)
 *   bins < 256 -> term quantized into that many bins
 *   bins == 256-> exact float, no quantization
 *
 * Approximation budget also varies by level:
 *   L0/L1: Manhattan radius, no sqrt; linear brightness envelope
 *   L2   : Newton-iteration inverse sqrt; quadratic envelopes
 *   L3   : hardware sqrtf; sqrt envelope
 *   L4   : sqrtf + powf, eager per-frame rescoring and full sort on eviction
 */
typedef struct {
    const char* name;
    int bins[3];
} LevelCfg;

static const LevelCfg LEVELS[NLEVELS] = {
    { "L0_life_only_coarse", {   2,   0,   0 } },
    { "L1_coarse",           {   4,   2,   2 } },
    { "L2_medium",           {   8,   8,   4 } },
    { "L3_fine",             {  16,  16,  16 } },
    { "L4_exact_sorted",     { 256, 256, 256 } },
};

void em_level_config(int level, int bins[3], float weights[3], const char** name) {
    if (level < 0 || level >= NLEVELS) {
        if (bins) memset(bins, 0, 3 * sizeof(int));
        if (weights) memset(weights, 0, 3 * sizeof(float));
        if (name) *name = "invalid";
        return;
    }
    const LevelCfg* c = &LEVELS[level];
    if (name) *name = c->name;
    /* inverse-variance: uniform quantization noise variance ~ 1/B^2,
     * so the inverse-variance weight of term k is B_k^2 / sum(B_j^2).
     * Disabled terms (B <= 1) get zero weight. */
    double acc = 0.0;
    for (int k = 0; k < 3; k++) {
        if (bins) bins[k] = c->bins[k];
        double b = c->bins[k] > 1 ? (double)c->bins[k] : 0.0;
        if (weights) weights[k] = (float)(b * b);
        acc += b * b;
    }
    if (weights) {
        for (int k = 0; k < 3; k++)
            weights[k] = acc > 0.0 ? weights[k] / (float)acc : 0.0f;
    }
}

/* ---------- raw factors (precision independent, used for audit) ------------ */

static float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

void em_factors(const P* p, float f[3]) {
    float fl = clamp01(1.0f - p->age / p->life);

    /* hardware-precision raw radius; cheap levels approximate their own. */
    float r = sqrtf(p->x * p->x + p->y * p->y);
    float d = clamp01(r / (2.0f * VIEW_R));
    float edge = 1.0f - d * d;
    float fa = clamp01((p->size * p->size * edge) / 2.25f);

    /* brightness envelope: full at birth, dimming with age (sqrt curve);
     * never zero at spawn so the term participates in first-burst eviction */
    float env = sqrtf(fl);
    float fb = clamp01(p->bright * env);

    f[0] = fl; f[1] = fa; f[2] = fb;
}

/* ---------- level-specific cheap math -------------------------------------- */

static float rsqrt_newton(float x) {
    /* classic bit-hack seed + three Newton iterations, no hardware sqrt */
    uint32_t bits;
    memcpy(&bits, &x, sizeof(bits));
    bits = 0x5f3759dfu - (bits >> 1);
    float y;
    memcpy(&y, &bits, sizeof(y));
    y *= (1.5f - 0.5f * x * y * y);
    y *= (1.5f - 0.5f * x * y * y);
    y *= (1.5f - 0.5f * x * y * y);
    return y;
}

static float quantize(float x, int bins) {
    if (bins >= 256) return x;                 /* exact level */
    float q = ((floorf(x * bins) + 0.5f) / (float)bins);
    return q < 0.0f ? 0.0f : (q >= 1.0f ? 1.0f - 1.0f / bins : q);
}

/* composite contribution score at a given precision.
 * When math_level >= 0, the geometric/envelope approximation of that level
 * is reused while only the caller's level decides the quantization bins;
 * this isolates quantization error from model-approximation error. */
float em_score_at(const P* p, int level, int math_level) {
    int bins[3]; float w[3];
    em_level_config(level, bins, w, 0);
    int ml = math_level >= 0 ? math_level : level;

    float fl = clamp01(1.0f - p->age / p->life);

    float r2 = p->x * p->x + p->y * p->y;
    float r;
    if (ml <= 1) {
        r = (fabsf(p->x) + fabsf(p->y)) * 0.75f;   /* Manhattan, no sqrt */
    } else if (ml == 2) {
        r = r2 * rsqrt_newton(r2 + 1e-12f);        /* Newton inverse sqrt */
    } else {
        r = sqrtf(r2);
    }
    float d = clamp01(r / (2.0f * VIEW_R));

    float edge;
    if (ml <= 1) edge = 1.0f - d;                  /* linear falloff */
    else if (ml <= 3) edge = 1.0f - d * d;        /* quadratic */
    else edge = powf(1.0f - d, 1.5f);             /* gamma, powf cost */

    float fa_raw = clamp01((p->size * p->size * edge) / 2.25f);

    float env;
    if (ml <= 1) env = fl;                        /* linear dim with age */
    else if (ml == 2) env = fl * fl;             /* quadratic */
    else if (ml == 3) env = sqrtf(fl);
    else env = powf(fl, 0.7f);
    float fb_raw = clamp01(p->bright * env);

    float vals[3] = { fl, fa_raw, fb_raw };
    float s = 0.0f;
    for (int k = 0; k < 3; k++) {
        if (bins[k] <= 1) continue;               /* disabled term */
        float v = level >= 4 ? vals[k] : quantize(vals[k], bins[k]);
        s += w[k] * v;
    }
    return s;
}

float em_score(const P* p, int level) {
    return em_score_at(p, level, -1);
}

/* Quantization-only probe: identical three-term equal-weight model at every
 * level, L4's geometric math, only the bin counts vary. Used to measure the
 * precision/error curve in isolation from weight and model changes. */
float em_quant_probe(const P* p, int level) {
    static const int qbins[NLEVELS][3] = {
        { 2, 2, 2 }, { 4, 4, 4 }, { 8, 8, 8 }, { 16, 16, 16 }, { 256, 256, 256 }
    };
    float fl = clamp01(1.0f - p->age / p->life);
    float r2 = p->x * p->x + p->y * p->y;
    float r = sqrtf(r2);
    float d = clamp01(r / (2.0f * VIEW_R));
    float edge = 1.0f - d * d;
    float fa = clamp01((p->size * p->size * edge) / 2.25f);
    float fb = clamp01(p->bright * sqrtf(fl));
    float vals[3] = { fl, fa, fb };
    float s = 0.0f;
    for (int k = 0; k < 3; k++) {
        int b = qbins[level][k];
        float v = b >= 256 ? vals[k] : quantize(vals[k], b);
        s += v / 3.0f;
    }
    return s;
}

/* reference: double precision, equal weights, no quantization */
double em_ref_score(const P* p) {
    double fl = (double)clamp01(1.0f - p->age / p->life);
    double r = sqrt((double)p->x * p->x + (double)p->y * p->y);
    double d = fmin(1.0, fmax(0.0, r / (2.0 * (double)VIEW_R)));
    double edge = 1.0 - d * d;
    double fa = fmin(1.0, fmax(0.0, (p->size * p->size * edge) / 2.25));
    double env = sqrt(fl);
    double fb = fmin(1.0, fmax(0.0, (double)p->bright * env));
    return (fl + fa + fb) / 3.0;
}

/* ---------- audit ----------------------------------------------------------- */

static int classify_reason(int level, float fl, float fa, float fb) {
    int bins[3]; float w[3];
    em_level_config(level, bins, w, 0);
    float v[3] = { fl, fa, fb };
    /* weakest active term; disabled ones cannot be the reason */
    int pick = -1;
    for (int k = 0; k < 3; k++) {
        if (bins[k] <= 1) continue;
        if (pick < 0 || v[k] < v[pick]) pick = k;
    }
    if (pick < 0) return REASON_COMPOSITE;
    for (int k = 0; k < 3; k++) {
        if (k == pick || bins[k] <= 1) continue;
        /* tie within 10% of the range -> no single dominant weakness */
        if (fabsf(v[k] - v[pick]) <= 0.1f) return REASON_COMPOSITE;
    }
    static const int map[3] = {
        REASON_LOW_LIFE, REASON_SMALL_FOOTPRINT, REASON_DIM
    };
    return map[pick];
}

int em_reason_from_factors(int level, float fl, float fa, float fb) {
    return classify_reason(level, fl, fa, fb);
}

const char* em_reason_str(int reason) {
    switch (reason) {
        case REASON_LOW_LIFE:        return "low_remaining_lifetime";
        case REASON_SMALL_FOOTPRINT: return "small_screen_footprint";
        case REASON_DIM:             return "dim_brightness";
        case REASON_COMPOSITE:       return "composite_low_contribution";
        case REASON_NEWCOMER_LOWER:  return "newcomer_score_below_pool_min";
        default:                     return "pool_full_drop";
    }
}

const char* em_audit_kind_str(int kind) {
    return kind == AUDIT_EVICT ? "evict" : "reject";
}

static void audit_push(Emitter* e, const Audit* a) {
    if (e->nlog < AUDIT_CAP) {
        e->log[e->nlog++] = *a;
    } else {
        e->log_overflow++;
    }
}

const Audit* em_audit_find(const Emitter* e, int particle_id) {
    for (int i = 0; i < e->nlog; i++) {
        if (e->log[i].victim_id == particle_id ||
            e->log[i].incoming_id == particle_id)
            return &e->log[i];
    }
    return 0;
}

void em_audit_print(const Emitter* e) {
    for (int i = 0; i < e->nlog; i++) {
        const Audit* a = &e->log[i];
        printf("frame=%d %s reason=%s victim=%d incoming=%d "
               "factors(life=%.3f area=%.3f bright=%.3f) "
               "scores(victim=%.4f incoming=%.4f) level=L%d\n",
               a->frame, em_audit_kind_str(a->kind), em_reason_str(a->reason),
               a->victim_id, a->incoming_id, a->fl, a->fa, a->fb,
               a->victim_score, a->incoming_score, a->level);
    }
    if (e->log_overflow)
        printf("audit_overflow=%d (raise AUDIT_CAP)\n", e->log_overflow);
}

/* ---------- emitter --------------------------------------------------------- */

void em_init(Emitter* e, Policy policy, int level) {
    memset(e, 0, sizeof(*e));
    e->policy = policy;
    e->level = (level < 0 || level >= NLEVELS) ? NLEVELS - 1 : level;
}

static int next_id(Emitter* e) { return e->id_counter++; }

static int find_dead(const Emitter* e) {
    for (int i = 0; i < e->n; i++)
        if (!e->p[i].alive) return i;
    return -1;
}

static int find_min_score(Emitter* e) {
    int idx = -1;
    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        if (!e->p[i].scored) {
            e->p[i].score = em_score(&e->p[i], e->level);
            e->p[i].scored = 1;
        }
        if (idx < 0 || e->p[i].score < e->p[idx].score) idx = i;
    }
    return idx;
}

static void fill_particle(P* p, int id, float x, float y, float vx, float vy,
                          float life, float size, float bright) {
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->age = 0.0f; p->life = life;
    p->size = size; p->bright = bright;
    p->id = id; p->alive = 1; p->score = 0.0f; p->scored = 0;
}

int em_spawn(Emitter* e, float x, float y, float vx, float vy,
             float life, float size, float bright) {
    if (life <= 0.0f || size < 0.0f) return -1;
    int id = next_id(e);

    int slot = find_dead(e);
    if (slot >= 0) {
        fill_particle(&e->p[slot], id, x, y, vx, vy, life, size, bright);
        e->emitted++;
        return slot;
    }

    if (e->n < MAXP) {
        fill_particle(&e->p[e->n], id, x, y, vx, vy, life, size, bright);
        e->emitted++;
        return e->n++;
    }

    /* pool fully alive: overflow */
    if (e->policy == POLICY_DROP) {
        e->dropped++;
        Audit a = { e->frame, AUDIT_REJECT, REASON_NONE, -1, id,
                    0, 0, 0, 0.0f, 0.0f, e->level };
        audit_push(e, &a);
        return -2;
    }

    P cand;
    fill_particle(&cand, id, x, y, vx, vy, life, size, bright);
    float cand_score = em_score(&cand, e->level);
    int min_i = find_min_score(e);
    P* victim = &e->p[min_i];

    /* ties resolve in favour of the incumbent: only a strictly better
     * newcomer (beyond float noise) earns a slot */
    if (cand_score <= victim->score + 1e-6f) {
        /* newcomer loses: refusal is itself an auditable drop */
        e->rejected++;
        float f[3]; em_factors(&cand, f);
        Audit a = { e->frame, AUDIT_REJECT, REASON_NEWCOMER_LOWER,
                    -1, id, f[0], f[1], f[2],
                    victim->score, cand_score, e->level };
        audit_push(e, &a);
        return -2;
    }

    float f[3]; em_factors(victim, f);
    Audit a = { e->frame, AUDIT_EVICT,
                classify_reason(e->level, f[0], f[1], f[2]),
                victim->id, id, f[0], f[1], f[2],
                victim->score, cand_score, e->level };
    audit_push(e, &a);

    fill_particle(victim, id, x, y, vx, vy, life, size, bright);
    victim->score = cand_score;
    victim->scored = 1;
    e->evicted++;
    e->emitted++;
    return min_i;
}

int em_update(Emitter* e, float dt, float rate, int burst) {
    if (dt < 0.0f) return -1;

    for (int i = 0; i < e->n; i++) {
        if (!e->p[i].alive) continue;
        e->p[i].age += dt;
        if (e->p[i].age >= e->p[i].life) { e->p[i].alive = 0; continue; }
        e->p[i].x += e->p[i].vx * dt;
        e->p[i].y += e->p[i].vy * dt;
        e->p[i].scored = 0;
    }

    /* L4 pays for an eager full-pool rescore every frame; lower levels
     * score lazily, only when an eviction actually happens. */
    if (e->policy == POLICY_EVICT && e->level >= 4) {
        for (int i = 0; i < e->n; i++) {
            if (!e->p[i].alive) continue;
            e->p[i].score = em_score(&e->p[i], e->level);
            e->p[i].scored = 1;
        }
    }

    /* default spawn parameters for rate/burst emission */
    for (int b = 0; b < burst; b++)
        em_spawn(e, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.8f);

    e->rate_acc += rate * dt;
    int want = (int)e->rate_acc;
    e->rate_acc -= (float)want;
    for (int i = 0; i < want; i++)
        em_spawn(e, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0.8f);

    /* compact dead slots to the front so find_dead stays O(1)-ish */
    int w = 0;
    for (int i = 0; i < e->n; i++) {
        if (e->p[i].alive) {
            if (w != i) e->p[w] = e->p[i];
            w++;
        }
    }
    e->n = w;

    e->frame++;
    int alive = 0;
    for (int i = 0; i < e->n; i++) if (e->p[i].alive) alive++;
    return alive;
}

/* ---------- self test ------------------------------------------------------- */

#ifndef EMITTER_NO_MAIN

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    Emitter e; em_init(&e, POLICY_DROP, 0);
    check("spawn_ok", em_spawn(&e, 1, 2, 0, 0, 1.0f, 0.5f, 0.8f) == 0);
    check("zero_life_rejected", em_spawn(&e, 0, 0, 0, 0, 0.0f, 0.5f, 0.8f) == -1);

    Emitter r; em_init(&r, POLICY_DROP, 0);
    int alive = em_update(&r, 1.0f, 10.0f, 0);
    check("rate_count", alive == 10 && r.emitted == 10);

    Emitter b; em_init(&b, POLICY_DROP, 0);
    alive = em_update(&b, 0.0f, 0.0f, 5);
    check("burst_count", alive == 5);

    Emitter x; em_init(&x, POLICY_DROP, 0);
    em_spawn(&x, 0, 0, 0, 0, 0.5f, 0.5f, 0.8f);
    em_update(&x, 1.0f, 0.0f, 0);
    int still = 0;
    for (int i = 0; i < x.n; i++) if (x.p[i].alive) still++;
    check("expired", still == 0);

    Emitter cap; em_init(&cap, POLICY_DROP, 0);
    for (int i = 0; i < MAXP + 5; i++)
        em_spawn(&cap, 0, 0, 0, 0, 10.0f, 0.5f, 0.8f);
    check("capacity_dropped_legacy", cap.dropped == 5 && cap.evicted == 0);

    Emitter mv; em_init(&mv, POLICY_DROP, 0);
    em_spawn(&mv, 0, 0, 2.0f, 0, 1.0f, 0.5f, 0.8f);
    em_update(&mv, 0.5f, 0, 0);
    check("moved", fabsf(mv.p[0].x - 1.0f) < 1e-5f);

    check("bad_dt", em_update(&mv, -1.0f, 0, 0) == -1);

    /* ---- contribution eviction behaviour ---- */

    /* weights are inverse-variance derived and normalized */
    for (int lv = 0; lv < NLEVELS; lv++) {
        int bins[3]; float w[3]; const char* nm;
        em_level_config(lv, bins, w, &nm);
        float sum = w[0] + w[1] + w[2];
        int nonneg = w[0] >= 0 && w[1] >= 0 && w[2] >= 0;
        printf("level=L%d name=%s bins=(%d,%d,%d) weights=(%.3f,%.3f,%.3f)\n",
               lv, nm, bins[0], bins[1], bins[2], w[0], w[1], w[2]);
        check("weights_normalized", fabsf(sum - 1.0f) < 1e-5f && nonneg);
    }

    /* identical particles: newcomer ties, so it is refused, incumbent kept */
    Emitter tie; em_init(&tie, POLICY_EVICT, 2);
    for (int i = 0; i < MAXP; i++)
        em_spawn(&tie, 0, 0, 0, 0, 10.0f, 0.5f, 0.8f);
    int rc = em_spawn(&tie, 0, 0, 0, 0, 10.0f, 0.5f, 0.8f);
    check("tie_keeps_incumbent", rc == -2 && tie.rejected == 1 && tie.evicted == 0);

    /* strong newcomer evicts the weakest incumbent: build a pool where
     * particle 0 is near death (low life term) and everyone else is fresh */
    Emitter ev; em_init(&ev, POLICY_EVICT, 3);
    em_spawn(&ev, 0, 0, 0, 0, 1.0f, 0.5f, 0.8f);   /* id 0: short life */
    for (int i = 0; i < MAXP - 1; i++)
        em_spawn(&ev, 0, 0, 0, 0, 100.0f, 1.0f, 1.0f);
    em_update(&ev, 0.9f, 0, 0);                    /* id 0 nearly expired */
    int weak_id = ev.p[0].id;
    int before = ev.emitted;
    int slot = em_spawn(&ev, 0, 0, 0, 0, 100.0f, 1.0f, 1.0f);
    check("eviction_happened", slot >= 0 && ev.evicted == 1 &&
          ev.emitted == before + 1);
    const Audit* a = em_audit_find(&ev, weak_id);
    check("weak_particle_audited", a && a->kind == AUDIT_EVICT &&
          a->victim_id == weak_id);
    if (a) {
        /* reason must be reproducible from the logged factors */
        int again = em_reason_from_factors(a->level, a->fl, a->fa, a->fb);
        check("reason_reproducible", again == a->reason);
        printf("evicted id=%d reason=%s factors=(%.3f,%.3f,%.3f)\n",
               weak_id, em_reason_str(a->reason), a->fl, a->fa, a->fb);
    }

    /* every removed/refused particle must be traceable: audit count matches */
    Emitter big; em_init(&big, POLICY_EVICT, 2);
    for (int i = 0; i < MAXP * 3; i++)
        em_spawn(&big, (float)(i % 7) - 3.0f, (float)(i % 5) - 2.0f,
                 0, 0, 1.0f + (i % 10), 0.2f + (i % 8) * 0.15f,
                 0.2f + (i % 5) * 0.15f);
    int accounted = big.evicted + big.rejected;
    check("audit_covers_all_losses", big.nlog == accounted &&
          big.log_overflow == 0);

    /* a near-death, tiny, dim newcomer must lose against healthy incumbents;
     * use L3 so all three terms are active */
    Emitter rj; em_init(&rj, POLICY_EVICT, 3);
    for (int i = 0; i < MAXP; i++)
        em_spawn(&rj, 0, 0, 0, 0, 100.0f, 1.0f, 1.0f);
    int rslot = em_spawn(&rj, 11.0f, 11.0f, 0, 0, 0.05f, 0.05f, 0.05f);
    check("weak_newcomer_rejected", rslot == -2 && rj.rejected == 1);
    /* find whatever incoming id the last audit row recorded */
    const Audit* last = &rj.log[rj.nlog - 1];
    const Audit* q = em_audit_find(&rj, last->incoming_id);
    check("reject_queryable", q && q->kind == AUDIT_REJECT &&
          q->reason == REASON_NEWCOMER_LOWER);

    /* legacy policy logs its drops too, with the pool-full reason */
    check("drop_audit_reason", cap.nlog == 5 &&
          cap.log[0].reason == REASON_NONE);

    /* higher precision must rank no worse against the double-precision
     * reference: monotone non-decreasing ranking agreement (checked fully
     * in the precision sweep; here just sanity-bound every level's score) */
    P probe = { 1.0f, 2.0f, 0, 0, 0.3f, 2.0f, 0.8f, 0.9f, 7, 1, 0, 0 };
    for (int lv = 0; lv < NLEVELS; lv++) {
        float s = em_score(&probe, lv);
        check("score_in_range", s >= 0.0f && s <= 1.0f);
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

#endif /* EMITTER_NO_MAIN */
