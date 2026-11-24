
/* visibility: contrast-driven per-particle luminance against a non-uniform
 * local background.
 *
 * Model (additive glow particle, normalized linear light):
 *   displayed pixel  L_d = L_b + w*L_p            (w = sprite falloff weight)
 *   Weber contrast   C = (L_d - L_b) / L_b = L_p / L_b
 *
 * Two contrast levels are used:
 *   C_JND    = 0.02  ideal-adaptation just-noticeable Weber fraction (lab);
 *                    drives the analytic bounds.
 *   C_TARGET = 0.55  practical visibility target for a small transient
 *                    particle against non-uniform glare (suprathreshold
 *                    small-target Weber fractions run an order of magnitude
 *                    above the large-field lab JND); drives the sweep.
 *
 * Analytic bounds (any threshold c):
 *   visible <=> L_p >= c * L_b
 *   brightest background on which L_p stays visible:  L_b,max = L_p / c
 *   minimum particle luminance on background L_b:     L_p,min = c * L_b
 * The floor is evaluated per particle from the *measured* local background
 * (with estimation error), so particles over dark regions keep their
 * intrinsic luminance instead of being brightened together with the rest.
 *
 * Overexposure is counted where L_b + w*L_p >= L_clip (0.60): the effective
 * tonemapped highlight headroom, above which the display saturates to white.
 * Three background tiers (dark / mid / bright) are each run once; a global
 * exposure gain is swept and chosen from the two constraint curves:
 *   bright-tier visibility rate >= 0.70
 *   dark-tier overexposed pixels <= 0.02 of all sprite pixels.
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>

#define C_JND    0.02f   /* lab just-noticeable Weber fraction */
#define C_TARGET 0.55f   /* practical visibility contrast for a small transient
                          * particle in glare (suprathreshold small-target Weber
                          * fractions run 0.5..1.0, far above the large-field
                          * lab JND) */
#define L_CLIP   0.60f   /* effective highlight headroom (white point after
                          * tone mapping): above this the display saturates */

/* ---- contrast primitives ------------------------------------------------ */

static float weber_contrast(float Lp, float Lb) {
    if (Lb <= 0.0f) return INFINITY;  /* any emission is distinguishable */
    return Lp / Lb;
}

static int is_visible(float Lp, float Lb, float cth) {
    return weber_contrast(Lp, Lb) >= cth;
}

/* brightest background on which a particle of luminance Lp stays visible */
static float bg_upper_bound(float Lp, float cth) {
    if (Lp <= 0.0f) return 0.0f;
    return Lp / cth;
}

/* minimum particle luminance required on a background of luminance Lb */
static float min_particle_lum(float Lb, float cth) {
    if (Lb <= 0.0f) return 0.0f;
    return cth * Lb;
}

/* ---- deterministic scene ------------------------------------------------ */

typedef struct { uint32_t s; } Rng;

static float rng_u(Rng* r) {
    r->s = r->s * 1103515245u + 12345u;
    return (float)(r->s >> 8) / (float)(1u << 24);  /* upper 24 bits */
}

static float rng_n(Rng* r) {  /* standard normal via Box-Muller */
    float u1 = fmaxf(rng_u(r), 1e-7f);
    float u2 = rng_u(r);
    return sqrtf(-2.0f * logf(u1)) * cosf(6.28318530718f * u2);
}

#define N_TIER 400
#define N_PIX  9    /* 3x3 sprite */

/* normalized sprite weights: center 1.0, edge ring 0.5, corners 0.25 */
static const float SPRITE_W[N_PIX] = {
    0.25f, 0.5f, 0.25f,
    0.5f,  1.0f, 0.5f,
    0.25f, 0.5f, 0.25f,
};

typedef struct {
    float base;    /* intrinsic emitted luminance */
    float Lb;      /* true local background */
    float Lb_est;  /* measured local background (estimation error) */
    float Lp;      /* rendered particle luminance */
} Particle;

typedef struct { const char* name; float lo, hi; } Tier;

static const Tier TIERS[3] = {
    { "dark",   0.02f, 0.10f },
    { "mid",    0.25f, 0.55f },
    { "bright", 0.70f, 0.95f },
};

#define HOT_FRAC 0.11f   /* fraction of hot sparks in the emission */

