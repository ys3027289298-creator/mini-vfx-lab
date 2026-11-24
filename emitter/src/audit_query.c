
/* audit_query: replay one explosion under contribution eviction and let a
 * particle id be looked up by number. Usage:
 *   ./audit_query            # built-in sample lookups + loss summary
 *   ./audit_query 17 84      # look up specific particle ids
 * Every eviction/rejection is stored in the emitter audit ring; a lookup
 * that finds nothing means the particle was never lost (still alive or
 * expired naturally), i.e. no eviction reason can exist for it.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "emitter.h"

#define NREQ 160
#define NFRAMES 60
#define DT 0.033f

static unsigned long rng_state;
static float rng_f(void) {
    unsigned long x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return (float)(x >> 11) / (float)(1UL << 53);
}
static float rng_range(float lo, float hi) { return lo + rng_f() * (hi - lo); }

static void print_lookup(const Emitter* e, int id) {
    const Audit* a = em_audit_find(e, id);
    if (!a) {
        printf("particle %4d: no loss record -> not evicted/rejected "
               "(natural expiry or still alive); no reason to report\n", id);
        return;
    }
    printf("particle %4d: frame=%d kind=%s reason=%s "
           "victim=%d incoming=%d factors(life=%.3f area=%.3f bright=%.3f) "
           "scores(victim=%.4f incoming=%.4f) level=L%d\n",
           id, a->frame, em_audit_kind_str(a->kind), em_reason_str(a->reason),
           a->victim_id, a->incoming_id, a->fl, a->fa, a->fb,
           a->victim_score, a->incoming_score, a->level);
    if (a->kind == AUDIT_EVICT) {
        int again = em_reason_from_factors(a->level, a->fl, a->fa, a->fb);
        printf("             reason re-derived from logged factors: %s "
               "(match=%d)\n", em_reason_str(again), again == a->reason);
    }
}

int main(int argc, char** argv) {
    Emitter e;
    em_init(&e, POLICY_EVICT, 3);

    /* identical deterministic explosion as bench.c */
    rng_state = 0x9e3779b97f4a7c15UL;
    for (int i = 0; i < NREQ; i++) {
        float ang = rng_range(0.0f, 6.2832f);
        float spd = rng_range(1.0f, 9.0f);
        em_spawn(&e, rng_range(-0.5f, 0.5f), rng_range(-0.5f, 0.5f),
                 cosf(ang) * spd, sinf(ang) * spd,
                 rng_range(0.8f, 6.0f), rng_range(0.2f, 1.5f),
                 rng_range(0.15f, 1.0f));
    }
    for (int fr = 0; fr < NFRAMES; fr++) em_update(&e, DT, 0, 0);

    printf("loss summary: evicted=%d rejected=%d audit_rows=%d "
           "overflow=%d\n\n", e.evicted, e.rejected, e.nlog, e.log_overflow);

    if (argc > 1) {
        for (int i = 1; i < argc; i++) print_lookup(&e, atoi(argv[i]));
        return 0;
    }

    /* sample: first three evicted ids, first two rejected ids, and one id
     * that was never lost (proves a negative lookup is meaningful) */
    int printed_ev = 0, printed_rj = 0;
    for (int i = 0; i < e.nlog && (printed_ev < 3 || printed_rj < 2); i++) {
        if (e.log[i].kind == AUDIT_EVICT && printed_ev < 3) {
            print_lookup(&e, e.log[i].victim_id);
            printed_ev++;
        } else if (e.log[i].kind == AUDIT_REJECT && printed_rj < 2) {
            print_lookup(&e, e.log[i].incoming_id);
            printed_rj++;
        }
    }
    int alive_id = -1;
    for (int i = 0; i < e.n; i++)
        if (e.p[i].alive) { alive_id = e.p[i].id; break; }
    if (alive_id >= 0) print_lookup(&e, alive_id);

    /* exhaustive check: every row is queryable by both relevant ids */
    int miss = 0;
    for (int i = 0; i < e.nlog; i++) {
        if (!em_audit_find(&e, e.log[i].incoming_id)) miss++;
        if (e.log[i].victim_id >= 0 && !em_audit_find(&e, e.log[i].victim_id))
            miss++;
    }
    printf("\nexhaustive lookup check: misses=%d (must be 0)\n", miss);
    return miss == 0 ? 0 : 1;
}
