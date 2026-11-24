#!/usr/bin/env python3
"""Plot the two isolation-granularity curves from pool_curves.csv.

Panel 1: conflict count vs band granularity (naive interval placement) and
         the measured zero line of the owner-block pool.
Panel 2: peak shared-pool memory ratio M_g/(K*D) vs block granularity,
         with the total-footprint optimum g* marked.
"""
import csv
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.ticker import FuncFormatter, FixedLocator

for p in ("/home/ys/.fonts/NotoSansCJKsc-Regular.otf",):
    try:
        font_manager.fontManager.addfont(p)
    except Exception:
        pass

plt.rcParams.update({
    "font.family": ["Noto Sans CJK SC", "system-ui", "DejaVu Sans"],
    "font.size": 10.5,
    "axes.facecolor": "#fcfcfb",
    "figure.facecolor": "#fcfcfb",
    "axes.edgecolor": "#c3c2b7",
    "axes.labelcolor": "#52514e",
    "text.color": "#0b0b0b",
    "xtick.color": "#52514e",
    "ytick.color": "#52514e",
    "grid.color": "#e1e0d9",
    "grid.linewidth": 0.8,
    "axes.linewidth": 0.9,
    "legend.frameon": True,
    "legend.facecolor": "#fcfcfb",
    "legend.edgecolor": "#e1e0d9",
    "legend.fontsize": 9.5,
})

K_COLORS = {2: "#2a78d6", 4: "#eb6834", 8: "#1baf7a"}
GOOD = "#0ca30c"
CRITICAL = "#d03b3b"
MUTED = "#898781"
GRID_INK = "#52514e"

csv_path, out_png = sys.argv[1], sys.argv[2]
out_svg = sys.argv[3] if len(sys.argv) > 3 else None

rows = []
with open(csv_path, newline="", encoding="utf-8") as f:
    for r in csv.DictReader(f):
        r["K"] = int(r["K"]); r["g"] = int(r["g"])
        for k in ("band_conflicts", "safe_conflicts", "peak_slots",
                  "full_slots", "gstar"):
            r[k] = int(r[k])
        for k in ("slot_ratio", "byte_ratio", "band_mem_ratio"):
            r[k] = float(r[k])
        rows.append(r)

gs = sorted({r["g"] for r in rows})
Ks = [2, 4, 8]


def series(K, key):
    by = {r["g"]: r for r in rows if r["K"] == K}
    return [by[g][key] for g in gs]


fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.6, 6.3), dpi=200)
fig.subplots_adjust(left=0.065, right=0.985, top=0.87, bottom=0.235,
                    wspace=0.17)

# ---------- Panel 1: conflict curve ----------
band_y = {}
for K in Ks:
    y = series(K, "band_conflicts")
    band_y[K] = {g: v for g, v in zip(gs, y)}
    ax1.plot(gs, y, color=K_COLORS[K], lw=2.1, marker="o", ms=5.5,
             markerfacecolor=K_COLORS[K], markeredgecolor="#fcfcfb",
             markeredgewidth=1.1, label=f"静态隔离带, K={K}", zorder=3)

# selective direct labels at separated points
for K, gx, dx, dy, ha in (
        (8, 1, 10, 2, "left"),
        (4, 1, 10, 2, "left"),
        (2, 32, -10, 14, "right")):
    ax1.annotate(f"K={K}", xy=(gx, band_y[K][gx]), xytext=(dx, dy),
                 textcoords="offset points", color=K_COLORS[K],
                 fontweight="bold", fontsize=10.5, ha=ha, va="bottom",
                 bbox=dict(boxstyle="round,pad=0.2", fc="#fcfcfb",
                           ec="none", alpha=0.9))

ax1.plot(gs, [0] * len(gs), color=GOOD, lw=2.2, ls="--", marker="D", ms=6,
         markerfacecolor=GOOD, markeredgecolor="#fcfcfb", markeredgewidth=1.1,
         label="属主块池实测冲突（三档 × 全粒度均为 0）", zorder=4)

ax1.set_xscale("log", base=2)
ax1.xaxis.set_major_locator(FixedLocator(gs))
ax1.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{int(v)}"))
ax1.set_xlim(0.85, 78)
ax1.set_ylim(-1600, 54000)
ax1.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{int(v):,}"))
ax1.grid(True, axis="y")
ax1.grid(True, axis="x", ls=":", alpha=0.7)
ax1.set_xlabel("隔离粒度 g（槽/属主块，log2 刻度）")
ax1.set_ylabel("冲突槽次 C（384 tick 累计）")
ax1.set_title("① 冲突次数曲线：静态隔离带须到 g ≥ D = 64 才归零",
              fontsize=12, fontweight="bold", pad=10, loc="left")

