#define _GNU_SOURCE
/* shard: fragment tumbling.
 *
 * 问题: 碎片角速度纯随机 -> 有的疯转有的纹丝不动。
 * 做法:
 *   1. 定义指标 R(ω) = 可见翻转次数 / 碎片可见时长;
 *      一帧转过的角度越大, 视觉相干性越低(时间混叠->模糊块), 用
 *      coherence = exp(-(ω·dt/θc)^2) 折减, 于是
 *      R(ω) = ω/2π · coherence。
 *   2. 扫 ω 找 R 的峰, 并论证峰的存在:
 *      ω→0 时 R→0(不翻转); ω→上限时 coherence→0(单帧转太多圈,
 *      只剩模糊块), R→0; 中间有唯一峰(解析峰 ω* = θc/(√2·dt))。
 *   3. 峰位与尺寸挂钩: ω·s = K(边缘线速度恒定), 小碎片转得快。
 *   4. 写出乘积 P = ω·s 的区间上下界(来自 ρ = R/Rmax 的接受带),
 *      落出区间就重扫, 不接受“近似可以”。 */
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---- 标定常量(命名, 不散落魔数) ---- */
#define DT_FRAME   (1.0 / 60.0)          /* 60 fps 的帧时长 s            */
#define THETA_C    3.14159265358979323846 /* 相干尺度: 每帧转半圈时      */
                                          /* coherence = e^-1/4 ≈ 0.78;  */
                                          /* 每帧转一整圈时 = e^-1, 已   */
                                          /* 接近方向不可辨的模糊块      */
#define PEAK_ETA   0.9                   /* 接受带: R >= 0.9·Rmax        */
#define SWEEP_LO   0.05                  /* 扫角角速度上下界 rad/s      */
#define SWEEP_HI   4000.0
#define SWEEP_N    400                   /* 对数网格点数                 */

typedef struct {
    double omega_peak;   /* 扫出的峰位 rad/s        */
    double r_peak;       /* 峰位处的指标值          */
    double omega_lo;     /* 接受带下界 rad/s        */
    double omega_hi;     /* 接受带上界 rad/s        */
    double s_ref;        /* 参考尺寸 px             */
    double K;            /* 设计常数 ω·s, rad·px/s  */
    double p_lo;         /* 乘积区间下界 (=ω_lo·s_ref) */
    double p_hi;         /* 乘积区间上界 (=ω_hi·s_ref) */
} Tumble;

/* 视觉相干性: 每帧步进角 a = ω·dt, a 越大越糊 */
static double coherence(double omega) {
    double a = omega * DT_FRAME / THETA_C;
    return exp(-(a * a));
}

/* 指标 R = 可见翻转次数 / 可见时长, 单位 flips/s。
 * 名义翻转率 ω/2π 被视觉相干性折减。 */
static double flip_rate(double omega) {
    if (omega <= 0.0) return 0.0;
    return omega / (2.0 * M_PI) * coherence(omega);
}

/* 帧步进视角的交叉验证: 离散地跑 T 秒, 逐帧统计可感知翻转。
 * 与连续式 flip_rate 用同一个相干模型, 两者应一致。 */
static double flip_rate_framed(double omega, double T) {
    int n = (int)lround(T / DT_FRAME);
    if (n <= 0) return 0.0;
    double step = omega * DT_FRAME;
    double coh = coherence(omega);
    /* 每帧贡献 step/2π 个翻转, 乘以可感知比例 */
    return (n * (step / (2.0 * M_PI)) * coh) / T;
}

