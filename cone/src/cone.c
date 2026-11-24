
/* cone: 锥内方向采样。
 * cone_dir_uniform 为无偏（单位立体角概率恒定）采样器；
 * cone_dir_angle_uniform 是常见的有偏写法（角度均匀，轴附近过密），
 * 仅作对照与自检基准。u0/u1 应为 [0,1) 上的独立均匀随机数。 */
#include <stdio.h>
#include <math.h>

/* 无偏：cos(theta) 在 [cos alpha, 1] 上均匀，phi 在 [0, 2pi) 上均匀。
 * 返回以 z 轴为锥轴的单位向量。 */
static void cone_dir_uniform(float u0, float u1, float cos_alpha,
                             float* dx, float* dy, float* dz) {
    float cos_theta = 1.0f - u0 * (1.0f - cos_alpha);
    float sin_theta = sqrtf(fmaxf(0.0f, 1.0f - cos_theta * cos_theta));
    float phi = 6.283185307179586f * u1;
    *dx = sin_theta * cosf(phi);
    *dy = sin_theta * sinf(phi);
    *dz = cos_theta;
}

/* 有偏对照：theta 本身在 [0, alpha] 上均匀。 */
static void cone_dir_angle_uniform(float u0, float u1, float alpha,
                                   float* dx, float* dy, float* dz) {
    float theta = u0 * alpha;
    float phi = 6.283185307179586f * u1;
    *dx = sinf(theta) * cosf(phi);
    *dy = sinf(theta) * sinf(phi);
    *dz = cosf(theta);
}

static unsigned long rng_state = 0x12345678UL;
static float urand(void) { /* xorshift32，确定性即可，自检不依赖随机性质量 */
    unsigned long x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return (float)(x & 0xFFFFFFUL) / (float)0x1000000UL;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

int main(void) {
    const float alpha = 0.5235987755982988f; /* 30 度 */
    const float cos_alpha = cosf(alpha);

    /* 边界 u0=0 -> 锥轴；u0=1 -> 锥面边缘；都必须落在锥内且为单位长度 */
    float x, y, z;
    cone_dir_uniform(0.0f, 0.0f, cos_alpha, &x, &y, &z);
    check("axis_sample", fabsf(z - 1.0f) < 1e-6f);
    cone_dir_uniform(1.0f, 0.25f, cos_alpha, &x, &y, &z);
    check("edge_sample_on_cone", fabsf(z - cos_alpha) < 1e-6f &&
                                  x * x + y * y + z * z > 0.999f);

    int in_cone = 1, unit_ok = 1;
    for (int i = 0; i < 10000; i++) {
        cone_dir_uniform(urand(), urand(), cos_alpha, &x, &y, &z);
        if (z < cos_alpha - 1e-6f) in_cone = 0;
        if (fabsf(x * x + y * y + z * z - 1.0f) > 1e-5f) unit_ok = 0;
    }
    check("all_inside_cone", in_cone);
    check("all_unit_length", unit_ok);

    /* 无偏性数值核对：10 个等立体角环，每环期望 10000，5 sigma 容忍。 */
    enum { RINGS = 10, PER = 100000 };
    int rings_u[RINGS] = {0}, rings_a[RINGS] = {0};
    for (int i = 0; i < PER; i++) {
        cone_dir_uniform(urand(), urand(), cos_alpha, &x, &y, &z);
        int r = (int)((1.0f - z) / (1.0f - cos_alpha) * RINGS);
        if (r >= RINGS) r = RINGS - 1;
        rings_u[r]++;

        cone_dir_angle_uniform(urand(), urand(), alpha, &x, &y, &z);
        r = (int)((1.0f - z) / (1.0f - cos_alpha) * RINGS);
        if (r >= RINGS) r = RINGS - 1;
        rings_a[r]++;
    }
    int uniform_flat = 1, biased_center_heavy = 1;
    for (int j = 0; j < RINGS; j++) {
        if (fabsf((float)rings_u[j] - (float)(PER / RINGS))
            > 5.0f * sqrtf(PER / RINGS))
            uniform_flat = 0;
    }
    /* 有偏采样：中心环显著多于边缘环（理论比 alpha/sin alpha 附近） */
    biased_center_heavy = rings_a[0] > rings_a[RINGS - 1] * 2;
    check("unbiased_rings_flat", uniform_flat);
    check("angle_uniform_center_heavy", biased_center_heavy);
    printf("unbiased center/edge: %d / %d\n", rings_u[0], rings_u[RINGS - 1]);
    printf("biased   center/edge: %d / %d\n", rings_a[0], rings_a[RINGS - 1]);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