/* intrinsic emission: 11% hot sparks (0.30..0.95), 89% dim embers whose
 * top end (0.24) stays far below the uniform floor (0.5225), so whether an
 * ember clears the bright-tier visibility target depends on the floor. */
static float base_lum(Rng* r) {
    if (rng_u(r) < HOT_FRAC) return 0.30f + 0.65f * rng_u(r);
    return 0.02f + 0.22f * rng_u(r);
}

static void build_tier(const Tier* t, Particle* ps, uint32_t seed) {
    Rng r = { seed };
    for (int i = 0; i < N_TIER; i++) {
        float Lb = t->lo + (t->hi - t->lo) * rng_u(&r);
        float est = Lb * (1.0f + 0.15f * rng_n(&r));        /* +-15% 1-sigma error */
        if (est < 0.0f) est = 0.0f;
        ps[i].base = base_lum(&r);
        ps[i].Lb = Lb;
        ps[i].Lb_est = est;
        ps[i].Lp = 0.0f;
    }
}

/* ---- rendering policies ------------------------------------------------- */

enum { ADAPTIVE = 0, UNIFORM = 1 };

/* ADAPTIVE: per-particle floor from its own measured local background.
 * UNIFORM:  one floor for every particle (the "brighten everything" baseline);
 *           with no local information it must be anchored on the bright-tier
 *           worst case, which drags dark-scene embers up toward clipping. */
static void render(Particle* ps, int n, float gain, int policy, float ref_bg) {
    float uniform_floor = min_particle_lum(ref_bg, C_TARGET);
    for (int i = 0; i < n; i++) {
        float floor_lp = (policy == ADAPTIVE)
                       ? min_particle_lum(ps[i].Lb_est, C_TARGET)
                       : uniform_floor;
        ps[i].Lp = gain * fmaxf(ps[i].base, floor_lp);
    }
}

typedef struct { float vis; float over; float boosted; } Score;

static Score evaluate(const Particle* ps) {
    int vis = 0, over = 0, boosted = 0;
    for (int i = 0; i < N_TIER; i++) {
        if (is_visible(ps[i].Lp, ps[i].Lb, C_TARGET)) vis++;
        if (min_particle_lum(ps[i].Lb_est, C_TARGET) > ps[i].base) boosted++;
        for (int k = 0; k < N_PIX; k++)
            if (ps[i].Lb + SPRITE_W[k] * ps[i].Lp >= L_CLIP) over++;
    }
    Score s = {
        (float)vis / N_TIER,
        (float)over / (N_TIER * N_PIX),
        (float)boosted / N_TIER,
    };
    return s;
}

/* ---- gain sweep --------------------------------------------------------- */

#define N_GAIN 49
#define G_LO   1.0f
#define G_HI   6.0f
#define VIS_REQ    0.70f
#define OVER_LIMIT 0.02f

static float gain_at(int j) {
    float a = logf(G_LO), b = logf(G_HI);
    return expf(a + (b - a) * (float)j / (float)(N_GAIN - 1));
}

