
/* cover: smoke opacity budget against alive particle count.
 *
 * A synchronized puff has C(t) = N * alpha(t), alpha=(1-t/L)^p.  A later
 * replenishment particle starts at alpha=1, so the frame budget has to be
 * checked cohort by cohort:
 *
 *     C_f = sum_k n_k * (1 - age_k/L)^p.
 *
 * The critical frame is the replenishment frame b.  Let h=dt/L and
 * x=1-b*h.  Before that frame the original puff contributes N0*(x+h)^p;
 * after one aging step it contributes N0*x^p, plus M new opacity.  The
 * total is non-increasing on that frame exactly when
 *
 *     (x+h)^p - x^p >= M/N0.
 *
 * The left side tends to zero for both p->0 and p->infinity, which gives a
 * bounded exponent window.  All other frames contain aging or expiry only,
 * so their contributions are non-increasing.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>

#define FRAME_DT (1.0 / 60.0)
#define PARTICLE_N 200
#define PULSE_N 1
#define PULSE_AT_LIFE 0.60
#define TAIL_BY_LIFE 0.90
#define TAIL_OPACITY 0.01
#define P_EXPONENT 2.25
#define MAX_FRAMES 512
#define MONO_EPS 1.0e-9

typedef struct {
    int frames;
    double t[MAX_FRAMES + 1];
    double cover[MAX_FRAMES + 1];
    int count[MAX_FRAMES + 1];

    int first_up_frame;
    double first_up_t;
    double first_up_before;
    double first_up_after;
    double first_up_ratio;

    int max_step_frame;
    double max_step_ratio;
    double min_cover;
    double max_cover;
    double min_to_max_ratio;

    int tail_frame;
    double tail_t;
    double tail_over_life;
    int monotone;
} CoverRun;

typedef struct {
    double lower;
    double peak;
    double upper;
    double max_loss;
    int exists;
} JumpWindow;

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static double clamp_unit(double x) {
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

/* Single-particle opacity.  Age at or beyond life is exactly invisible. */
static double cover_alpha(double age, double life, double exponent) {
    if (!(life > 0.0) || !(exponent > 0.0) || age < 0.0) return -1.0;
    if (age >= life) return 0.0;
    double u = clamp_unit(age / life);
    return pow(1.0 - u, exponent);
}

/* Literal synchronized-cohort product requested in the diagnosis. */
static double cover_product(int n, double t, double life, double exponent) {
    if (n < 0) return -1.0;
    return (double)n * cover_alpha(t, life, exponent);
}

static int frame_count(double life) {
    return (int)lround(life / FRAME_DT);
}

/* Normalized one-frame opacity loss of the original puff at pulse frame. */
static double jump_loss(double exponent, double life) {
    int m = frame_count(life);
    int b = (int)lround(PULSE_AT_LIFE * (double)m);
    double h = 1.0 / (double)m;
    double x = 1.0 - (double)b / (double)m;
    return pow(x + h, exponent) - pow(x, exponent);
}

static double jump_required(void) {
    return (double)PULSE_N / (double)PARTICLE_N;
}

static JumpWindow jump_window(double life) {
    JumpWindow w = { 0.0, 0.0, 0.0, 0.0, 0 };
    const int samples = 20001;
    const double p_min = 1.0e-4;
    const double p_max = 100.0;
    double best_p = 1.0;

    for (int i = 0; i < samples; i++) {
        double q = (double)i / (double)(samples - 1);
        double p = exp(log(p_min) + q * (log(p_max) - log(p_min)));
        double loss = jump_loss(p, life);
        if (loss > w.max_loss) {
            w.max_loss = loss;
            best_p = p;
        }
    }
    w.peak = best_p;
    if (w.max_loss < jump_required()) return w;
    w.exists = 1;

    double lo = 1.0e-12, hi = best_p;
    for (int i = 0; i < 100; i++) {
        double mid = 0.5 * (lo + hi);
        if (jump_loss(mid, life) >= jump_required()) hi = mid;
        else lo = mid;
    }
    w.lower = 0.5 * (lo + hi);

    lo = best_p;
    hi = best_p * 2.0;
    while (jump_loss(hi, life) >= jump_required() && hi < 1.0e8) hi *= 2.0;
    for (int i = 0; i < 100; i++) {
        double mid = 0.5 * (lo + hi);
        if (jump_loss(mid, life) >= jump_required()) lo = mid;
        else hi = mid;
    }
    w.upper = 0.5 * (lo + hi);
    return w;
}

