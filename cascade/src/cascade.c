/*
 * cascade: 子发射器级联 —— 解析上界、阈值二分、十万粒子单帧计时。
 *
 * 模型
 *   种子粒子每秒 R 颗。每颗粒子在寿命 L 内每隔 d 秒发射 1 个子粒子；
 *   子粒子带父粒子剩余寿命与一条"能量" e，子代能量 e' = rho * e (0<rho<1)。
 *   只有 e > theta 的粒子才发射，因此级联深度被阈值截断。
 *
 * 结论（推导见 README.md）
 *   k       = 每颗可发射粒子的子粒数 = ceil(L/d) - 1
 *   g_max   = 满足 E0*rho^g > theta 的最大层号
 *   单颗种子总数 N <= sum_{g=0}^{g_max+1} k^g = (k^(g_max+2)-1)/(k-1)
 *
 * 实现上把 theta 预先折算成 g_max，避免在能量恰好等于阈值时被浮点舍入
 * 随机地多/少展开一整层（阈值是层级的阶跃函数，一层相差 k 倍）。
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <limits.h>

#define MAXGEN   64
#define MAXCAP   (1 << 20)
#define CHILDCAP (1 << 18)

typedef struct {
    double L, d, rho, E0, theta;
    double rate;   /* 种子粒子发射率，颗/秒 */
    double dt;     /* 固定步长 */
} Params;

typedef struct {
    float x, y, vx, vy;
    float age, life, next_emit;
    int gen;
} P;                /* 32 字节，8 颗/缓存行 */

typedef struct {
    P* p; int cap, n;
    P* child; int child_cap, nc;
    int gmax;
    float theta;
    double rate, rate_acc;
    float life, d;
    int track_gen;
    long long born, dropped;
    long long born_by_gen[MAXGEN];
    double last_us[5];   /* 4 个阶段 + 总耗时 */
} World;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---------- 解析部分 ---------- */

/* k = 恰好在 age < L 时能到达的发射点数 = ceil(L/d)-1 */
static int k_children(const Params* ps) {
    double q = ps->L / ps->d;
    int k = (int)ceil(q - 1e-9) - 1;
    return k < 0 ? 0 : k;
}

/* 最大的可发射层号；-1 表示连种子都不发射 */
static int gmax_for(const Params* ps) {
    if (ps->theta <= 0.0) return INT_MAX;   /* 0 = 不设阈值 */
    double e = ps->E0;
    int gmax = -1;
    for (int g = 0; g < MAXGEN; g++) {
        if (!(e > ps->theta)) break;
        gmax = g;
        e *= ps->rho;
    }
    return gmax;
}

/* 单颗种子的级联总数上界；theta=0 时无界 */
static double per_spark_bound(int k, int gmax) {
    if (gmax >= MAXGEN - 1) return INFINITY;
    if (gmax < 0) return 1.0;               /* 只有种子本身 */
    int levels = gmax + 2;                  /* 0 .. gmax+1 */
    if (k == 1) return (double)levels;
    return (pow((double)k, levels) - 1.0) / (double)(k - 1);
}

/* 层 g 的粒子最早要在 g*d 秒之后才可能出现，所以 T 秒内最多展开到
 * G = floor(T/d) 层；每层节点数 ≤ k^g。这是单颗种子的有限时窗上界。 */
static double bound_within(const Params* ps, int k, int gmax, double T) {
    long long G = (long long)floor(T / ps->d + 1e-9);
    long long last = (long long)gmax + 1;   /* 阈值截断：最深层是 gmax+1 */
    if (last < G) G = last;
    if (G < 0) return 1.0;
    if (G > 1000000) return INFINITY;
    double s = 0.0;
    for (long long g = 0; g <= G; g++) s += pow((double)k, (double)g);
    return s;
}

/* 出生总数 <= 发射率 * 时间 * 单颗种子的级联总数 */
static double total_bound(const Params* ps, int k, int gmax, double T) {
    return ps->rate * T * per_spark_bound(k, gmax);
}

/* 对 theta 二分：找最小的 theta 使单颗种子总数 <= target。
 * N(theta) 单调不增且是阶跃函数；返回可行的最小端点。 */
