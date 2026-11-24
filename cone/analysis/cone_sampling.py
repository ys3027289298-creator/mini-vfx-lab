#!/usr/bin/env python3
"""锥内方向采样的两种方法对比：角度均匀（有偏）vs 立体角均匀（无偏）。

同一样本数 N 各生成一次，投影到球面上按「等立体角环 × 方位角扇区」
分 K 个等立体角桶，对桶计数做 Pearson 卡方均匀性检验。

只用标准库；固定种子，结果可复现。
"""

import math
import random

N = 100_000          # 每种采样、每个锥角的样本数
RINGS = 10           # 极角方向的等立体角环数
PHI_BINS = 4         # 方位角扇区数
K = RINGS * PHI_BINS  # 总桶数
DF = K - 1           # 卡方自由度
P_TARGET = 0.05      # 显著性水平
SEED = 20261125


# ---------- 两种采样器：输出 (theta, phi)，极轴为锥轴 ----------

def sample_angle_uniform(rng, n, cos_a):
    """方法 A：theta 在 [0, alpha] 上按角度均匀（有偏，中心过密）。"""
    alpha = math.acos(cos_a)
    for _ in range(n):
        u, v = rng.random(), rng.random()
        theta = u * alpha
        phi = 2.0 * math.pi * v
        yield theta, phi


def sample_solid_angle_uniform(rng, n, cos_a):
    """方法 B：cos(theta) 均匀（无偏，单位立体角概率恒定）。"""
    one_minus = 1.0 - cos_a
    for _ in range(n):
        u, v = rng.random(), rng.random()
        cos_theta = 1.0 - u * one_minus
        theta = math.acos(cos_theta)
        phi = 2.0 * math.pi * v
        yield theta, phi


# ---------- 分桶与检验 ----------

def bin_counts(samples, cos_a):
    """等立体角分桶：环边界 cos(theta_j) = 1 - j(1-cos alpha)/RINGS。"""
    counts = [[0] * PHI_BINS for _ in range(RINGS)]
    one_minus = 1.0 - cos_a
    for theta, phi in samples:
        r = (1.0 - math.cos(theta)) / one_minus
        ring = min(RINGS - 1, int(r * RINGS))
        sector = min(PHI_BINS - 1, int(phi / (2.0 * math.pi) * PHI_BINS))
        counts[ring][sector] += 1
    return counts


def chi_square_uniform(counts):
    """H0：每个等立体角桶的期望计数都是 N/K。"""
    expected = N / K
    chi2 = 0.0
    for ring in counts:
        for c in ring:
            chi2 += (c - expected) ** 2 / expected
    return chi2


def _gamma_ser_p(a, x, itmax=300, eps=1e-14):
    """正则化下不完全伽马 P(a,x)，级数展开，适用于 x < a+1。"""
    if x <= 0.0:
        return 0.0
    ap, term, s = a, 1.0 / a, 1.0 / a
    for _ in range(itmax):
        ap += 1.0
        term *= x / ap
        s += term
        if abs(term) < abs(s) * eps:
            break
    return s * math.exp(-x + a * math.log(x) - math.lgamma(a))


def _gamma_cf_q(a, x, itmax=300, eps=1e-14):
    """正则化上不完全伽马 Q(a,x)，Lentz 连分式，适用于 x >= a+1。"""
    b = x + 1.0 - a
    c, d = 1e300, 1.0 / b
    h = d
    for i in range(1, itmax + 1):
        an = -i * (i - a)
        b += 2.0
        d = an * d + b
        if abs(d) < 1e-300:
            d = 1e-300
        c = b + an / c
        if abs(c) < 1e-300:
            c = 1e-300
        d = 1.0 / d
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < eps:
            break
    return math.exp(-x + a * math.log(x) - math.lgamma(a)) * h


def gammaincc(a, x):
    """正则化上不完全伽马 Q(a,x) = Γ(a,x)/Γ(a)（stdlib math 无此函数）。"""
    if x <= 0.0:
        return 1.0
    return 1.0 - _gamma_ser_p(a, x) if x < a + 1.0 else _gamma_cf_q(a, x)


def chi2_critical(df, alpha):
    """上侧分位数：gammaincc(df/2, x/2) = alpha 的二分求根。"""
    lo, hi = 0.0, 10.0
    while gammaincc(df / 2.0, hi / 2.0) > alpha:
        hi *= 2.0
    for _ in range(100):
        mid = (lo + hi) / 2.0
        if gammaincc(df / 2.0, mid / 2.0) > alpha:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2.0