/* Total normalized cover at TAIL_BY_LIFE*L, including the pulse cohort. */
static double tail_equation(double exponent) {
    double original_remaining = 1.0 - TAIL_BY_LIFE;
    double pulse_remaining = 1.0 - TAIL_BY_LIFE + PULSE_AT_LIFE;
    return pow(original_remaining, exponent)
         + jump_required() * pow(pulse_remaining, exponent)
         - TAIL_OPACITY;
}

static double tail_exponent_required(void) {
    double lo = 0.0, hi = 10.0;
    while (tail_equation(hi) > 0.0 && hi < 1.0e8) hi *= 2.0;
    for (int i = 0; i < 100; i++) {
        double mid = 0.5 * (lo + hi);
        if (tail_equation(mid) > 0.0) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

static int cover_sim(double life, double exponent, CoverRun* r) {
    if (!(life > 0.0) || !(exponent > 0.0) || r == NULL) return -1;
    memset(r, 0, sizeof(*r));

    int m = frame_count(life);
    int pulse_frame = (int)lround(PULSE_AT_LIFE * (double)m);
    int end_frame = pulse_frame + m;
    if (end_frame > MAX_FRAMES) return -2;
    r->frames = end_frame;

    typedef struct { int birth_frame; int n; } Cohort;
    Cohort cohorts[2] = {
        { 0, PARTICLE_N },
        { pulse_frame, PULSE_N }
    };

    r->min_cover = 1.0e300;
    r->max_cover = -1.0e300;
    r->first_up_frame = -1;
    r->max_step_frame = 0;
    r->max_step_ratio = 0.0;
    r->tail_frame = -1;

    for (int f = 0; f <= end_frame; f++) {
        double c = 0.0;
        int alive = 0;
        for (int k = 0; k < 2; k++) {
            if (f < cohorts[k].birth_frame) continue;
            int age_frames = f - cohorts[k].birth_frame;
            double age = (double)age_frames * FRAME_DT;
            c += (double)cohorts[k].n * cover_alpha(age, life, exponent);
            if (age_frames < m) alive += cohorts[k].n;
        }
        r->t[f] = (double)f * FRAME_DT;
        r->cover[f] = c;
        r->count[f] = alive;
        if (c < r->min_cover) r->min_cover = c;
        if (c > r->max_cover) r->max_cover = c;
    }

    r->min_to_max_ratio = r->max_cover > 0.0
        ? r->min_cover / r->max_cover : 0.0;

    for (int f = 1; f <= end_frame; f++) {
        double before = r->cover[f - 1];
        double after = r->cover[f];
        if (before > 1.0e-15) {
            double ratio = after / before;
            if (ratio > r->max_step_ratio) {
                r->max_step_ratio = ratio;
                r->max_step_frame = f;
            }
        }
        if (r->first_up_frame < 0 && after > before + MONO_EPS) {
            r->first_up_frame = f;
            r->first_up_t = r->t[f];
            r->first_up_before = before;
            r->first_up_after = after;
            r->first_up_ratio = before > 1.0e-15 ? after / before : INFINITY;
        }
    }
    r->monotone = r->first_up_frame < 0;

    double threshold = TAIL_OPACITY * (double)PARTICLE_N;
    for (int f = 0; f <= end_frame; f++) {
        int stays_below = 1;
        for (int j = f; j <= end_frame; j++) {
            if (r->cover[j] > threshold + 1.0e-10) {
                stays_below = 0;
                break;
            }
        }
        if (stays_below) {
            r->tail_frame = f;
            r->tail_t = r->t[f];
            r->tail_over_life = r->tail_t / life;
            break;
        }
    }
    return 0;
}

static int design_ok(double exponent) {
    const double lives[3] = { 0.5, 1.0, 2.0 };
    for (int j = 0; j < 3; j++) {
        CoverRun r;
        if (cover_sim(lives[j], exponent, &r) != 0 || !r.monotone ||
            r.tail_frame < 0 ||
            r.tail_over_life > TAIL_BY_LIFE + 1.0e-9) {
            return 0;
        }
    }
    return 1;
}

/* Empirical all-life design window around the selected exponent.  Tail
 * quantization makes this a small subset of the single-frame jump window. */
static void empirical_window(double* lo_out, double* hi_out) {
    const double step = 0.0025;
    double lo = P_EXPONENT;
    double hi = P_EXPONENT;

    while (lo - step >= 1.5 && design_ok(lo - step)) lo -= step;
    while (hi + step <= 4.5 && design_ok(hi + step)) hi += step;

    *lo_out = lo;
    *hi_out = hi;
}

static void print_run_row(double life, double p, const CoverRun* r) {
    printf("L=%.2f p=%.4f frames=%d pulse_frame=%d C0=%.6f "
           "min_max_ratio=%.9f max_step_ratio=%.12f max_step_frame=%d "
           "first_rebound_frame=%d",
           life, p, r->frames,
           (int)lround(PULSE_AT_LIFE * frame_count(life)),
           r->cover[0], r->min_to_max_ratio,
           r->max_step_ratio, r->max_step_frame, r->first_up_frame);
    if (r->first_up_frame >= 0) {
        printf(" rebound_t=%.4f rebound_ratio=%.9f "
               "before=%.9f after=%.9f",
               r->first_up_t, r->first_up_ratio,
               r->first_up_before, r->first_up_after);
    } else {
        printf(" rebound_t=- rebound_ratio=-");
    }
    printf(" tail_frame=%d tail_t=%.4f tail_over_L=%.4f monotone=%d\n",
           r->tail_frame, r->tail_t, r->tail_over_life, r->monotone);
}

static void print_summary(void) {
    const double lives[3] = { 0.5, 1.0, 2.0 };
    double p_tail = tail_exponent_required();
    double common_lo = p_tail;
    double common_hi = 1.0e300;

    printf("# cover: particle count x single-particle opacity\n");
    printf("dt=%.6f N0=%d pulse_n=%d pulse_at=%.2fL selected_p=%.4f\n",
           FRAME_DT, PARTICLE_N, PULSE_N, PULSE_AT_LIFE, P_EXPONENT);
    printf("tail_spec: C <= %.3f*N0 by %.2fL\n",
           TAIL_OPACITY, TAIL_BY_LIFE);
    printf("tail_equation_p_required=%.6f\n", p_tail);

    for (int i = 0; i < 3; i++) {
        JumpWindow w = jump_window(lives[i]);
        printf("jump_window L=%.2f exists=%d lower=%.6f peak=%.6f "
               "upper=%.6f max_loss=%.9f required=%.6f\n",
               lives[i], w.exists, w.lower, w.peak, w.upper,
               w.max_loss, jump_required());
        if (w.exists) {
            if (w.lower > common_lo) common_lo = w.lower;
            if (w.upper < common_hi) common_hi = w.upper;
        }
    }
    if (p_tail > common_lo) common_lo = p_tail;
    printf("common_analytic_window=[%.6f, %.6f]\n", common_lo, common_hi);

    double empirical_lo, empirical_hi;
    empirical_window(&empirical_lo, &empirical_hi);
    printf("common_frame_scan_window=[%.4f, %.4f]\n",
           empirical_lo, empirical_hi);

    for (int i = 0; i < 3; i++) {
        CoverRun r;
        cover_sim(lives[i], P_EXPONENT, &r);
        print_run_row(lives[i], P_EXPONENT, &r);
    }

    const double stress_p[2] = { 4.0, 8.0 };
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 3; i++) {
            CoverRun r;
            cover_sim(lives[i], stress_p[k], &r);
            printf("stress ");
            print_run_row(lives[i], stress_p[k], &r);
        }
    }
}

