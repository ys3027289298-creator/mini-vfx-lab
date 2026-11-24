
/* rain: perspective-correct rain streak sizing and range validation.
 *
 * Screen width follows inverse distance:
 *
 *     w(z) = K / z,          K = focal_px * world_width_m
 *
 * Anchored at the near endpoint this is written as:
 *
 *     w(z) = w_near / (1 + c * (z - near_z)),  c = 1 / near_z
 *
 * and, with u = (z - near_z) / (far_z - near_z):
 *
 *     w(u) = w_near / (1 + A * u),  A = (far_z - near_z) / near_z
 *
 * A is range-normalized. It must be recomputed for each view-distance tier;
 * reusing one constant A makes the far/near ratio independent of the camera
 * range and is caught by the three-tier check below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NEAR_Z_DEFAULT      4.0f    /* m */
#define NEAR_WIDTH_DEFAULT  4.0f    /* px at NEAR_Z_DEFAULT */
#define SCREEN_W_PX         960.0f
#define SCREEN_H_PX         540.0f
#define FAR_STREAK_COUNT    6000.0f
#define FAR_LENGTH_PX       24.0f
#define RATIO_TOLERANCE     0.20f

typedef struct {
    float near_z;
    float far_z;
    float near_width_px;
} RainCamera;

typedef struct {
    float world_width_m;
    float z;
} RainDrop;

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int valid_camera(const RainCamera* cam) {
    return cam != NULL && isfinite(cam->near_z) && isfinite(cam->far_z) &&
           isfinite(cam->near_width_px) && cam->near_z > 0.0f &&
           cam->far_z > cam->near_z && cam->near_width_px > 0.0f;
}

/* Pure inverse-distance projection. K has units px*m. */
static float rain_width_k(float z, float k_px_m) {
    if (!isfinite(z) || z <= 0.0f || !isfinite(k_px_m) || k_px_m < 0.0f)
        return -1.0f;
    return k_px_m / z;
}

static float rain_perspective_k(float near_z, float near_width_px) {
    if (!(near_z > 0.0f) || !(near_width_px > 0.0f)) return -1.0f;
    return near_width_px * near_z;
}

static float rain_width(const RainCamera* cam, float z) {
    if (!valid_camera(cam) || !isfinite(z) || z <= 0.0f) return -1.0f;
    return cam->near_width_px * cam->near_z / z;
}

/* rho = w_far / w_near = near_z / far_z. */
static float rain_theory_ratio(float near_z, float far_z) {
    if (!(near_z > 0.0f) || !(far_z > near_z)) return -1.0f;
    return near_z / far_z;
}

/* c in w = w_near / (1 + c*(z-near_z)); exact perspective needs c=1/near_z. */
static float rain_depth_coefficient(float near_z) {
    if (!(near_z > 0.0f)) return -1.0f;
    return 1.0f / near_z;
}

/* A = c * span = span / near_z; this changes with the visible range. */
static float rain_normalized_attenuation(float near_z, float far_z) {
    if (!(near_z > 0.0f) || !(far_z > near_z)) return -1.0f;
    return (far_z - near_z) / near_z;
}

/* Invert rho and span L = far-near:
 *
 *     near = rho*L/(1-rho), far = L/(1-rho), c = (1/rho - 1)/L = 1/near.
 */
static int rain_invert_span(float ratio, float span,
                            float* near_z, float* far_z, float* c) {
    if (!isfinite(ratio) || !isfinite(span) || !(ratio > 0.0f) ||
        !(ratio < 1.0f) || !(span > 0.0f))
        return -1;
    float nz = ratio * span / (1.0f - ratio);
    float fz = span / (1.0f - ratio);
    float cv = (1.0f / ratio - 1.0f) / span;
    if (near_z) *near_z = nz;
    if (far_z) *far_z = fz;
    if (c) *c = cv;
    return 0;
}

static float drop_width_px(const RainDrop* d, float focal_px) {
    if (!d || d->z <= 0.0f || focal_px <= 0.0f) return -1.0f;
    return focal_px * d->world_width_m / d->z;
}

/* Deterministic width ensemble. The same physical drop widths are projected
 * at both endpoints, so this exercises particle projection rather than only
 * returning the closed-form ratio.
 */
static float rain_measured_ratio(const RainCamera* cam) {
    if (!valid_camera(cam)) return -1.0f;
    const int n = 64;
    double near_sum = 0.0, far_sum = 0.0;
    float focal_px = rain_perspective_k(cam->near_z, cam->near_width_px) / 0.010f;
    for (int i = 0; i < n; i++) {
        float unit = (float)((i * 37 + 11) % 53) / 52.0f;
        RainDrop d = { 0.010f * (0.85f + 0.30f * unit), cam->near_z };
        near_sum += drop_width_px(&d, focal_px);
        d.z = cam->far_z;
        far_sum += drop_width_px(&d, focal_px);
    }
    float near_mean = (float)(near_sum / n);
    float far_mean = (float)(far_sum / n);
    return far_mean / near_mean;
}