/* uniform baseline anchor: bright-tier worst-case background */
#define UNIFORM_REF_BG 0.95f

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    int csv_mode = argc > 1 && strcmp(argv[1], "csv") == 0;

    static Particle scene[3][N_TIER];
    for (int t = 0; t < 3; t++) build_tier(&TIERS[t], scene[t], 11u + 101u * t);

    if (csv_mode) {
        printf("gain,policy,tier,visibility,overexposure,boosted\n");
        for (int j = 0; j < N_GAIN; j++) {
            float g = gain_at(j);
            for (int policy = 0; policy < 2; policy++) {
                for (int t = 0; t < 3; t++) {
                    render(scene[t], N_TIER, g, policy, UNIFORM_REF_BG);
                    Score s = evaluate(scene[t]);
                    printf("%.5f,%s,%s,%.5f,%.5f,%.5f\n",
                           g, policy == ADAPTIVE ? "adaptive" : "uniform",
                           TIERS[t].name, s.vis, s.over, s.boosted);
                }
            }
        }
        return 0;
    }

    /* determinism: same seed rebuilds the same scene (before rendering) */
    Particle again[N_TIER];
    build_tier(&TIERS[2], again, 11u + 101u * 2u);
    check("deterministic_build", memcmp(again, scene[2], sizeof(again)) == 0);

    /* ---- unit checks on the contrast math ---- */
    check("weber_basic", fabsf(weber_contrast(0.01f, 0.5f) - 0.02f) < 1e-6f);
    check("above_jnd", is_visible(0.0101f, 0.5f, C_JND) == 1);
    check("below_jnd", is_visible(0.009f, 0.5f, C_JND) == 0);
    check("bg_bound_jnd", fabsf(bg_upper_bound(0.02f, C_JND) - 1.0f) < 1e-6f);
    check("min_lum_jnd", fabsf(min_particle_lum(0.5f, C_JND) - 0.01f) < 1e-6f);
    check("target_above_jnd", C_TARGET > C_JND);
    check("practical_floor", fabsf(min_particle_lum(0.95f, C_TARGET) - 0.5225f) < 1e-6f);
    check("zero_bg_visible", is_visible(0.001f, 0.0f, C_JND) == 1 &&
                             isinf(weber_contrast(0.001f, 0.0f)));
    check("bad_lum_guarded", min_particle_lum(-1.0f, C_JND) == 0.0f &&
                             bg_upper_bound(0.0f, C_JND) == 0.0f);

    /* ---- adaptive sweep: both curves must move monotonically with gain ---- */
    static Score ad[N_GAIN][3];
    for (int j = 0; j < N_GAIN; j++) {
        float g = gain_at(j);
        for (int t = 0; t < 3; t++) {
            render(scene[t], N_TIER, g, ADAPTIVE, UNIFORM_REF_BG);
            ad[j][t] = evaluate(scene[t]);
        }
    }
    int mono = 1;
    for (int j = 1; j < N_GAIN; j++) {
        for (int t = 0; t < 3; t++) {
            if (ad[j][t].vis  < ad[j - 1][t].vis)  mono = 0;
            if (ad[j][t].over < ad[j - 1][t].over) mono = 0;
        }
    }
    check("curves_monotonic", mono);

    /* feasible window: smallest gain satisfying both hard constraints */
    int chosen = -1;
    for (int j = 0; j < N_GAIN; j++) {
        if (ad[j][2].vis >= VIS_REQ && ad[j][0].over <= OVER_LIMIT) {
            chosen = j;
            break;
        }
    }
    check("adaptive_feasible", chosen >= 0);
    if (chosen >= 0) {
        float g = gain_at(chosen);
        printf("chosen_gain=%.4f\n", g);
        for (int t = 0; t < 3; t++) {
            printf("%s: visibility=%.4f overexposure=%.4f boosted=%.4f\n",
                   TIERS[t].name, ad[chosen][t].vis,
                   ad[chosen][t].over, ad[chosen][t].boosted);
        }
        check("bright_visibility_met", ad[chosen][2].vis >= VIS_REQ);
        check("dark_overexposure_met", ad[chosen][0].over <= OVER_LIMIT);
        /* dark-scene embers keep intrinsic luminance: not globally raised */
        check("not_all_brightened", ad[chosen][0].boosted < 0.5f &&
                                    ad[chosen][2].boosted > ad[chosen][0].boosted);
    }

    /* ---- uniform baseline: no gain may satisfy both constraints ---- */
    int uniform_ok = 0;
    float uniform_min_over = 2.0f, uniform_vis_at_min = 0.0f, uniform_g_at_min = 0.0f;
    for (int j = 0; j < N_GAIN; j++) {
        float g = gain_at(j);
        Score sd, sb;
        render(scene[0], N_TIER, g, UNIFORM, UNIFORM_REF_BG);
        sd = evaluate(scene[0]);
        render(scene[2], N_TIER, g, UNIFORM, UNIFORM_REF_BG);
        sb = evaluate(scene[2]);
        if (sb.vis >= VIS_REQ && sd.over <= OVER_LIMIT) uniform_ok = 1;
        if (sd.over < uniform_min_over) {
            uniform_min_over = sd.over;
            uniform_vis_at_min = sb.vis;
            uniform_g_at_min = g;
        }
    }
    check("uniform_has_no_window", !uniform_ok);
    printf("uniform_min_dark_over=%.4f at gain=%.4f (bright vis there %.4f; over limit %.2f)\n",
           uniform_min_over, uniform_g_at_min, uniform_vis_at_min, OVER_LIMIT);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