ax1.annotate("g = D = 64 → C = 0\n任意两系统写区间不相交",
             xy=(64, 0), xytext=(9, 16500),
             fontsize=9.5, color="#0b0b0b",
             arrowprops=dict(arrowstyle="->", color=GRID_INK, lw=1.1),
             bbox=dict(boxstyle="round,pad=0.35", fc="#ffffff",
                       ec="#e1e0d9"))
ax1.text(0.03, 0.045,
         "公共基址裸池 (g=0) 复现颜色污染冲突：\n"
         "K=2: 6,758    K=4: 20,274    K=8: 50,224",
         transform=ax1.transAxes, fontsize=9.3, color=CRITICAL,
         bbox=dict(boxstyle="round,pad=0.4", fc="#fdf1f1", ec="#ecc9c9"))
for s in ("top", "right"):
    ax1.spines[s].set_visible(False)
ax1.legend(loc="upper center", bbox_to_anchor=(0.5, -0.155), ncol=2,
           columnspacing=1.4)

# ---------- Panel 2: peak memory ratio curve ----------
for K in Ks:
    y = series(K, "slot_ratio")
    ax2.plot(gs, y, color=K_COLORS[K], lw=2.1, marker="o", ms=5.5,
             markerfacecolor=K_COLORS[K], markeredgecolor="#fcfcfb",
             markeredgewidth=1.1, label=f"共享池槽位比, K={K}", zorder=3)
    by = {r["g"]: r for r in rows if r["K"] == K}
    gstar = next(g for g in gs if by[g]["gstar"] == 1)
    rstar = by[gstar]
    ax2.scatter([gstar], [rstar["slot_ratio"]], marker="*", s=300,
                facecolor=K_COLORS[K], edgecolor="#0b0b0b", linewidth=1.1,
                zorder=5)
    dy = {2: 14, 4: -26, 8: 12}[K]
    dx = {2: 10, 4: 12, 8: 10}[K]
    ax2.annotate(f"g*={gstar}  槽位 {rstar['slot_ratio']*100:.1f}%\n"
                 f"含标签字节 {rstar['byte_ratio']*100:.1f}%",
                 xy=(gstar, rstar["slot_ratio"]), xytext=(dx, dy),
                 textcoords="offset points", fontsize=9.2,
                 color="#0b0b0b",
                 arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.9),
                 bbox=dict(boxstyle="round,pad=0.3", fc="#ffffff",
                           ec="#d8d7d0"))

band = [g / 64.0 for g in gs]
ax2.plot(gs, band, color=MUTED, lw=1.8, ls="--",
         label="静态独占带占用 g/D", zorder=2)
ax2.axhline(1.0, color="#b8b6ad", lw=1.2)
ax2.text(1.05, 1.015, "完全隔离基线 = 100% = K·D 槽（K=2 即单池两倍）",
         fontsize=9.2, color=GRID_INK)

ax2.set_xscale("log", base=2)
ax2.xaxis.set_major_locator(FixedLocator(gs))
ax2.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{int(v)}"))
ax2.set_xlim(0.85, 78)
ax2.set_ylim(0.25, 1.10)
ax2.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v*100:.0f}%"))
ax2.grid(True, axis="y")
ax2.grid(True, axis="x", ls=":", alpha=0.7)
ax2.set_xlabel("属主块粒度 g（槽/块，log2 刻度）")
ax2.set_ylabel("峰值内存 / 完全隔离 K·D")
ax2.set_title("② 峰值内存曲线：细分配 + 属主标签把峰值压到 37.5%–81.3%",
              fontsize=12, fontweight="bold", pad=10, loc="left")
for s in ("top", "right"):
    ax2.spines[s].set_visible(False)
ax2.legend(loc="upper center", bbox_to_anchor=(0.5, -0.155), ncol=2,
           columnspacing=1.4)

fig.suptitle("共享粒子池隔离粒度判据：冲突次数定下界，峰值内存定最优点",
             fontsize=13.5, fontweight="bold", x=0.065, ha="left", y=0.965)

fig.savefig(out_png, bbox_inches="tight", pad_inches=0.22)
print(f"wrote {out_png}")
if out_svg:
    fig.savefig(out_svg, bbox_inches="tight", pad_inches=0.22)
    print(f"wrote {out_svg}")