def analytic_ring_expectation_a(alpha_deg, cos_a):
    """方法 A 在等立体角环上的解析期望计数 N*(theta_{j+1}-theta_j)/alpha。"""
    alpha = math.radians(alpha_deg)
    out = []
    for j in range(RINGS):
        c_hi = 1.0 - j * (1.0 - cos_a) / RINGS
        c_lo = 1.0 - (j + 1) * (1.0 - cos_a) / RINGS
        out.append(N * (math.acos(c_lo) - math.acos(c_hi)) / alpha)
    return out


# ---------- 跑实验 ----------

def run(alpha_deg, sampler, label):
    cos_a = math.cos(math.radians(alpha_deg))
    rng = random.Random(SEED + alpha_deg * 8 + (0 if sampler == "A" else 1))
    gen = (sample_angle_uniform if sampler == "A"
           else sample_solid_angle_uniform)(rng, N, cos_a)
    counts = bin_counts(gen, cos_a)
    ring_totals = [sum(row) for row in counts]
    chi2 = chi_square_uniform(counts)
    p_value = gammaincc(DF / 2.0, chi2 / 2.0)
    return {
        "label": label,
        "alpha": alpha_deg,
        "center": ring_totals[0],
        "edge": ring_totals[-1],
        "ring_totals": ring_totals,
        "chi2": chi2,
        "p": p_value,
    }


def main():
    critical = chi2_critical(DF, P_TARGET)
    print(f"配置: N={N}, 桶数 K={K} ({RINGS} 等立体角环 x {PHI_BINS} 方位扇区), "
          f"df={DF}, 每桶均匀期望={N // K}")
    print(f"卡方临界值 chi2_0.95({DF}) = {critical:.4f}（p<{P_TARGET} 即拒绝均匀）\n")

    configs = []
    for deg in (15, 30, 45):
        configs.append(run(deg, "A", f"{deg}° 角度均匀（有偏）"))
        configs.append(run(deg, "B", f"{deg}° 立体角均匀（无偏）"))
    # 对照组：整球（alpha=180°），B 退化为标准球面均匀采样
    configs.append(run(180, "A", "180° 角度均匀（对照组）"))
    configs.append(run(180, "B", "180° 立体角均匀（对照组）"))

    print("== 表 1：锥角、边缘桶计数、中间桶计数（等立体角环，环内 4 扇区求和） ==")
    print("| 锥角 | 边缘桶计数 | 中间桶计数 |")
    print("|---|---|---|")
    for r in configs:
        print(f"| {r['label']} | {r['edge']} | {r['center']} |")
    print(f"| 理论期望（无偏时每环） | {N // RINGS} | {N // RINGS} |")

    print("\n== 表 2：卡方均匀性检验 ==")
    print("| 锥角/采样 | 检验量 chi2 | 临界值 | p 值 | 结论 |")
    print("|---|---|---|---|---|")
    for r in configs:
        verdict = "拒绝均匀（有偏）" if r["chi2"] > critical else "不拒绝（均匀）"
        print(f"| {r['label']} | {r['chi2']:.2f} | {critical:.4f} | "
              f"{r['p']:.3g} | {verdict} |")

    print("\n== 密度比 R(theta)=(1-cos alpha)/(alpha·sin theta)（A 相对 B 的单位立体角密度比） ==")
    for deg in (15, 30, 45):
        a = math.radians(deg)
        edge = (1.0 - math.cos(a)) / (a * math.sin(a))
        print(f"alpha={deg:>3}°: 边缘 R(alpha)={edge:.4f}（轴上 R→∞，单调下降）")

    print("\n== 解析核对：方法 A 的环期望计数 vs 实测（alpha=30°） ==")
    cos_a = math.cos(math.radians(30))
    analytic = analytic_ring_expectation_a(30, cos_a)
    measured = next(r for r in configs if r["alpha"] == 30 and "角度" in r["label"])
    print("环 j:  解析期望 / 实测计数")
    for j, (e, m) in enumerate(zip(analytic, measured["ring_totals"])):
        print(f"  {j:>2}: {e:9.1f} / {m}")


if __name__ == "__main__":
    main()
