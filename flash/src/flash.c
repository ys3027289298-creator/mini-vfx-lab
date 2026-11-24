
/* flash: two-term explosion brightness with a delayed rebound lobe.
 *
 * L(t) = exp(-ln2*t/h) + A*exp(-(t-tp)^2/(2*sigma^2))
 *
 * The first term is the monotone main flash.  The second is a delayed Gaussian
 * pulse whose peak time is inverted from the main half-life.  All times are
 * proportional to h, so changing the half-life rescales time without changing
 * the normalized rebound shape.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define FLASH_LN2 0.69314718055994530942f

/* Main brightness left at the secondary peak: 1/8, i.e. three half-lives.
 * This is strictly below the requested 0.30 gate. */
#define FLASH_PEAK_LEVEL 0.125f
#define FLASH_PEAK_HALF_LIVES 3.0f

/* Gaussian sigma as a fraction of the main half-life. */
#define FLASH_SIGMA_H 0.5f

#define FLASH_GATE_LEVEL 0.30f
#define FLASH_DEPTH_MIN 0.30f
#define FLASH_DEPTH_MAX 0.50f
#define FLASH_DEPTH_TARGET 0.40f
#define FLASH_FLICKER_LIMIT 1.0f

static const float FLASH_HALVES[3] = { 0.10f, 0.20f, 0.40f };

typedef struct {
    int ok;
    float t_min, l_min;
    float t_second, l_second;
    float depth;
    float max_rise_slope;
    float flicker;
} FlashMeasure;

typedef struct {
    int ok;
    float amp_depth_30;
    float amp_depth_40;
    float amp_depth_50;
    float amp_flicker_1;
    float amp_selected;
    FlashMeasure selected;
} FlashCalibration;

static int flash_valid_h(float h) { return isfinite(h) && h > 0.0f; }
static int flash_valid_a(float a) { return isfinite(a) && a >= 0.0f; }

/* Time at which the monotone term falls to level c. */
static float flash_time_for_level(float h, float c) {
    if (!flash_valid_h(h) || !isfinite(c) || c <= 0.0f || c >= 1.0f) return NAN;
    return h * logf(1.0f / c) / FLASH_LN2;
}

static float flash_t_peak(float h) {
    return flash_time_for_level(h, FLASH_PEAK_LEVEL);
}

static float flash_t_gate(float h) {
    return flash_time_for_level(h, FLASH_GATE_LEVEL);
}

static float flash_main(float h, float t) {
    if (!flash_valid_h(h) || !isfinite(t) || t < 0.0f) return NAN;
    return expf(-FLASH_LN2 * t / h);
}

static float flash_secondary(float h, float amp, float t) {
    if (!flash_valid_h(h) || !flash_valid_a(amp) || !isfinite(t) || t < 0.0f) return NAN;
    float tp = flash_t_peak(h);
    float sigma = FLASH_SIGMA_H * h;
    float z = (t - tp) / sigma;
    return amp * expf(-0.5f * z * z);
}

static float flash_value(float h, float amp, float t) {
    return flash_main(h, t) + flash_secondary(h, amp, t);
}

static float flash_derivative(float h, float amp, float t) {
    if (!flash_valid_h(h) || !flash_valid_a(amp) || !isfinite(t) || t < 0.0f) return NAN;

    float main_l = flash_main(h, t);
    float main_d = -(FLASH_LN2 / h) * main_l;

    float tp = flash_t_peak(h);
    float sigma = FLASH_SIGMA_H * h;
    float z = (t - tp) / sigma;
    float secondary_d = amp * ((tp - t) / (sigma * sigma)) *
                        expf(-0.5f * z * z);
    return main_d + secondary_d;
}

/* Minimum secondary amplitude that can create any stationary pair.
 *
 * Let k=ln2/h, s=sigma, p=tp, and y=p-t.  An extremum satisfies
 * A*y/s^2*exp(-y^2/(2s^2)) = k*exp(-k(p-y)).
 * The hardest point to satisfy is the maximum of the amplitude-free ratio.
 * Its y solves k + y/s^2 - 1/y = 0. */
