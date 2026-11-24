
/* emitter: rate + burst emission, lifetime expiry and a fixed-capacity pool.
 *
 * Two overflow policies:
 *   POLICY_DROP  - legacy "whoever arrives when full is dropped"
 *   POLICY_EVICT - contribution-based eviction at a scoring precision level
 *
 * Contribution blends three normalized terms:
 *   life   - remaining lifetime fraction
 *   area   - on-screen footprint (size^2 * edge falloff)
 *   bright - brightness * lifetime fade envelope
 * Term weights are derived from the precision level (inverse-variance,
 * see em_level_config in emitter.c).
 */
#ifndef EMITTER_H
#define EMITTER_H

#define MAXP 64
#define AUDIT_CAP 256
#define NLEVELS 5
#define VIEW_R 12.0f

typedef enum { POLICY_DROP = 0, POLICY_EVICT = 1 } Policy;

enum {
    AUDIT_EVICT = 1,   /* an incumbent was evicted for a newcomer */
    AUDIT_REJECT = 2   /* a newcomer lost the score contest and was dropped */
};

enum {
    REASON_NONE = 0,
    REASON_LOW_LIFE = 1,
    REASON_SMALL_FOOTPRINT = 2,
    REASON_DIM = 3,
    REASON_COMPOSITE = 4,        /* no single term dominates the weakness */
    REASON_NEWCOMER_LOWER = 5    /* reject: newcomer scored below the min incumbent */
};

typedef struct {
    float x, y, vx, vy;
    float age, life;
    float size;     /* base radius in world units */
    float bright;   /* base brightness in [0,1] */
    int id;         /* monotonic spawn-request id */
    int alive;
    float score;    /* cached composite contribution score */
    int scored;     /* lazy-scoring flag */
} P;

typedef struct {
    int frame;
    int kind;       /* AUDIT_EVICT / AUDIT_REJECT */
    int reason;     /* REASON_* */
    int victim_id;  /* incumbent removed (EVICT), or -1 (REJECT) */
    int incoming_id;
    float fl, fa, fb;           /* victim's three raw factor values */
    float victim_score;
    float incoming_score;
    int level;
} Audit;

typedef struct {
    P p[MAXP];
    int n;
    float rate_acc;
    int emitted;   /* total spawn requests accepted into the pool at some point */
    int dropped;   /* newcomers refused because the pool was full */
    int evicted;   /* incumbents removed by contribution eviction */
    int rejected;  /* newcomers that lost the score contest */
    Policy policy;
    int level;
    int frame;
    int id_counter; /* monotonic spawn-request id, per emitter */
    Audit log[AUDIT_CAP];
    int nlog;
    int log_overflow;
} Emitter;

void em_init(Emitter* e, Policy policy, int level);

/* Full-parameter spawn. Returns slot index, -1 on bad args, -2 on refusal. */
int em_spawn(Emitter* e, float x, float y, float vx, float vy,
             float life, float size, float bright);

int em_update(Emitter* e, float dt, float rate, int burst);

/* Precision configuration: quantization bins and inverse-variance weights. */
void em_level_config(int level, int bins[3], float weights[3], const char** name);

/* Raw unquantized factors of a particle, independent of any precision level. */
void em_factors(const P* p, float f[3]);

/* Composite contribution score under a given precision level. */
float em_score(const P* p, int level);

/* Score with level's bins/weights but another level's geometric model;
 * math_level < 0 means use level's own model. */
float em_score_at(const P* p, int level, int math_level);

/* Equal-weight, fixed-model score with only the quantization bins varying;
 * isolates pure quantization error for the precision curve. */
float em_quant_probe(const P* p, int level);

/* Reference contribution: double precision, equal weights, no quantization. */
double em_ref_score(const P* p);

/* Re-derive the reason code from the logged factor values. */
int em_reason_from_factors(int level, float fl, float fa, float fb);

const char* em_reason_str(int reason);
const char* em_audit_kind_str(int kind);

/* Find the audit event involving particle_id (as victim or newcomer). */
const Audit* em_audit_find(const Emitter* e, int particle_id);
void em_audit_print(const Emitter* e);

#endif