static void print_sweep_csv(void) {
    const double lives[3] = { 0.5, 1.0, 2.0 };
    printf("p,L,monotone,first_up_frame,first_up_t,first_up_ratio,"
           "min_to_max_ratio,max_step_ratio,max_step_frame,"
           "tail_frame,tail_t,tail_over_L\n");
    for (int i = 0; i <= 318; i++) {
        double p = 0.05 + 0.025 * (double)i;  /* 0.05 .. 8.0 */
        for (int j = 0; j < 3; j++) {
            CoverRun r;
            cover_sim(lives[j], p, &r);
            printf("%.4f,%.2f,%d,%d,%.6f,%.12g,%.12g,%.12g,%d,%d,%.6f,%.6f\n",
                   p, lives[j], r.monotone, r.first_up_frame,
                   r.first_up_frame < 0 ? 0.0 : r.first_up_t,
                   r.first_up_frame < 0 ? 1.0 : r.first_up_ratio,
                   r.min_to_max_ratio, r.max_step_ratio,
                   r.max_step_frame, r.tail_frame, r.tail_t,
                   r.tail_over_life);
        }
    }
}

static void print_frames_csv(void) {
    const double lives[3] = { 0.5, 1.0, 2.0 };
    printf("L,p,frame,t,cover,count\n");
    for (int j = 0; j < 3; j++) {
        CoverRun r;
        cover_sim(lives[j], P_EXPONENT, &r);
        for (int f = 0; f <= r.frames; f++) {
            printf("%.2f,%.4f,%d,%.6f,%.12g,%d\n",
                   lives[j], P_EXPONENT, f, r.t[f], r.cover[f], r.count[f]);
        }
    }
}

