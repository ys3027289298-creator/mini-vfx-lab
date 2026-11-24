/* substep_bench: measure ground tunnelling versus collision substeps.
 *
 * The controlled question is: for a particle of radius r moving at speed v
 * during a frame dt, how many equal substeps N make the largest substep
 * displacement v*dt/N smaller than r?
 *
 * Penetration is counted when an uncorrected substep places the particle
 * centre at or below the mathematical ground plane (y <= 0). Ordinary contact
 * is the radius offset y = r and is clamped/reflected immediately.
 */
#define _POSIX_C_SOURCE 199309L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RADIUS 0.10
#define RADIUS_F 0.10f
#define VMAX 12.0
#define VMAX_F 12.0f
#define RESTITUTION 0.40f

typedef struct {
    float y;
    float vy;
} Particle;

static double now_wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double now_cpu_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static double uniform01(uint64_t *state) {
    return (double)(splitmix64(state) >> 11) * 0x1.0p-53;
}

static double lambda_for(double fps) {
    double dt = 1.0 / fps;
    return VMAX * dt / RADIUS;
}

/* Smallest integer N with N > lambda = vmax*dt/r. */
static int worst_case_substeps(double lambda) {
    int n = (int)floor(lambda + 1e-12) + 1;
    return n < 1 ? 1 : n;
}

/* Uniform-speed stress model: V ~ Uniform(0, Vmax).
 * p_tunnel(N) = max(0, 1 - N/lambda). Invert p <= target.
 * target == 0 uses the strict worst-case bound, not ceil(lambda).
 */
static int target_rate_substeps(double lambda, double target) {
    if (!(target > 0.0)) return worst_case_substeps(lambda);
    if (target >= 1.0) return 1;
    double raw = lambda * (1.0 - target);
    int n = (int)ceil(raw - 1e-12);
    return n < 1 ? 1 : n;
}

/* One straight-motion collision substep. Returns 1 if the pre-resolution
 * position reaches/crosses the actual ground plane.
 */
static inline int substep(Particle *p, float h) {
    p->y += p->vy * h;
    int crossed_plane = (p->y <= 0.0f);
    if (p->y < RADIUS_F) {
        p->y = RADIUS_F;
        p->vy = -p->vy * RESTITUTION;
    }
    return crossed_plane;
}

static int run_one_trial(int n, double dt, double speed, double gap) {
    float h = (float)(dt / (double)n);
    Particle p = {
        RADIUS_F + (float)gap,
        -(float)speed
    };
    int tunneled = 0;
    for (int k = 0; k < n; k++) {
        if (substep(&p, h)) tunneled = 1;
    }
    return tunneled;
}

static uint64_t simulate_frames(Particle *particles, int count, int frames,
                                double dt, int n, uint64_t *crossings) {
    float h = (float)(dt / (double)n);
    uint64_t hit_count = 0;

    for (int frame = 0; frame < frames; frame++) {
        /* Outer substep loop: each substep updates the whole particle system,
         * matching the usual engine scheduler and charging load/store cost N
         * times as well.
         */
        for (int k = 0; k < n; k++) {
            for (int i = 0; i < count; i++) {
                float y = particles[i].y;
                float vy = particles[i].vy;
                y += vy * h;
                if (y <= 0.0f) hit_count++;
                if (y < RADIUS_F) {
                    y = RADIUS_F;
                    vy = -vy * RESTITUTION;
                }
                particles[i].y = y;
                particles[i].vy = vy;
            }
        }
    }

    *crossings = hit_count;
    uint64_t checksum = hit_count ^ UINT64_C(0x9e3779b97f4a7c15);
    for (int i = 0; i < count; i++) {
        uint32_t bits;
        memcpy(&bits, &particles[i].y, sizeof(bits));
        checksum ^= (uint64_t)bits * UINT64_C(0x9e3779b97f4a7c15);
    }
    return checksum;
}

static int opt_int(int argc, char **argv, const char *key, int fallback) {
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], key) == 0) return atoi(argv[i + 1]);
    }
    return fallback;
}