static float flash_critical_amp(float h) {
    if (!flash_valid_h(h)) return NAN;

    float k = FLASH_LN2 / h;
    float s = FLASH_SIGMA_H * h;
    float p = flash_t_peak(h);
    float y = 0.5f * (-k * s * s + sqrtf(k * k * s * s * s * s + 4.0f * s * s));
    float t = p - y;
    if (t < 0.0f) return NAN;

    float main_d = k * expf(-k * t);
    float pulse_d_per_amp = (y / (s * s)) * expf(-y * y / (2.0f * s * s));
    return main_d / pulse_d_per_amp;
}

static int flash_bisect_root(float h, float amp, float lo, float hi,
                             float f_lo, float* root_out) {
    if (!isfinite(f_lo)) return -1;
    for (int i = 0; i < 52; i++) {
        float mid = 0.5f * (lo + hi);
        float f_mid = flash_derivative(h, amp, mid);
        if (!isfinite(f_mid)) return -1;
        if (f_lo * f_mid <= 0.0f) {
            hi = mid;
        } else {
            lo = mid;
            f_lo = f_mid;
        }
    }
    *root_out = 0.5f * (lo + hi);
    return 0;
}

/* Find the negative-to-positive derivative crossing (the valley) followed by
 * the positive-to-negative crossing (the rebound peak). */
static int flash_measure(float h, float amp, FlashMeasure* m) {
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    if (!flash_valid_h(h) || !flash_valid_a(amp)) return -1;

    enum { MAX_ROOTS = 8, STEPS = 3072, SLOPE_STEPS = 384 };
    float roots[MAX_ROOTS];
    int n_roots = 0;

    float end = 4.2f * h;
    float prev_t = 0.0f;
    float prev_d = flash_derivative(h, amp, prev_t);

    for (int i = 1; i <= STEPS && n_roots < MAX_ROOTS; i++) {
        float t = end * (float)i / (float)STEPS;
        float d = flash_derivative(h, amp, t);
        int crossing = (prev_d < 0.0f && d > 0.0f) ||
                       (prev_d > 0.0f && d < 0.0f);
        if (crossing) {
            float r;
            if (flash_bisect_root(h, amp, prev_t, t, prev_d, &r) == 0) {
                if (n_roots == 0 || fabsf(r - roots[n_roots - 1]) > 1e-5f * h)
                    roots[n_roots++] = r;
            }
        }
        prev_t = t;
        prev_d = d;
    }

    float t_min = NAN, t_second = NAN;
    for (int i = 0; i < n_roots; i++) {
        float eps = 1e-3f * h;
        float d_left = flash_derivative(h, amp, roots[i] - eps);
        float d_right = flash_derivative(h, amp, roots[i] + eps);
        if (isnan(t_min) && d_left < 0.0f && d_right > 0.0f) {
            t_min = roots[i];
        } else if (!isnan(t_min) && isnan(t_second) &&
                   d_left > 0.0f && d_right < 0.0f && roots[i] > t_min) {
            t_second = roots[i];
        }
    }

    if (isnan(t_min) || isnan(t_second)) return -2;

    m->t_min = t_min;
    m->t_second = t_second;
    m->l_min = flash_value(h, amp, t_min);
    m->l_second = flash_value(h, amp, t_second);
    m->depth = m->l_second - m->l_min;

    float max_slope = 0.0f;
    for (int i = 0; i <= SLOPE_STEPS; i++) {
        float u = (float)i / (float)SLOPE_STEPS;
        float t = t_min + (t_second - t_min) * u;
        float d = flash_derivative(h, amp, t);
        if (d > max_slope) max_slope = d;
    }
    m->max_rise_slope = max_slope;

    /* Compare the steepest rebound edge with the main flash's initial decay
     * speed scale k*L0.  Above one, the rebrightening edge is sharper than
     * the original decay and reads as a strobing fault. */
    m->flicker = max_slope / (FLASH_LN2 / h);
    m->ok = 1;
    return 0;
}