static double bisect_threshold(const Params* ps, double target, int k,
                               double* out_lo, double* out_hi, int verbose) {
    double lo = 0.0;                        /* 不可行（theta=0 无界） */
    double hi = ps->E0;                     /* 可行：theta>=E0 时只有种子 */
    for (int it = 0; it < 200; it++) {
        double mid = 0.5 * (lo + hi);
        if (mid <= lo || mid >= hi) break;  /* 浮点相邻，收敛 */
        int gm = gmax_for(&(Params){ ps->L, ps->d, ps->rho, ps->E0, mid, 0, 0 });
        double n = per_spark_bound(k, gm);
        if (n <= target) hi = mid; else lo = mid;
        if (verbose && (it < 4 || it % 20 == 19))
            printf("  iter %3d  lo=%.17g  hi=%.17g  mid=%.17g  gmax=%-5d N=%.0f\n",
                   it, lo, hi, mid, gm, n);
    }
    *out_lo = lo; *out_hi = hi;
    return hi;
}

/* ---------- 模拟部分 ---------- */

static int w_init(World* w, const Params* ps, int cap) {
    memset(w, 0, sizeof(*w));
    w->p = (P*)calloc((size_t)cap, sizeof(P));
    w->child = (P*)malloc((size_t)CHILDCAP * sizeof(P));
    if (!w->p || !w->child) { free(w->p); free(w->child); return -1; }
    w->cap = cap;
    w->child_cap = CHILDCAP;
    w->gmax = gmax_for(ps);
    if (w->gmax > MAXGEN - 2) w->gmax = MAXGEN - 2;
    w->theta = (float)ps->theta;
    w->rate = ps->rate;
    w->life = (float)ps->L;
    w->d = (float)ps->d;
    return 0;
}

static void w_free(World* w) { free(w->p); free(w->child); }

static void w_seed(World* w, float x, float y, float vx, float vy) {
    if (w->nc >= w->child_cap) { w->dropped++; return; }
    P* c = &w->child[w->nc++];
    *c = (P){ x, y, vx, vy, 0.0f, w->life, w->d, 0 };
}

/*
 * 一帧：推进/过期 → 子发射 → 种子发射 → 并回粒子池。
 * last_us[0..3] 是四段耗时，last_us[4] 为帧内总耗时。
 */
static int w_step(World* w, float dt) {
    double t0 = now_sec();

    /* 1) 推进 + 过期；swap-remove 保证 [0, n) 全是活粒子 */
    int n = w->n;
    for (int i = 0; i < n; ) {
        P* p = &w->p[i];
        p->age += dt;
        if (p->age >= p->life) {
            w->p[i] = w->p[--n];
            continue;
        }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        i++;
    }
    w->n = n;
    double t1 = now_sec();

    /* 2) 子发射：子粒子先写缓冲，避免遍历中改动池 */
    int nc = 0;
    for (int i = 0; i < n; i++) {
        P* p = &w->p[i];
        if (p->gen > w->gmax) continue;
        float ne = p->next_emit;
        if (ne > p->age) continue;
        while (ne <= p->age && ne < p->life) {
            if (nc >= w->child_cap) { w->dropped++; ne += w->d; continue; }
            P* c = &w->child[nc++];
            c->x = p->x; c->y = p->y;
            c->vx = p->vx; c->vy = p->vy;
            c->age = 0.0f; c->life = p->life; c->next_emit = w->d;
            c->gen = p->gen + 1;
            ne += w->d;
        }
        p->next_emit = ne;
    }
    double t2 = now_sec();

    /* 3) 种子发射（与本帧子粒子共用缓冲，最后一次性并池） */
    w->rate_acc += w->rate * (double)dt;
    int want = (int)w->rate_acc;
    w->rate_acc -= (double)want;
    if (want > 0) {
        if (want > w->child_cap - nc) { w->dropped += want - (w->child_cap - nc); want = w->child_cap - nc; }
        for (int s = 0; s < want; s++) {
            P* c = &w->child[nc++];
            *c = (P){ 0, 0, 0, 1.0f, 0.0f, w->life, w->d, 0 };
        }
    }
    double t3 = now_sec();

    /* 4) 并回粒子池（容量不足则丢弃并计数） */
    int room = w->cap - n;
    int add = nc < room ? nc : room;
    if (add < nc) w->dropped += nc - add;
    if (add > 0) {
        memcpy(&w->p[n], w->child, (size_t)add * sizeof(P));
        if (w->track_gen) {
            for (int i = 0; i < add; i++) {
                int g = w->child[i].gen;
                if (g >= 0 && g < MAXGEN) w->born_by_gen[g]++;
            }
        }
        w->n = n + add;
        w->born += add;
    }
    double t4 = now_sec();

    w->last_us[0] = (t1 - t0) * 1e6;
    w->last_us[1] = (t2 - t1) * 1e6;
    w->last_us[2] = (t3 - t2) * 1e6;
    w->last_us[3] = (t4 - t3) * 1e6;
    w->last_us[4] = (t4 - t0) * 1e6;
    return w->n;
}