static void run_tests(void) {
    check("alpha_center_linear",
          fabs(cover_alpha(0.5, 1.0, 1.0) - 0.5) < 1.0e-12);
    check("alpha_power_quarter",
          fabs(cover_alpha(0.5, 1.0, 2.0) - 0.25) < 1.0e-12);
    check("alpha_end_life_zero", cover_alpha(1.0, 1.0, 2.0) == 0.0);
    check("alpha_bad_args", cover_alpha(0.0, 0.0, 2.0) < 0.0);

    check("literal_product_64",
          fabs(cover_product(64, 0.5, 1.0, 2.0) - 16.0) < 1.0e-12);

    double p_tail = tail_exponent_required();
    check("tail_inverse_root", fabs(p_tail - 2.116361) < 1.0e-5);

    const double lives[3] = { 0.5, 1.0, 2.0 };
    JumpWindow windows[3];
    int windows_ok = 1;
    int selected_inside = 1;
    int selected_runs = 1;
    int minmax_ok = 1;
    int tail_ok = 1;

    for (int i = 0; i < 3; i++) {
        windows[i] = jump_window(lives[i]);
        if (!windows[i].exists) windows_ok = 0;
        if (!(P_EXPONENT > windows[i].lower &&
              P_EXPONENT < windows[i].upper)) selected_inside = 0;

        CoverRun r;
        if (cover_sim(lives[i], P_EXPONENT, &r) != 0) {
            selected_runs = 0;
            continue;
        }
        if (!r.monotone || r.first_up_frame >= 0) selected_runs = 0;
        if (!(r.max_step_ratio <= 1.0 + 1.0e-12)) selected_runs = 0;
        if (!(r.min_to_max_ratio <= 1.0 + 1.0e-12)) minmax_ok = 0;
        if (r.tail_frame < 0 ||
            r.tail_over_life > TAIL_BY_LIFE + 1.0e-9) tail_ok = 0;
    }
    check("jump_windows_exist", windows_ok);
    check("selected_inside_all_windows", selected_inside);
    check("selected_three_lives_monotone", selected_runs);
    check("min_to_max_ratio_never_above_one", minmax_ok);
    check("selected_three_lives_tail", tail_ok);

    double empirical_lo, empirical_hi;
    empirical_window(&empirical_lo, &empirical_hi);
    check("frame_scan_lower_bound", empirical_lo > 2.10 && empirical_lo < 2.14);
    check("frame_scan_upper_bound", empirical_hi > 2.61 && empirical_hi < 2.66);

    CoverRun fast1, fast2, very_fast[3];
    cover_sim(1.0, 4.0, &fast1);
    cover_sim(2.0, 4.0, &fast2);
    check("p4_life1_rebound_frame_36", fast1.first_up_frame == 36);
    check("p4_life2_rebound_frame_72", fast2.first_up_frame == 72);
    check("p4_rebound_ratio_above_one",
          fast1.first_up_ratio > 1.015 && fast2.first_up_ratio > 1.10);

    CoverRun slow;
    cover_sim(1.0, 0.5, &slow);
    check("small_exponent_drags_past_budget",
          slow.tail_over_life > TAIL_BY_LIFE + 0.05);

    CoverRun count_run;
    cover_sim(0.5, P_EXPONENT, &count_run);
    check("expired_cohort_removed_from_count",
          count_run.count[30] == PULSE_N && count_run.count[48] == 0);

    static const int expected_p8_frames[3] = { 18, 36, 72 };
    int p8_frames_ok = 1;
    for (int i = 0; i < 3; i++) {
        cover_sim(lives[i], 8.0, &very_fast[i]);
        if (very_fast[i].first_up_frame != expected_p8_frames[i] ||
            very_fast[i].first_up_ratio <= 1.0) p8_frames_ok = 0;
    }
    check("p8_rebound_frames_18_36_72", p8_frames_ok);

    printf("all_ok=%d\n", failures == 0);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "csv") == 0) {
        print_sweep_csv();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "frames-csv") == 0) {
        print_frames_csv();
        return 0;
    }

    print_summary();
    run_tests();
    return failures == 0 ? 0 : 1;
}