/* Poisson-union coverage of projected streak rectangles.
 * C = 1 - exp(-N*w*length / (screen_w*screen_h)).
 */
static float rain_far_coverage(float count, float width_px, float length_px,
                               float screen_w, float screen_h) {
    if (!isfinite(count) || !isfinite(width_px) || !isfinite(length_px) ||
        count < 0.0f || width_px < 0.0f || length_px < 0.0f ||
        !(screen_w > 0.0f) || !(screen_h > 0.0f))
        return -1.0f;
    float lambda = count * width_px * length_px / (screen_w * screen_h);
    return 1.0f - expf(-lambda);
}

static int run_tier(float far_z) {
    RainCamera cam = { NEAR_Z_DEFAULT, far_z, NEAR_WIDTH_DEFAULT };
    if (!valid_camera(&cam)) {
        printf("tier_error=bad_camera far_z=%.3f\n", far_z);
        return 2;
    }

    float rho_th = rain_theory_ratio(cam.near_z, cam.far_z);
    float rho_meas = rain_measured_ratio(&cam);
    float rel_err = fabsf(rho_meas - rho_th) / rho_th;
    float k = rain_perspective_k(cam.near_z, cam.near_width_px);
    float c = rain_depth_coefficient(cam.near_z);
    float A = rain_normalized_attenuation(cam.near_z, cam.far_z);
    float span = cam.far_z - cam.near_z;
    float far_width = rain_width(&cam, cam.far_z);
    float coverage = rain_far_coverage(FAR_STREAK_COUNT, far_width,
                                       FAR_LENGTH_PX, SCREEN_W_PX, SCREEN_H_PX);
    int ratio_pass = rel_err <= RATIO_TOLERANCE;
    int coverage_band = coverage >= 0.04f && coverage <= 0.20f;
    int pillar_risk = far_width >= 1.0f;

    printf("tier far_z=%.0f near_z=%.1f span=%.0f ratio_theory=%.6f "
           "ratio_meas=%.6f rel_err=%.4f tolerance=%.2f ratio_pass=%d "
           "k_px_m=%.3f c_per_m=%.6f A_normalized=%.3f near_width_px=%.3f "
           "far_width_px=%.4f far_coverage=%.5f coverage_band=%d pillar_risk=%d\n",
           cam.far_z, cam.near_z, span, rho_th, rho_meas, rel_err,
           RATIO_TOLERANCE, ratio_pass, k, c, A, cam.near_width_px,
           far_width, coverage, coverage_band, pillar_risk);
    return ratio_pass ? 0 : 1;
}

static void print_curves(void) {
    const float near_z = NEAR_Z_DEFAULT;
    const float far_z = 40.0f;
    const float span = far_z - near_z;
    const float rho_th = rain_theory_ratio(near_z, far_z);
    const float c_persp = rain_depth_coefficient(near_z);

    puts("c_per_m,ratio_model,ratio_perspective,ratio_rel_err,"
         "far_width_px,far_coverage,ratio_band_ok,coverage_band_ok,"
         "perspective_point");
    for (int i = 1; i <= 80; i++) {
        float c = i * 0.01f;              /* 0.01 .. 0.80 /m */
        float ratio = 1.0f / (1.0f + c * span);
        float err = fabsf(ratio - rho_th) / rho_th;
        float far_width = NEAR_WIDTH_DEFAULT * ratio;
        float cov = rain_far_coverage(FAR_STREAK_COUNT, far_width,
                                      FAR_LENGTH_PX, SCREEN_W_PX, SCREEN_H_PX);
        int ratio_ok = err <= RATIO_TOLERANCE;
        int cov_ok = cov >= 0.04f && cov <= 0.20f;
        int point = fabsf(c - c_persp) < 0.005f;
        printf("%.2f,%.6f,%.6f,%.6f,%.5f,%.6f,%d,%d,%d\n",
               c, ratio, rho_th, err, far_width, cov, ratio_ok, cov_ok, point);
    }
}