static long long opt_ll(int argc, char **argv, const char *key,
                        long long fallback) {
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], key) == 0) return atoll(argv[i + 1]);
    }
    return fallback;
}

static double opt_double(int argc, char **argv, const char *key,
                         double fallback) {
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], key) == 0) return atof(argv[i + 1]);
    }
    return fallback;
}

static int failures = 0;

static void check(const char *name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

static int run_tests(void) {
    check("lambda_60", fabs(lambda_for(60.0) - 2.0) < 1e-12);
    check("lambda_30", fabs(lambda_for(30.0) - 4.0) < 1e-12);
    check("lambda_15", fabs(lambda_for(15.0) - 8.0) < 1e-12);
    check("strict_n_60", worst_case_substeps(lambda_for(60.0)) == 3);
    check("strict_n_30", worst_case_substeps(lambda_for(30.0)) == 5);
    check("strict_n_15", worst_case_substeps(lambda_for(15.0)) == 9);
    check("target_n_15_p20",
          target_rate_substeps(lambda_for(15.0), 0.20) == 7);
    check("target_n_15_p0",
          target_rate_substeps(lambda_for(15.0), 0.0) == 9);

    const double fps_values[3] = {60.0, 30.0, 15.0};
    for (int f = 0; f < 3; f++) {
        double fps = fps_values[f];
        double dt = 1.0 / fps;
        int n_safe = worst_case_substeps(lambda_for(fps));

        int negative_control = run_one_trial(n_safe - 1, dt, VMAX, 0.0);
        char name[64];
        snprintf(name, sizeof(name), "fps%.0f_unsafe_control_detects", fps);
        check(name, negative_control == 1);

        int boundary_bad = 0;
        for (int k = 0; k <= 10000; k++) {
            double gap = RADIUS * (double)k / 10000.0;
            if (run_one_trial(n_safe, dt, VMAX, gap)) boundary_bad = 1;
        }
        snprintf(name, sizeof(name), "fps%.0f_worst_case_zero_tunnel", fps);
        check(name, boundary_bad == 0);
    }

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

static int run_study(int argc, char **argv) {
    double fps = opt_double(argc, argv, "--fps", 60.0);
    long long samples = opt_ll(argc, argv, "--samples", 1000000);
    long long verify_trials = opt_ll(argc, argv, "--verify-trials", 5000000);
    int particle_count = opt_int(argc, argv, "--particles", 100000);
    int frames = opt_int(argc, argv, "--frames", 60);
    int rounds = opt_int(argc, argv, "--rounds", 5);
    double target = opt_double(argc, argv, "--target", 0.20);

    double dt = 1.0 / fps;
    double lambda = lambda_for(fps);
    int n_safe = worst_case_substeps(lambda);
    int n_target = target_rate_substeps(lambda, target);

    printf("{\n");
    printf("  \"fps\": %.17g,\n", fps);
    printf("  \"dt\": %.17g,\n", dt);
    printf("  \"radius\": %.17g,\n", RADIUS);
    printf("  \"vmax\": %.17g,\n", VMAX);
    printf("  \"lambda\": %.17g,\n", lambda);
    printf("  \"n_safe\": %d,\n", n_safe);
    printf("  \"target_rate\": %.17g,\n", target);
    printf("  \"n_target\": %d,\n", n_target);

    printf("  \"penetration\": [\n");
    for (int n = 1; n <= n_safe; n++) {
        uint64_t seed = UINT64_C(0x517cc1b727220a95) ^
                       ((uint64_t)fps * 1000003ULL) ^ (uint64_t)n;
        long long tunneled = 0;
        for (long long i = 0; i < samples; i++) {
            double speed = VMAX * uniform01(&seed);
            if (run_one_trial(n, dt, speed, 0.0)) tunneled++;
        }
        double rate = (double)tunneled / (double)samples;
        printf("    {\"n\": %d, \"samples\": %lld, \"tunneled\": %lld, "
               "\"rate\": %.17g}%s\n",
               n, samples, tunneled, rate, n == n_safe ? "" : ",");
    }
    printf("  ],\n");

    Particle *base = malloc((size_t)particle_count * sizeof(*base));
    Particle *current = malloc((size_t)particle_count * sizeof(*current));
    if (!base || !current) {
        fprintf(stderr, "allocation failed\n");
        free(base);
        free(current);
        return 2;
    }

    uint64_t init_seed = UINT64_C(0x243f6a8885a308d3);
    for (int i = 0; i < particle_count; i++) {
        base[i].y = RADIUS_F + RADIUS_F * (float)uniform01(&init_seed);
        base[i].vy = -VMAX_F;
    }

    printf("  \"timing\": [\n");
    int row = 0;
    int total_rows = n_safe * rounds;
    for (int n = 1; n <= n_safe; n++) {
        uint64_t crossings = 0;
        uint64_t checksum = 0;

        memcpy(current, base, (size_t)particle_count * sizeof(*base));
        checksum = simulate_frames(current, particle_count, 2, dt, n,
                                   &crossings);

        for (int round_index = 0; round_index < rounds; round_index++) {
            memcpy(current, base, (size_t)particle_count * sizeof(*base));
            double wall0 = now_wall_seconds();
            double cpu0 = now_cpu_seconds();
            checksum = simulate_frames(current, particle_count, frames, dt, n,
                                       &crossings);
            double cpu1 = now_cpu_seconds();
            double wall1 = now_wall_seconds();
            row++;
            printf("    {\"n\": %d, \"round\": %d, \"elapsed_s\": %.17g, "
                   "\"cpu_s\": %.17g, \"frame_ms\": %.17g, "
                   "\"wall_frame_ms\": %.17g, \"checksum\": %llu, "
                   "\"crossings\": %llu}%s\n",
                   n, round_index + 1, wall1 - wall0, cpu1 - cpu0,
                   (cpu1 - cpu0) * 1000.0 / (double)frames,
                   (wall1 - wall0) * 1000.0 / (double)frames,
                   (unsigned long long)checksum,
                   (unsigned long long)crossings,
                   row == total_rows ? "" : ",");
        }
    }
    printf("  ],\n");

    uint64_t verify_seed = UINT64_C(0xb7e151628aed2a6b) ^ (uint64_t)fps;
    long long verify_tunneled = 0;
    for (long long i = 0; i < verify_trials; i++) {
        double gap = RADIUS * uniform01(&verify_seed);
        if (run_one_trial(n_safe, dt, VMAX, gap)) verify_tunneled++;
    }

    int boundary_tunneled = 0;
    for (int k = 0; k <= 10000; k++) {
        double gap = RADIUS * (double)k / 10000.0;
        if (run_one_trial(n_safe, dt, VMAX, gap)) boundary_tunneled++;
    }

    int negative_control =
        n_safe > 1 ? run_one_trial(n_safe - 1, dt, VMAX, 0.0) : -1;
    int control_ok = (n_safe == 1) ? negative_control == -1
                                   : negative_control == 1;

    printf("  \"verification\": {\n");
    printf("    \"n\": %d,\n", n_safe);
    printf("    \"random_trials\": %lld,\n", verify_trials);
    printf("    \"random_tunneled\": %lld,\n", verify_tunneled);
    printf("    \"boundary_trials\": 10001,\n");
    printf("    \"boundary_tunneled\": %d,\n", boundary_tunneled);
    printf("    \"negative_control_tunneled\": %d,\n", negative_control);
    printf("    \"particle_count\": %d,\n", particle_count);
    printf("    \"timed_frames\": %d,\n", frames);
    printf("    \"timing_rounds\": %d\n", rounds);
    printf("  }\n");
    printf("}\n");

    free(base);
    free(current);
    return (verify_tunneled == 0 && boundary_tunneled == 0 && control_ok) ? 0
                                                                           : 1;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "test") == 0) return run_tests();
    if (strcmp(argv[1], "study") == 0) return run_study(argc, argv);

    fprintf(stderr, "usage: %s [test|study options]\n", argv[0]);
    return 2;
}