static float flash_interp_amp(float a0, float y0, float a1, float y1, float target) {
    if (fabsf(y1 - y0) < 1e-8f) return 0.5f * (a0 + a1);
    float u = (target - y0) / (y1 - y0);
    return a0 + (a1 - a0) * u;
}

/* Sweep the two tuning curves: depth(A) and flicker(A).  Linear interpolation
 * locates their design thresholds; the selected amplitude is then refined by
 * bisection on the depth target at the middle of the required 0.30--0.50 band. */
static int flash_calibrate(float h, FlashCalibration* c) {
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    c->amp_depth_30 = NAN;
    c->amp_depth_40 = NAN;
    c->amp_depth_50 = NAN;
    c->amp_flicker_1 = NAN;
    c->amp_selected = NAN;

    const float a_lo = 0.20f, a_hi = 0.90f, a_step = 0.001f;
    float prev_a = a_lo;
    FlashMeasure prev_m;
    if (flash_measure(h, prev_a, &prev_m) != 0) return -2;

    for (float a = a_lo + a_step; a <= a_hi + 1e-7f; a += a_step) {
        FlashMeasure m;
        if (flash_measure(h, a, &m) != 0) return -3;

        if (isnan(c->amp_depth_30) && prev_m.depth < 0.30f && m.depth >= 0.30f)
            c->amp_depth_30 = flash_interp_amp(prev_a, prev_m.depth, a, m.depth, 0.30f);
        if (isnan(c->amp_depth_40) && prev_m.depth < 0.40f && m.depth >= 0.40f)
            c->amp_depth_40 = flash_interp_amp(prev_a, prev_m.depth, a, m.depth, 0.40f);
        if (isnan(c->amp_depth_50) && prev_m.depth < 0.50f && m.depth >= 0.50f)
            c->amp_depth_50 = flash_interp_amp(prev_a, prev_m.depth, a, m.depth, 0.50f);
        if (isnan(c->amp_flicker_1) && prev_m.flicker < 1.0f && m.flicker >= 1.0f)
            c->amp_flicker_1 = flash_interp_amp(prev_a, prev_m.flicker, a, m.flicker, 1.0f);

        prev_a = a;
        prev_m = m;
    }

    if (isnan(c->amp_depth_30) || isnan(c->amp_depth_40) ||
        isnan(c->amp_depth_50) || isnan(c->amp_flicker_1))
        return -4;

    float lo = c->amp_depth_30;
    float hi = c->amp_depth_50;
    for (int i = 0; i < 45; i++) {
        float mid = 0.5f * (lo + hi);
        FlashMeasure m;
        if (flash_measure(h, mid, &m) != 0 || m.depth < FLASH_DEPTH_TARGET)
            lo = mid;
        else
            hi = mid;
    }
    c->amp_selected = 0.5f * (lo + hi);
    if (flash_measure(h, c->amp_selected, &c->selected) != 0) return -5;
    c->ok = 1;
    return 0;
}

static void print_curves_csv(void) {
    puts("amp,depth,flicker");
    for (int i = 20; i <= 80; i++) {
        float a = (float)i / 100.0f;
        FlashMeasure m;
        if (flash_measure(1.0f, a, &m) == 0)
            printf("%.2f,%.6f,%.6f\n", a, m.depth, m.flicker);
        else
            printf("%.2f,,\n", a);
    }
}