/* ---------- 统计小工具 ---------- */

static int cmp_d(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double pct(const double* sorted, int n, double q) {
    if (n <= 0) return 0.0;
    int i = (int)(q * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return sorted[i];
}

/* ---------- 各运行模式 ---------- */

static Params default_params(void) {
    Params ps;
    ps.L = 1.1;
    ps.d = 0.2;
    ps.rho = 0.8;
    ps.E0 = 1.0;
    ps.theta = 0.0;        /* 当前：没有触发阈值 */
    ps.rate = 0.93;
    ps.dt = 1.0 / 120.0;
    return ps;
}

static int analyse(void) {
    Params ps = default_params();
    int k = k_children(&ps);
    printf("params: L=%.6g d=%.6g rho=%.6g E0=%.6g theta=%.6g rate=%.6g dt=1/%.6g\n",
           ps.L, ps.d, ps.rho, ps.E0, ps.theta, ps.rate, 1.0 / ps.dt);
    printf("k = ceil(L/d)-1 = %d  (L/d=%.6f)\n", k, ps.L / ps.d);
    printf("theta=0 -> gmax unlimited -> per-spark bound infinite\n");

    printf("\ndivergence at theta=0 (upper bound within T):\n");
    printf("  %-6s %-6s %-24s %s\n", "T", "G", "N<=sum k^0..k^G", "growth");
    double prev = 0;
    for (double T = 1.0; T <= 3.0001; T += 0.2) {
        int G = (int)floor(T / ps.d + 1e-9);
        double b = bound_within(&ps, k, INT_MAX, T);
        printf("  %-6.2f %-6d %-24.6g", T, G, b);
        if (prev > 0) printf("x%.3g vs prev", b / prev);
        printf("\n");
        prev = b;
    }
    printf("  per 0.2s frontier x%d; per second x%d (~%.2f e-folds/s, doubling %.3fs)\n",
           k, (int)lround(pow((double)k, 1.0 / ps.d)),
           log((double)k) / ps.d, log(2.0) / (log((double)k) / ps.d));
    printf("  at theta=0 the per-spark bound is infinite, so steady-state alive\n");
    printf("  R*L*N is unbounded too; only a finite horizon T gives a finite number.\n");

    printf("\nbisection on theta for per-spark total <= 100000:\n");
    double lo = 0.0, hi = 0.0;
    double target = 100000.0;
    double th = bisect_threshold(&ps, target, k, &lo, &hi, 1);
    int gm = gmax_for(&(Params){ ps.L, ps.d, ps.rho, ps.E0, th, 0, 0 });
    double nb = per_spark_bound(k, gm);
    printf("  theta* = %.17g  (rho^7 = %.17g)\n", th, pow(ps.rho, 7.0));
    printf("  gmax(theta*) = %d, levels = %d, bound = %.0f  (target %.0f)\n",
           gm, gm + 2, nb, target);
    double th_down = nextafter(th, -INFINITY);
    int gm_down = gmax_for(&(Params){ ps.L, ps.d, ps.rho, ps.E0, th_down, 0, 0 });
    printf("  minimality: theta just below (%.17g) -> gmax=%d bound=%.0f (> target)\n",
           th_down, gm_down, per_spark_bound(k, gm_down));

    Params p2 = ps; p2.theta = th;
    int k2 = k_children(&p2);
    printf("\nat theta*: per-spark total <= %.0f for any horizon T\n", nb);
    printf("  seeds at rate R=%.3g/s: spawned in [0,T] <= R*T*N = %.4g*T\n",
           p2.rate, p2.rate * nb);
    int seeds_in_L = (int)floor(p2.rate * p2.L) + 1;
    printf("  alive at any instant <= (%d seeds in last L) * N = %.0f\n",
           seeds_in_L, (double)seeds_in_L * nb);
    printf("  long-run mean alive = R*L*N = %.0f\n", p2.rate * p2.L * nb);
    printf("  birth-count bound at T=3s = R*T*N = %.0f (linear in T, no longer exponential)\n",
           total_bound(&p2, k2, gmax_for(&p2), 3.0));
    printf("  tighter finite-window bound (single spark) at T=3s = %.0f\n",
           bound_within(&p2, k2, gmax_for(&p2), 3.0));
    (void)k2;
    return nb <= target ? 0 : 1;
}

static int verify(void) {
    Params ps = default_params();
    /* 阈值取迭代乘出的 rho^7，与 gmax_for 的能量迭代同一条浮点路径 */
    double e = ps.E0;
    for (int g = 0; g < 7; g++) e *= ps.rho;
    ps.theta = e;
    ps.rate = 0.0;                      /* 单颗种子 */
    int k = k_children(&ps);
    int gm = gmax_for(&ps);
    double bound = per_spark_bound(k, gm);
    printf("verify: theta=%.17g gmax=%d k=%d bound=%.0f\n", ps.theta, gm, k, bound);

    World w;
    if (w_init(&w, &ps, MAXCAP) != 0) return 1;
    w.track_gen = 1;
    w_seed(&w, 0, 0, 0, 1.0f);
    /* 把第一颗种子并进池 */
    w.p[0] = w.child[0]; w.n = 1; w.nc = 0; w.born = 1; w.born_by_gen[0] = 1;

    int peak = 0;
    double T = 0.0;
    for (int f = 0; f < 1200; f++) {   /* 10 s */
        int alive = w_step(&w, (float)ps.dt);
        if (alive > peak) peak = alive;
        T += ps.dt;
        if (alive == 0 && T > 2.0) break;
    }
    printf("  born=%lld  bound=%.0f  dropped=%lld  peak_alive=%d  extinct_at=%.3fs\n",
           w.born, bound, w.dropped, peak, T);
    int ok = (w.born == (long long)llround(bound)) && w.dropped == 0 && peak <= bound;
    for (int g = 0; g <= gm + 1; g++) {
        long long expect = 1;
        for (int i = 0; i < g; i++) expect *= k;
        printf("  gen %d: born=%lld expected=%lld %s\n", g, w.born_by_gen[g], expect,
               w.born_by_gen[g] == expect ? "ok" : "MISMATCH");
        if (w.born_by_gen[g] != expect) ok = 0;
    }
    printf("  check_born_eq_bound=%d check_peak_le_bound=%d\n",
           w.born == (long long)llround(bound), peak <= bound);
    w_free(&w);
    return ok ? 0 : 1;
}

static int bench(void) {
    Params ps = default_params();
    ps.theta = 0.0;
    {
        /* theta* = 迭代 7 次的能量，等价 rho^7 */
        double e = ps.E0;
        for (int i = 0; i < 7; i++) e *= ps.rho;
        ps.theta = e;
    }
    int k = k_children(&ps);
    int gm = gmax_for(&ps);
    double per = per_spark_bound(k, gm);

    World w;
    if (w_init(&w, &ps, MAXCAP) != 0) return 1;
    float dt = (float)ps.dt;
    int fps = 120;
    double warm = 10.0, total_t = 18.0;
    int f0 = (int)lround(warm * fps);
    int f1 = (int)lround(total_t * fps);
    int nf = f1 - f0;
    double* totals = (double*)malloc((size_t)nf * sizeof(double));
    double* ph = (double*)malloc((size_t)nf * 4 * sizeof(double));
    int* alives = (int*)malloc((size_t)nf * sizeof(int));
    double* sortbuf = (double*)malloc((size_t)nf * sizeof(double));

    int peak_alive = 0;
    long long born_at_measure = 0;
    for (int f = 0; f < f1; f++) {
        int alive = w_step(&w, dt);
        if (alive > peak_alive) peak_alive = alive;
        if (f == f0) born_at_measure = w.born;
        if (f >= f0) {
            int j = f - f0;
            totals[j] = w.last_us[4];
            for (int q = 0; q < 4; q++) ph[j * 4 + q] = w.last_us[q];
            alives[j] = alive;
        }
    }

    double sum = 0;
    int amin = INT_MAX, amax = 0;
    for (int i = 0; i < nf; i++) { sum += alives[i]; if (alives[i] < amin) amin = alives[i]; if (alives[i] > amax) amax = alives[i]; }
    double amean = sum / nf;
    double bound_alive = per * (double)((int)floor(ps.rate * ps.L) + 1);

    memcpy(sortbuf, totals, (size_t)nf * sizeof(double));
    qsort(sortbuf, (size_t)nf, sizeof(double), cmp_d);
    double tmax = sortbuf[nf - 1];
    double tmean = 0;
    for (int i = 0; i < nf; i++) tmean += totals[i];
    tmean /= nf;

    printf("bench: theta=rho^7=%.17g gmax=%d k=%d per_spark=%.0f R=%.4g/s dt=1/120\n",
           ps.theta, gm, k, per, ps.rate);
    printf("frames=%d (%.1fs..%.1fs)  alive min=%d mean=%.0f max=%d  bound<=%.0f\n",
           nf, warm, total_t, amin, amean, amax, bound_alive);
    printf("total frame us: mean=%.1f p50=%.1f p95=%.1f p99=%.1f p99.9=%.1f max=%.1f\n",
           tmean, pct(sortbuf, nf, 0.50), pct(sortbuf, nf, 0.95),
           pct(sortbuf, nf, 0.99), pct(sortbuf, nf, 0.999), tmax);
    printf("phase us (mean/max):\n");
    const char* names[4] = { "integrate+expire", "sub-emit", "seed", "append" };
    for (int q = 0; q < 4; q++) {
        double s = 0, m = 0;
        for (int i = 0; i < nf; i++) { s += ph[i * 4 + q]; if (ph[i * 4 + q] > m) m = ph[i * 4 + q]; }
        printf("  %-18s %8.1f / %8.1f\n", names[q], s / nf, m);
    }
    printf("born=%lld dropped=%lld  (births during measurement=%lld)\n",
           w.born, w.dropped, w.born - born_at_measure);
    int ok = (tmax < 4000.0) && (amax <= bound_alive) && (w.dropped == 0) && (amax >= 100000);
    printf("check_frame_max_lt_4ms=%d check_alive_le_bound=%d check_dropped0=%d\n",
           tmax < 4000.0, amax <= bound_alive, w.dropped == 0);
    printf("bench_ok=%d\n", ok);

    free(totals); free(ph); free(alives); free(sortbuf);
    w_free(&w);
    return ok ? 0 : 1;
}

static int selftest(void) {
    int failures = 0;
    Params ps = default_params();
    printf("selftest:\n");
    int k = k_children(&ps);
    printf("  k=%d %s\n", k, k == 5 ? "ok" : "FAIL");
    failures += k != 5;

    double e7 = ps.E0;
    for (int i = 0; i < 7; i++) e7 *= ps.rho;
    Params t = ps; t.theta = e7;
    int gm = gmax_for(&t);
    printf("  gmax(rho^7)=%d %s\n", gm, gm == 6 ? "ok" : "FAIL");
    failures += gm != 6;
    double b = per_spark_bound(k, gm);
    printf("  bound(k=5,gmax=6)=%.0f %s\n", b, (long long)llround(b) == 97656 ? "ok" : "FAIL");
    failures += (long long)llround(b) != 97656;

    t.theta = nextafter(e7, -INFINITY);
    gm = gmax_for(&t);
    printf("  gmax(just below rho^7)=%d bound=%.0f %s\n", gm, per_spark_bound(k, gm),
           per_spark_bound(k, gm) == 488281 ? "ok" : "FAIL");
    failures += per_spark_bound(k, gm) != 488281;

    double lo, hi;
    t.theta = 0;
    double th = bisect_threshold(&t, 100000.0, k, &lo, &hi, 0);
    gm = gmax_for(&(Params){ ps.L, ps.d, ps.rho, ps.E0, th, 0, 0 });
    printf("  bisect theta*=%.17g gmax=%d bound=%.0f %s\n",
           th, gm, per_spark_bound(k, gm),
           (gm == 6 && per_spark_bound(k, gm) <= 100000.0) ? "ok" : "FAIL");
    failures += !(gm == 6 && per_spark_bound(k, gm) <= 100000.0);

    printf("  all_ok=%d\n", failures == 0);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "all";
    int rc = 0;
    if (!strcmp(mode, "selftest") || !strcmp(mode, "all")) rc |= selftest();
    if (!strcmp(mode, "analyse") || !strcmp(mode, "all")) rc |= analyse();
    if (!strcmp(mode, "verify") || !strcmp(mode, "all")) rc |= verify();
    if (!strcmp(mode, "bench") || !strcmp(mode, "all")) rc |= bench();
    if (strcmp(mode, "all") && strcmp(mode, "selftest") && strcmp(mode, "analyse") &&
        strcmp(mode, "verify") && strcmp(mode, "bench")) {
        fprintf(stderr, "usage: %s [all|selftest|analyse|verify|bench]\n", argv[0]);
        return 2;
    }
    return rc;
}