static void print_tiers_csv(void) {
    puts("tier,near_z,far_z,span,ratio_theory,ratio_meas,rel_err,"
         "A_normalized,c_per_m,far_width_px,far_coverage,ratio_pass");
    const float tiers[] = { 20.0f, 40.0f, 80.0f };
    const char* names[] = { "short", "medium", "long" };
    for (int i = 0; i < 3; i++) {
        RainCamera cam = { NEAR_Z_DEFAULT, tiers[i], NEAR_WIDTH_DEFAULT };
        float th = rain_theory_ratio(cam.near_z, cam.far_z);
        float meas = rain_measured_ratio(&cam);
        float err = fabsf(meas - th) / th;
        printf("%s,%.1f,%.0f,%.0f,%.6f,%.6f,%.6f,%.3f,%.6f,%.5f,%.6f,%d\n",
               names[i], cam.near_z, cam.far_z, cam.far_z - cam.near_z,
               th, meas, err,
               rain_normalized_attenuation(cam.near_z, cam.far_z),
               rain_depth_coefficient(cam.near_z),
               rain_width(&cam, cam.far_z),
               rain_far_coverage(FAR_STREAK_COUNT, rain_width(&cam, cam.far_z),
                                 FAR_LENGTH_PX, SCREEN_W_PX, SCREEN_H_PX),
               err <= RATIO_TOLERANCE);
    }
}

static int selftest(void) {
    check("inverse_size_near",
          fabsf(rain_width_k(4.0f, 16.0f) - 4.0f) < 1e-6f);
    check("inverse_size_far",
          fabsf(rain_width_k(40.0f, 16.0f) - 0.4f) < 1e-6f);
    check("theory_ratio",
          fabsf(rain_theory_ratio(4.0f, 40.0f) - 0.10f) < 1e-6f);

    float nz, fz, cv;
    check("invert_ok", rain_invert_span(0.10f, 36.0f, &nz, &fz, &cv) == 0);
    check("invert_near", fabsf(nz - 4.0f) < 1e-5f);
    check("invert_far", fabsf(fz - 40.0f) < 1e-5f);
    check("invert_c", fabsf(cv - 0.25f) < 1e-6f);

    check("normalized_A_short",
          fabsf(rain_normalized_attenuation(4.0f, 20.0f) - 4.0f) < 1e-6f);
    check("normalized_A_medium",
          fabsf(rain_normalized_attenuation(4.0f, 40.0f) - 9.0f) < 1e-6f);
    check("normalized_A_long",
          fabsf(rain_normalized_attenuation(4.0f, 80.0f) - 19.0f) < 1e-6f);

    const float tiers[] = { 20.0f, 40.0f, 80.0f };
    int all_ratios = 1;
    for (int i = 0; i < 3; i++) {
        RainCamera cam = { NEAR_Z_DEFAULT, tiers[i], NEAR_WIDTH_DEFAULT };
        float th = rain_theory_ratio(cam.near_z, cam.far_z);
        float meas = rain_measured_ratio(&cam);
        float err = fabsf(meas - th) / th;
        char name[64];
        snprintf(name, sizeof(name), "tier_%g_ratio_within_20pct", tiers[i]);
        check(name, err <= RATIO_TOLERANCE);
        all_ratios &= err <= RATIO_TOLERANCE;
    }
    check("three_tier_ratio_ok", all_ratios);

    /* Bug model: hard-code the medium tier's normalized A=9. */
    const float constant_A = 9.0f;
    float worst_constant_bug_err = 0.0f;
    for (int i = 0; i < 3; i++) {
        float th = rain_theory_ratio(NEAR_Z_DEFAULT, tiers[i]);
        float buggy_ratio = 1.0f / (1.0f + constant_A);
        worst_constant_bug_err =
            fmaxf(worst_constant_bug_err, fabsf(buggy_ratio - th) / th);
    }
    check("constant_normalized_A_rejected",
          worst_constant_bug_err > RATIO_TOLERANCE);

    check("coverage_zero",
          fabsf(rain_far_coverage(0.0f, 1.0f, 1.0f, 100.0f, 100.0f)) < 1e-6f);
    check("coverage_grows",
          rain_far_coverage(10.0f, 4.0f, 24.0f, 960.0f, 540.0f) >
          rain_far_coverage(10.0f, 0.4f, 24.0f, 960.0f, 540.0f));
    check("bad_distance", rain_width_k(0.0f, 1.0f) < 0.0f);
    check("bad_inversion", rain_invert_span(1.2f, 10.0f, NULL, NULL, NULL) < 0);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 1 || strcmp(argv[1], "--check") == 0)
        return selftest();
    if (strcmp(argv[1], "--curves") == 0) {
        print_curves();
        return 0;
    }
    if (strcmp(argv[1], "--tiers-csv") == 0) {
        print_tiers_csv();
        return 0;
    }
    if (strcmp(argv[1], "--tier") == 0 && argc == 3) {
        char* end = NULL;
        float far_z = strtof(argv[2], &end);
        if (end == argv[2] || *end != '\0' || far_z <= NEAR_Z_DEFAULT) {
            fprintf(stderr, "usage: %s --tier <far_z > %.1f>\n",
                    argv[0], NEAR_Z_DEFAULT);
            return 2;
        }
        return run_tier(far_z);
    }

    fprintf(stderr,
            "usage: %s [--check|--tier <far_z>|--curves|--tiers-csv]\n",
            argv[0]);
    return 2;
}
