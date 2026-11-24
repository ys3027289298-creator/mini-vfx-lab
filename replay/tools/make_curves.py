#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Plot the replay consistency vs archive-size curves for the 3-tier lab.

Reads sweep.csv / tiers.csv produced by replay_lab and writes
replay_curves.png/.svg into the artifacts directory.
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FixedFormatter

plt.rcParams["font.family"] = "sans-serif"
plt.rcParams["font.sans-serif"] = ["Noto Sans CJK SC", "WenQuanYi Zen Hei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

ART = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..", "artifacts")
ART = os.path.abspath(ART)

# validated light-mode palette
BLUE, ORANGE, GREEN, RED = "#2a78d6", "#eb6834", "#1baf7a", "#e34948"
INK, INK2, GRID, SURFACE = "#0b0b0b", "#52514e", "#e6e4dc", "#fcfcfb"

# ---- load sweep (64 classification boundaries) ----
sweep = {}
with open(os.path.join(ART, "sweep.csv"), newline="", encoding="utf-8") as f:
    for row in csv.DictReader(f):
        sweep[int(row["mask"])] = {
            "groups": row["groups"],
            "avg": float(row["bytes_avg"]),
            "total": int(row["bytes_total"]),
            "pass": int(row["clips_pass"]),
            "rate": float(row["clip_rate_pct"]),
        }

# ---- load official tiers ----
tiers = []
with open(os.path.join(ART, "tiers.csv"), newline="", encoding="utf-8") as f:
    for row in csv.DictReader(f):
        tiers.append(row)
tier_by_key = {r["tier"]: r for r in tiers}

# Reverse-solve ladder: each rung promotes exactly one criterion-required group.
rungs = [
    (0,  "仅参数\n切点零初始化"),
    (1,  "+rate_acc\n累加相位"),
    (5,  "+emitted/\ndropped"),
    (13, "+em.p[]\n粒子槽记录"),
    (29, "+tr.p[]\n轨迹历史"),
    (61, "+fp[] 初始/边界\n= 反解边界 M*"),
]
rx = list(range(len(rungs)))
rates = [sweep[m]["rate"] for m, _ in rungs]
avgs = [sweep[m]["avg"] for m, _ in rungs]
passes = [sweep[m]["pass"] for m, _ in rungs]

# Pareto frontier over all 64 boundaries (min bytes, max consistency)
pts = [(v["avg"], v["rate"], m) for m, v in sweep.items()]
front = []
for b, r, m in pts:
    dominated = any(b2 <= b and r2 >= r and (b2 < b or r2 > r) for b2, r2, _ in pts)
    if not dominated:
        front.append((b, r, m))
front.sort(key=lambda z: z[0])

fig, ((axr, axs), (axsc, axt)) = plt.subplots(
    2, 2, figsize=(15, 10.6), gridspec_kw={"height_ratios": [1, 1.05]}
)
fig.patch.set_facecolor("white")

def style_ax(ax):
    ax.set_facecolor("white")
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color("#b9b6ac")
    ax.grid(axis="y", color=GRID, lw=0.9)
    ax.set_axisbelow(True)
    ax.tick_params(colors=INK2, labelsize=9.5)

# ---------- panel 1: consistency ladder ("curve 1") ----------
style_ax(axr)
axr.plot(rx, rates, color=INK2, lw=1.8, zorder=2)
for x, r, p in zip(rx, rates, passes):
    ok = p == 24
    axr.scatter([x], [r], marker="o" if ok else "X", s=130 if ok else 95,
                color=GREEN if ok else RED, edgecolor=SURFACE, linewidth=1.2, zorder=3)
    axr.annotate(f"{p}/24", (x, r), textcoords="offset points", xytext=(0, 10),
                 ha="center", fontsize=9.5, color=INK, fontweight="bold" if ok else "normal")
axr.axhline(100, color=GREEN, ls="--", lw=1.3, zorder=1)
axr.annotate("验收线 100%", (0.02, 101.5), fontsize=9.5, color="#0c6b4f")
axr.set_ylim(0, 116)
axr.set_ylabel("回放一致率（严格片段通过率 %）", fontsize=10.5, color=INK)
axr.set_title("① 归类阶梯上的一致率曲线：缺任一必补量，整段回放即分叉",
              fontsize=12, color=INK, loc="left", pad=10, fontweight="bold")
axr.set_xticks(rx)
axr.set_xticklabels([])

# ---------- panel 2: archive-size ladder ("curve 2") ----------
style_ax(axs)
bars = axs.bar(rx, avgs, width=0.62, color=[BLUE] * 5 + [ORANGE], zorder=2)
for x, b in zip(rx, avgs):
    axs.annotate(f"{b:.0f} B", (x, b), textcoords="offset points", xytext=(0, 4),
                 ha="center", fontsize=9, color=INK2)
axs.set_ylim(0, max(avgs) * 1.18)
axs.set_ylabel("存档体积（B / 片段，均值）", fontsize=10.5, color=INK)
axs.set_xticks(rx)
axs.set_xticklabels([lab for _, lab in rungs], fontsize=9.2)
axs.set_title("② 同阶梯的体积曲线：一致率跳变只发生在边界补全的最后一级",
              fontsize=12, color=INK, loc="left", pad=10, fontweight="bold")

# ---------- panel 3: 64-boundary sweep + Pareto ----------
style_ax(axsc)
fail = [(b, r) for b, r, m in pts if r < 100]
okp = [(b, r, m) for b, r, m in pts if r == 100 and m != 61]
axsc.scatter([b for b, _ in fail], [r for _, r in fail], marker="X", s=52,
             color=RED, alpha=0.55, edgecolor=SURFACE, linewidth=0.6,
             label="未达唯一（4 或 5/24）", zorder=2)
axsc.scatter([b for b, _, _ in okp], [r for _, r, _ in okp], marker="o", s=64,
             color=GREEN, edgecolor=SURFACE, linewidth=1.0,
             label="24/24 的归类组合", zorder=3)
fb = [z[0] for z in front]
fr = [z[1] for z in front]
axsc.plot(fb, fr, color=ORANGE, lw=2.0, drawstyle="steps-post", zorder=4,
          label="Pareto 前沿（给定体积最高一致率）")
# reverse-solved boundary
mstar = sweep[61]["avg"]
axsc.scatter([mstar], [100], marker="*", s=520, color=ORANGE, edgecolor=INK,
             linewidth=0.9, zorder=6)
axsc.annotate("反解边界 M*\nmask=0x3D, 1133 B", (mstar, 100),
              textcoords="offset points", xytext=(-118, -34), fontsize=9.5,
              color=INK, arrowprops=dict(arrowstyle="->", color=INK2, lw=1.0))
# other official tiers
s_avg = float(tier_by_key["S_origin"]["bytes_avg"])
l_avg = float(tier_by_key["L_full"]["bytes_avg"])
axsc.scatter([s_avg], [100], marker="D", s=95, color=BLUE, edgecolor=SURFACE,
             linewidth=1.1, zorder=6)
axsc.annotate("S 原点档 607 B", (s_avg, 100), textcoords="offset points",
              xytext=(10, 12), fontsize=9.5, color=BLUE, fontweight="bold")
axsc.scatter([l_avg], [100], marker="s", s=95, color=INK2, edgecolor=SURFACE,
             linewidth=1.1, zorder=6)
axsc.annotate("L 全量档 2541 B", (l_avg, 100), textcoords="offset points",
              xytext=(-96, 14), fontsize=9.5, color=INK2, fontweight="bold")
axsc.set_xlim(420, 2750)
axsc.xaxis.set_major_locator(FixedLocator([500, 750, 1000, 1500, 2000, 2500]))
axsc.xaxis.set_major_formatter(FixedFormatter(["500", "750", "1000", "1500", "2000", "2500"]))
axsc.xaxis.get_offset_text().set_visible(False)
axsc.set_ylim(0, 118)
axsc.set_yticks([0, 25, 50, 75, 100])
axsc.set_xlabel("每片段平均存档字节（线性轴）", fontsize=10.5, color=INK)
axsc.set_ylabel("回放一致率 %", fontsize=10.5, color=INK)
axsc.set_title("③ 64 组归类扫描与 Pareto 反解：最便宜的 100% 边界即 M*",
               fontsize=12, color=INK, loc="left", pad=10, fontweight="bold")
leg = axsc.legend(loc="lower right", fontsize=9, frameon=True, facecolor="white",
                  edgecolor="#d8d5ca")
for t in leg.get_texts():
    t.set_color(INK)

# ---------- panel 4: the three official tiers ----------
style_ax(axt)
keys = ["S_origin", "M_compact", "L_full"]
xlabels = ["S 原点重放档\n参数 + 从零点完整输入流",
           "M 紧凑切点档\n参数 + 活动边界状态(M*)",
           "L 全量快照档\nMAXP/MAXT 定长叶子"]
vals = [float(tier_by_key[k]["bytes_avg"]) for k in keys]
bcols = [BLUE, ORANGE, INK2]
tb = axt.bar(range(3), vals, width=0.56, color=bcols, zorder=2)
for i, (v, k) in enumerate(zip(vals, keys)):
    row = tier_by_key[k]
    axt.annotate(f"{v:.0f} B", (i, v), textcoords="offset points", xytext=(0, 5),
                 ha="center", fontsize=11, color=INK, fontweight="bold")
    axt.annotate(f"{row['clips_pass']}/24 = 100%", (i, v / 2),
                 ha="center", fontsize=11.5, color="white", fontweight="bold")
axt.set_xticks(range(3))
axt.set_xticklabels(xlabels, fontsize=9.6)
axt.set_ylim(0, max(vals) * 1.16)
axt.set_ylabel("平均存档体积 B/clip", fontsize=10.5, color=INK)
axt.set_title("④ 三档存档各跑一次：回放一致率全部 100%",
              fontsize=12, color=INK, loc="left", pad=10, fontweight="bold")

fig.suptitle("mini-vfx-lab · 参数/状态分离与回放边界反解（24 个确定性语料 × 81 个比对帧）",
             fontsize=15.5, color=INK, fontweight="bold", x=0.012, ha="left", y=0.985)
fig.text(0.012, 0.108,
         "分叉量自动命名（留一探针的最早症状）：省略 em.rate_acc → f=1 em.n（整数发射时刻错位）；省略计数 → f=0 em.emitted；",
         fontsize=9.3, color=INK2)
fig.text(0.012, 0.086,
         "省略 em.p[] → em.p[].alive / em.n；省略 tr.p[] → tr.n、tr.p[].age；省略 fp[] → fp[].x。",
         fontsize=9.3, color=INK2)
fig.text(0.012, 0.064,
         "参数不钉记录值时，rate/gx/gy/drag/swirl/max_age/width_base 单变量漂移均为 0/24。",
         fontsize=9.3, color=INK2)
fig.text(0.012, 0.038,
         "数据：artifacts/sweep.csv（64 边界）、tiers.csv（三档）、culprits.csv、drift.csv。",
         fontsize=9.3, color=INK2)
fig.text(0.012, 0.018,
         "比对容差 1e-6；同二进制 IEEE-754 往返，达标档全部叶子逐位可复现。",
         fontsize=9.3, color=INK2)

fig.subplots_adjust(left=0.07, right=0.975, top=0.925, bottom=0.17, hspace=0.45, wspace=0.2)
out_png = os.path.join(ART, "replay_curves.png")
out_svg = os.path.join(ART, "replay_curves.svg")
fig.savefig(out_png, dpi=170, facecolor="white")
fig.savefig(out_svg, facecolor="white")
print("wrote", out_png)
print("wrote", out_svg)
