
/* consistency: 同一份发射参数、同一时长 T，在 30/60/144 三档帧率下对比发射方式。
 *
 * 方案A 按帧发射:   N = k·f·T                   —— 正比于帧率，跨机器不可复现
 * 方案B 时间累积:   N = floor(R·T)              —— 与帧率无关，累积器必须跨帧保存
 * 方案C 定步累积:   N = floor(R·h·floor(T/h))   —— 与帧率无关，h 由一致性上界反解
 *
 * 跨机器相对差上界: delta <= (1/f_min + h)/T + 1/(R·T) <= eps
 * 反解步长:         h   <= eps·T - 1/f_min - 1/R
 * 代入 eps=3%, T=10s, f_min=30, R=60:  h_max = 0.25s （算出来的，不是固定值）
 */
#include <stdio.h>
#include <math.h>

#define EPS  0.03   /* 跨机器相对差预算 3% */
#define T    10.0   /* 同一时长 (s) */
#define RATE 60.0   /* 发射速率 R (个/s) */
#define K    1      /* 方案A 每帧固定 k 个 */

static const double FPS[] = { 30.0, 60.0, 144.0 };
#define NFPS (sizeof(FPS) / sizeof(FPS[0]))
#define FMIN 30.0

/* 方案A：每帧固定 k 个，N = k·f·T */
static long run_per_frame(double f) {
    long n = 0;
    int frames = (int)(T * f);
    for (int i = 0; i < frames; i++) n += K;
    return n;
}

/* 方案B：acc += R·dt，取整发射，余数跨帧保存 */
static long run_accumulator(double f) {
    double acc = 0.0, dt = 1.0 / f;
    long n = 0;
    int frames = (int)(T * f);
    for (int i = 0; i < frames; i++) {
        acc += RATE * dt;
        long want = (long)acc;
        acc -= (double)want;
        n += want;
    }
    return n;
}

/* 方案B 的坏实现：累积器每帧清零（余数没保存），退化为 floor(R/f)·f·T */
static long run_accumulator_unsaved(double f) {
    long n = 0;
    int frames = (int)(T * f);
    for (int i = 0; i < frames; i++)
        n += (long)(RATE / f);
    return n;
}

/* 方案C：定步 h，时间累积器 tau 与发射余数 credit 都跨帧保存 */
static long run_fixed_step(double f, double h) {
    double tau = 0.0, credit = 0.0, dt = 1.0 / f;
    long n = 0;
    int frames = (int)(T * f);
    for (int i = 0; i < frames; i++) {
        tau += dt;
        while (tau >= h) { tau -= h; credit += RATE * h; }
        long want = (long)credit;
        credit -= (double)want;
        n += want;
    }
    return n;
}

static double rel_spread(const long* v) {
    long lo = v[0], hi = v[0];
    double sum = 0.0;
    for (size_t i = 0; i < NFPS; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
        sum += (double)v[i];
    }
    return (double)(hi - lo) / (sum / NFPS);
}

static double rel_dev_ideal(const long* v, double ideal) {
    double worst = 0.0;
    for (size_t i = 0; i < NFPS; i++) {
        double d = fabs((double)v[i] - ideal) / ideal;
        if (d > worst) worst = d;
    }
    return worst;
}

static int failures = 0;
static void check(const char* name, int ok) {
    printf("  %s=%d\n", name, ok ? 1 : 0);
    if (!ok) failures++;
}

/* 打印三档结果，返回跨档相对差 */
static double report(const char* name, long* v, double ideal) {
    double spread = rel_spread(v);
    double dev = rel_dev_ideal(v, ideal);
    printf("%-24s %8ld %8ld %8ld   spread=%7.2f%%  ideal_dev=%6.2f%%  %s\n",
           name, v[0], v[1], v[2], spread * 100.0, dev * 100.0,
           spread <= EPS ? "PASS" : "FAIL");
    return spread;
}

int main(void) {
    double ideal = RATE * T;
    double h_max = EPS * T - 1.0 / FMIN - 1.0 / RATE;
    long v[NFPS];

    printf("ideal N = R*T = %.0f,  h_max = eps*T - 1/f_min - 1/R = %.4f s\n",
           ideal, h_max);
    printf("%-24s %8s %8s %8s\n", "scheme", "30fps", "60fps", "144fps");

    for (size_t i = 0; i < NFPS; i++) v[i] = run_per_frame(FPS[i]);
    double s_a = report("A per-frame k=1", v, ideal);

    for (size_t i = 0; i < NFPS; i++) v[i] = run_accumulator(FPS[i]);
    double s_b = report("B accumulator saved", v, ideal);

    for (size_t i = 0; i < NFPS; i++) v[i] = run_accumulator_unsaved(FPS[i]);
    double s_bu = report("B' accumulator unsaved", v, ideal);

    for (size_t i = 0; i < NFPS; i++) v[i] = run_fixed_step(FPS[i], h_max);
    double s_c = report("C fixed-step h=h_max", v, ideal);

    for (size_t i = 0; i < NFPS; i++) v[i] = run_fixed_step(FPS[i], 0.24);
    double s_c2 = report("C fixed-step h=0.24", v, ideal);

    for (size_t i = 0; i < NFPS; i++) v[i] = run_fixed_step(FPS[i], 0.8);
    report("C fixed-step h=0.8(too coarse)", v, ideal);
    double dev_c3 = rel_dev_ideal(v, ideal);

    /* 门限判定：三档相对差 <= 3%，超了说明累积器没保存（或根本不是累积方案） */
    check("A_fails_gate", s_a > EPS);            /* 按帧发射必然超 3%：N 正比于 f */
    check("B_passes_gate", s_b <= EPS);          /* 累积器保存：三档一致 */
    check("unsaved_detected", s_bu > EPS);       /* 余数没保存会被 3% 门限检出 */
    check("C_passes_gate", s_c <= EPS);
    check("C_under_bound_passes", s_c2 <= EPS);  /* h <= h_max 即满足预算 */
    check("C_over_bound_detected", dev_c3 > EPS);/* h 超界：余量误差吃掉 3% 预算 */
    check("h_max_derived", fabs(h_max - 0.25) < 1e-9);

    printf("all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}