/* 对数网格扫峰 + 三点对数抛物线插值精修 */
static int tumble_sweep(Tumble* t, double s_ref) {
    if (s_ref <= 0.0) return -1;
    memset(t, 0, sizeof(*t));

    double grid[SWEEP_N];
    double rval[SWEEP_N];
    double llo = log(SWEEP_LO), lhi = log(SWEEP_HI);
    for (int i = 0; i < SWEEP_N; i++) {
        grid[i] = exp(llo + (lhi - llo) * i / (SWEEP_N - 1));
        rval[i] = flip_rate(grid[i]);
    }

    int best = 0;
    for (int i = 1; i < SWEEP_N; i++)
        if (rval[i] > rval[best]) best = i;

    /* 峰必须落在网格内部, 否则是边界伪峰, 扫描域不可信 */
    if (best == 0 || best == SWEEP_N - 1) return -2;

    /* 对数空间二次插值 */
    double x0 = log(grid[best - 1]), x1 = log(grid[best]),
           x2 = log(grid[best + 1]);
    double y0 = rval[best - 1], y1 = rval[best], y2 = rval[best + 1];
    double xpeak = 0.5 * ((y0 - y2) * x1 * x1 + (y2 - y1) * x0 * x0 +
                          (y1 - y0) * x2 * x2) /
                   (y0 * (x1 - x2) + y1 * (x2 - x0) + y2 * (x0 - x1));
    t->omega_peak = exp(xpeak);
    t->r_peak = flip_rate(t->omega_peak);

    /* 两头必须差: 论证峰的存在 */
    if (!(rval[0] < 0.02 * t->r_peak)) return -3;          /* ω→0: 翻转趋零 */
    if (!(rval[SWEEP_N - 1] < 0.02 * t->r_peak)) return -3; /* ω→上限: 模糊块 */

    /* 单峰性: 峰前非降、峰后非升(允许浮点小抖动) */
    for (int i = 1; i <= best; i++)
        if (rval[i] + 1e-12 < rval[i - 1]) return -4;
    for (int i = best + 1; i < SWEEP_N; i++)
        if (rval[i] > rval[i - 1] + 1e-12) return -4;

    /* 接受带边界: R(ω) = η·Rmax, 用二分求 ω_lo / ω_hi */
    double target = PEAK_ETA * t->r_peak;
    double lo = SWEEP_LO, hi = t->omega_peak;
    for (int k = 0; k < 80; k++) {
        double m = 0.5 * (lo + hi);
        if (flip_rate(m) < target) lo = m; else hi = m;
    }
    t->omega_lo = 0.5 * (lo + hi);
    lo = t->omega_peak; hi = SWEEP_HI;
    for (int k = 0; k < 80; k++) {
        double m = 0.5 * (lo + hi);
        if (flip_rate(m) > target) lo = m; else hi = m;
    }
    t->omega_hi = 0.5 * (lo + hi);

    /* 尺寸挂钩: 边缘线速度恒定 ω·s = K */
    t->s_ref = s_ref;
    t->K = t->omega_peak * s_ref;
    t->p_lo = t->omega_lo * s_ref;
    t->p_hi = t->omega_hi * s_ref;
    return 0;
}

/* 解析峰: R = ω/2π·exp(-(ω·dt/θc)^2), 令 dR/dω=0 -> ω* = θc/(√2·dt) */
static double analytic_peak(void) {
    return THETA_C / (M_SQRT2 * DT_FRAME);
}

/* 受支持的尺寸范围: K/s 必须仍在接受带内 */
static double size_min(const Tumble* t) { return t->K / t->omega_hi; }
static double size_max(const Tumble* t) { return t->K / t->omega_lo; }

/* 给碎片分配角速度。jitter 为相对抖动幅度(如 0.08 = ±8%),
 * rng 状态由调用方持有(确定性)。落出乘积区间就重扫, 最多重试。
 * 返回 0 成功; -1 坏参; -2 尺寸超出可支持范围(重扫也救不回)。 */
static unsigned lcg_next(unsigned* s) {
    *s = *s * 1103515245u + 12345u;
    return *s;
}