static void print_trace_csv(void) {
    puts("half_life,x,t,main,secondary,total");
    for (int j = 0; j < 3; j++) {
        float h = FLASH_HALVES[j];
        FlashCalibration c;
        if (flash_calibrate(h, &c) != 0) continue;
        for (int i = 0; i <= 600; i++) {
            float x = 6.0f * (float)i / 600.0f;
            float t = x * h;
            float main_l = flash_main(h, t);
            float secondary_l = flash_secondary(h, c.amp_selected, t);
            printf("%.2f,%.4f,%.6f,%.6f,%.6f,%.6f\n",
                   h, x, t, main_l, secondary_l, main_l + secondary_l);
        }
    }
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--curves") == 0) {
        print_curves_csv();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--trace") == 0) {
        print_trace_csv();
        return 0;
    }

    const float h = 0.16f;

    check("main_half_life", fabsf(flash_main(h, h) - 0.5f) < 1e-6f);

    float t30 = flash_t_gate(h);
    float tp = flash_t_peak(h);
    check("thirty_percent_time", fabsf(t30 / h - logf(10.0f / 3.0f) / FLASH_LN2) < 1e-6f);
    check("peak_inverted_from_half_life", fabsf(tp - FLASH_PEAK_HALF_LIVES * h) < 1e-6f);
    check("peak_after_thirty_gate", tp > t30 && flash_main(h, tp) < FLASH_GATE_LEVEL);
    check("peak_main_level_eighth", fabsf(flash_main(h, tp) - FLASH_PEAK_LEVEL) < 1e-6f);
    check("secondary_peak_is_amp", fabsf(flash_secondary(h, 0.59f, tp) - 0.59f) < 1e-6f);

    float a_crit = flash_critical_amp(h);
    check("critical_amp_derived", isfinite(a_crit) && a_crit > 0.095f && a_crit < 0.101f);

    FlashCalibration base;
    check("calibration_ok", flash_calibrate(h, &base) == 0 && base.ok);
    check("selected_above_critical", base.amp_selected > a_crit);
    check("flicker_threshold_on_curve",
          isfinite(base.amp_flicker_1) &&
          base.amp_flicker_1 > base.amp_depth_40 &&
          base.amp_flicker_1 < base.amp_depth_50);

    const char* names[3] = { "fast", "medium", "slow" };
    float depths[3];

    for (int i = 0; i < 3; i++) {
        float half = FLASH_HALVES[i];
        FlashCalibration c;
        int cal_ok = flash_calibrate(half, &c) == 0;
        check(i == 0 ? "fast_calibration_ok" :
              i == 1 ? "medium_calibration_ok" : "slow_calibration_ok", cal_ok);
        if (!cal_ok) {
            depths[i] = NAN;
            continue;
        }

        depths[i] = c.selected.depth;
        int band = c.selected.depth >= FLASH_DEPTH_MIN &&
                   c.selected.depth <= FLASH_DEPTH_MAX;
        int flicker = c.selected.flicker < FLASH_FLICKER_LIMIT;
        int valley_first = c.selected.t_min > 0.0f &&
                           c.selected.t_min < c.selected.t_second &&
                           c.selected.t_second < flash_t_peak(half);

        char depth_name[64], flicker_name[64], valley_name[64];
        snprintf(depth_name, sizeof(depth_name), "%s_depth_30_to_50", names[i]);
        snprintf(flicker_name, sizeof(flicker_name), "%s_flicker_below_limit", names[i]);
        snprintf(valley_name, sizeof(valley_name), "%s_valley_then_second_peak", names[i]);
        check(depth_name, band);
        check(flicker_name, flicker);
        check(valley_name, valley_first);

        printf("run=%s half_life=%.3f t30=%.4f tp=%.4f A30=%.4f A40=%.4f "
               "A50=%.4f Af=%.4f A=%.4f tmin=%.5f lmin=%.4f t2=%.5f "
               "l2=%.4f depth=%.4f flicker=%.4f\n",
               names[i], half, flash_t_gate(half),
               flash_t_peak(half), c.amp_depth_30, c.amp_depth_40,
               c.amp_depth_50, c.amp_flicker_1, c.amp_selected,
               c.selected.t_min, c.selected.l_min, c.selected.t_second,
               c.selected.l_second, c.selected.depth, c.selected.flicker);
    }

    if (isfinite(depths[0]) && isfinite(depths[2]))
        check("half_life_scale_invariant", fabsf(depths[0] - depths[2]) < 1e-5f);

    check("bad_half_life", isnan(flash_main(-1.0f, 0.0f)));
    FlashMeasure bad;
    check("bad_measure", flash_measure(0.0f, 0.6f, &bad) != 0);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