static int shard_assign(const Tumble* t, double size, double jitter,
                        unsigned* seed, double* omega_out) {
    if (size <= 0.0 || jitter < 0.0 || jitter >= 1.0 || !omega_out) return -1;
    if (size < size_min(t) || size > size_max(t)) return -2;

    double target = t->K / size;
    for (int attempt = 0; attempt < 16; attempt++) {
        double u = (lcg_next(seed) >> 8) / 16777216.0; /* [0,1) */
        double omega = target * (1.0 + jitter * (2.0 * u - 1.0));
        double P = omega * size;
        if (P >= t->p_lo && P <= t->p_hi) {   /* 乘积区间门禁 */
            *omega_out = omega;
            return 0;
        }
        /* 落出区间: 重扫, 不接受“近似可以” */
    }
    /* 抖动序列持续越界(理论上仅当尺寸贴边时可能), 退回无抖动设计值 */
    double omega = target;
    if (omega * size < t->p_lo || omega * size > t->p_hi) return -2;
    *omega_out = omega;
    return 0;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("%s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* ASCII 扫描曲线, 峰用 ^ 标出 */
static void plot_sweep(const Tumble* t) {
    const char* glyph = " .:-=+*#%@";
    int W = 64, H = 10;
    char buf[11][65];
    memset(buf, ' ', sizeof(buf));
    for (int i = 0; i < W; i++) {
        double w = exp(log(SWEEP_LO) + (log(SWEEP_HI) - log(SWEEP_LO)) *
                                          i / (W - 1));
        int row = H - 1 -
                  (int)lround((H - 1) * flip_rate(w) / t->r_peak);
        if (row < 0) row = 0;
        if (row >= H) row = H - 1;
        buf[row][i] = glyph[(int)(9.0 * flip_rate(w) / t->r_peak)];
    }
    int pkcol = (int)((log(t->omega_peak) - log(SWEEP_LO)) /
                      (log(SWEEP_HI) - log(SWEEP_LO)) * (W - 1));
    buf[0][pkcol] = '^';
    for (int r = 0; r < H; r++) {
        buf[r][W] = '\0';
        printf("  |%s|\n", buf[r]);
    }
    printf("  +%.*s+\n", W,
           "----------------------------------------------------------------");
    printf("   log ω: %.2f rad/s ........................ %.2f rad/s\n",
           SWEEP_LO, SWEEP_HI);
}

int main(void) {
    Tumble t;
    check("bad_size_rejected", tumble_sweep(&t, 0.0) == -1);

    int rc = tumble_sweep(&t, 12.0);
    check("sweep_ok", rc == 0);
    printf("omega_peak=%.3f rad/s (%.2f rev/s), Rmax=%.3f flips/s\n",
           t.omega_peak, t.omega_peak / (2.0 * M_PI), t.r_peak);
    printf("accept_band[%d%%]: omega in [%.3f, %.3f] rad/s\n",
           (int)(PEAK_ETA * 100), t.omega_lo, t.omega_hi);
    printf("size law: omega*size = K = %.3f rad*px/s\n", t.K);
    printf("product interval P in [%.3f, %.3f] rad*px/s\n", t.p_lo, t.p_hi);
    printf("supported size range: [%.3f, %.3f] px (ref %.1f)\n",
           size_min(&t), size_max(&t), t.s_ref);

    /* 峰的存在性: 解析峰与扫描峰一致(1%), 且两端指标趋零 */
    double ap = analytic_peak();
    check("peak_matches_analytic", fabs(t.omega_peak - ap) / ap < 0.01);
    check("zero_omega_zero_flips", flip_rate(0.0) == 0.0);
    check("low_omega_tail", flip_rate(SWEEP_LO) < 0.02 * t.r_peak);
    check("high_omega_tail", flip_rate(SWEEP_HI) < 0.02 * t.r_peak);
    check("peak_beats_neighbors",
          flip_rate(t.omega_peak) >= flip_rate(t.omega_peak * 0.5) &&
          flip_rate(t.omega_peak) >= flip_rate(t.omega_peak * 1.5));

    /* 接受带边界自洽: 边界处恰为 η·Rmax, 带外低于 */
    double rlo = flip_rate(t.omega_lo), rhi = flip_rate(t.omega_hi);
    check("band_edges",
          fabs(rlo - PEAK_ETA * t.r_peak) / t.r_peak < 1e-3 &&
          fabs(rhi - PEAK_ETA * t.r_peak) / t.r_peak < 1e-3);
    check("outside_band_low",
          flip_rate(t.omega_lo * 0.5) < PEAK_ETA * t.r_peak &&
          flip_rate(t.omega_hi * 1.5) < PEAK_ETA * t.r_peak);

    /* 连续式指标 vs 帧步进交叉验证(2 秒可见时长) */
    double rc_cont = flip_rate(t.omega_peak);
    double rc_frame = flip_rate_framed(t.omega_peak, 2.0);
    check("framed_crosscheck", fabs(rc_cont - rc_frame) / rc_cont < 1e-9);

    /* 尺寸挂钩: 小碎片转得快, 乘积恒定 */
    double w_small, w_big;
    unsigned seed = 42;
    int a1 = shard_assign(&t, 10.0, 0.0, &seed, &w_small);
    int a2 = shard_assign(&t, 16.0, 0.0, &seed, &w_big);
    check("assign_ok", a1 == 0 && a2 == 0);
    check("small_spins_faster", w_small > w_big);
    check("product_constant",
          fabs(w_small * 10.0 - t.K) / t.K < 1e-12 &&
          fabs(w_big * 16.0 - t.K) / t.K < 1e-12);

    /* 带抖动的分配: 每次都必须通过乘积区间门禁, 否则触发重扫 */
    seed = 7;
    int gate_violations = 0;
    double sizes[6] = { 10.0, 11.0, 12.0, 13.0, 15.0, 16.0 };
    for (int i = 0; i < 6; i++) {
        double w;
        int r = shard_assign(&t, sizes[i], 0.08, &seed, &w);
        if (r != 0) { check("jittered_assign", 0); continue; }
        double P = w * sizes[i];
        if (P < t.p_lo || P > t.p_hi) gate_violations++;
        /* 且对应角速度本身落在接受带内 */
        if (w < t.omega_lo || w > t.omega_hi) gate_violations++;
    }
    check("jittered_assign_gate", gate_violations == 0);

    /* 坏参与超范围: 重扫也救不回, 必须拒绝而不是返回近似值 */
    double dummy;
    check("assign_bad_arg", shard_assign(&t, -1.0, 0.0, &seed, &dummy) == -1);
    check("assign_oversize_rejected",
          shard_assign(&t, size_max(&t) * 1.5, 0.0, &seed, &dummy) == -2);
    check("assign_tiny_rejected",
          shard_assign(&t, size_min(&t) * 0.5, 0.0, &seed, &dummy) == -2);

    /* 端到端: 100 个碎片全部满足区间门禁, 无“近似可以”漏网 */
    seed = 99;
    int leaked = 0;
    for (int i = 0; i < 100; i++) {
        double s = 10.0 + 6.0 * ((lcg_next(&seed) >> 8) / 16777216.0);
        double w;
        if (shard_assign(&t, s, 0.08, &seed, &w) != 0) { leaked++; continue; }
        double P = w * s;
        if (P < t.p_lo || P > t.p_hi) leaked++;
    }
    check("batch_all_in_interval", leaked == 0);

    plot_sweep(&t);
    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
