#!/usr/bin/env python3
"""Plot the coefficient trade-off curves for the depth module.

Usage:
    python3 plot_curves.py SWEEP.csv OUT_PREFIX

Writes OUT_PREFIX_light.png and OUT_PREFIX_dark.png. The chart is a 2x2
small-multiples figure (never a dual axis):

  1. overlap-region contrast CR(k) per depth-of-field tier (log scale)
  2. relative gap between measured CR and the N-factor product bound
  3. total visibility fraction (hard gate >= 0.50)
  4. far-depth-band visibility fraction (the "far visibility" curve)

The CSV is the accessible table view of every point drawn.
"""
import csv
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

plt.rcParams["font.sans-serif"] = ["Noto Sans CJK SC", "WenQuanYi Zen Hei",
                                  "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

TIERS = ["shallow", "standard", "deep"]
TIER_CN = {"shallow": "浅景深 D=3", "standard": "中景深 D=6", "deep": "深景深 D=12"}
RHO_STEP = 1.16
LAYERS = 8
GAP_LIMIT = 0.30
VIS_LIMIT = 0.50


def read_sweep(path):
    rows = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            for key in ("k", "depth_span", "product_bound", "actual_contrast",
                        "contrast_gap", "natural_gap", "visibility",
                        "far_visibility", "step_ratio"):
                row[key] = float(row[key])
            row["visible"] = int(row["visible"])
            row["total"] = int(row["total"])
            rows[row["mode"]].append(row)
    for tier in rows:
        rows[tier].sort(key=lambda r: r["k"])
    return rows


def selected_k(rows):
    for r in rows[TIERS[0]]:
        if int(r["contrast_ok"]) == 1 and int(r["gates_ok"]) == 1:
            return r["k"]
    return None


def first_gate_failure(rows):
    for r in rows[TIERS[0]]:
        if int(r["gates_ok"]) == 0:
            return r["k"]
    return None


def style(dark):
    if dark:
        return {
            "fig": "#1a1a19", "ax": "#1a1a19", "ink": "#f0efec",
            "secondary": "#c3c2b7", "muted": "#898781", "grid": "#2c2c2a",
            "baseline": "#383835",
            "tiers": ["#3987e5", "#d95926", "#199e70"],
            "critical": "#e66767",
        }
    return {
        "fig": "#fcfcfb", "ax": "#fcfcfb", "ink": "#0b0b0b",
        "secondary": "#52514e", "muted": "#898781", "grid": "#e1e0d9",
        "baseline": "#c3c2b7",
        "tiers": ["#2a78d6", "#eb6834", "#1baf7a"],
        "critical": "#d03b3b",
    }


def render(rows, kstar, kfail, dark, out_path):
    c = style(dark)
    plt.rcParams.update({
        "text.color": c["ink"], "axes.labelcolor": c["secondary"],
        "xtick.color": c["muted"], "ytick.color": c["muted"],
        "font.size": 10.5,
    })
    fig, axs = plt.subplots(2, 2, figsize=(13.6, 9.4), dpi=170)
    fig.patch.set_facecolor(c["fig"])

    xs = [r["k"] for r in rows[TIERS[0]]]
    cr_target = RHO_STEP ** (LAYERS - 1)

    for ax in axs.flat:
        ax.set_facecolor(c["ax"])
        ax.grid(True, color=c["grid"], linewidth=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(c["baseline"])
        ax.axvline(kstar, color=c["secondary"], linewidth=1.2, linestyle="-.",
                   zorder=1)

    # -- panel 1: overlap contrast vs k -----------------------------------
    ax = axs[0, 0]
    for tier, color in zip(TIERS, c["tiers"]):
        x = [r["k"] for r in rows[tier]]
        y = [r["actual_contrast"] for r in rows[tier]]
        b = [r["product_bound"] for r in rows[tier]]
        ax.plot(x, b, color=color, linewidth=1.3, linestyle=":", alpha=0.75,
                zorder=2)
        ax.plot(x, y, color=color, linewidth=2.2, zorder=3, label=TIER_CN[tier])
        # colored end marker + ink direct label (text token, not series color)
        ax.scatter([x[-1]], [y[-1]], s=46, color=color, edgecolor=c["ax"],
                   linewidth=1.6, zorder=4)
        ax.annotate(TIER_CN[tier].split()[0], xy=(x[-1], y[-1]),
                    xytext=(7, 0), textcoords="offset points", va="center",
                    fontsize=10, color=c["ink"], fontweight="bold")
    ax.axhline(cr_target, color=c["muted"], linewidth=1.3, linestyle="--")
    ax.text(0.012, cr_target * 1.12,
            f"步进可辨目标 ρ^(N−1) = {cr_target:.2f}",
            color=c["secondary"], fontsize=9.5,
            bbox=dict(facecolor=c["ax"], edgecolor="none", alpha=0.85,
                      pad=1.5))
    ax.set_yscale("log")
    ax.set_xlim(0, 0.93)
    ax.set_ylim(0.8, 4e4)
    ax.set_xlabel("深度系数 κ")
    ax.set_ylabel("最亮 / 最暗对比度 CR（对数轴）")
    ax.set_title("① 重叠区亮度对比随系数上升", color=c["ink"],
                 fontweight="bold", loc="left", pad=8)
    bound_handle = Line2D([0], [0], color=c["muted"], lw=1.4, ls=":",
                          label="N 因子连乘理论上界（点线）")
    handles, labels = ax.get_legend_handles_labels()
    handles.append(bound_handle)
    labels.append(bound_handle.get_label())
    leg = ax.legend(handles=handles, labels=labels, loc="upper left",
                    frameon=False, fontsize=9.2)
    for t in leg.get_texts():
        t.set_color(c["secondary"])

    # -- panel 2: gap to the product bound --------------------------------
    ax = axs[0, 1]
    if kfail is not None:
        ax.axvspan(kfail, xs[-1] + 0.02, color=c["critical"], alpha=0.07,
                   zorder=0)
    for tier, color in zip(TIERS, c["tiers"]):
        x = [r["k"] for r in rows[tier]]
        g = [100.0 * r["contrast_gap"] for r in rows[tier]]
        ng = [100.0 * r["natural_gap"] for r in rows[tier]]
        ax.plot(x, g, color=color, linewidth=2.2, label=TIER_CN[tier])
        ax.plot(x, ng, color=color, linewidth=1.2, linestyle="--", alpha=0.65)
    ax.axhline(100.0 * GAP_LIMIT, color=c["critical"], linewidth=1.7,
               linestyle="--")
    ax.text(0.012, 31.5, "判据线 30%", color=c["critical"], fontsize=10,
            fontweight="bold")
    ax.annotate("自然抖动栈（虚线）与实线基本重合", xy=(0.34, 88),
                color=c["muted"], fontsize=9,
                bbox=dict(facecolor=c["ax"], edgecolor="none", alpha=0.85,
                          pad=1.5))
    ax.set_xlim(0, xs[-1])
    ax.set_ylim(0, 102)
    ax.set_xlabel("深度系数 κ")
    ax.set_ylabel("|实测 CR − 上界| / 上界（%）")
    ax.set_title("② 实际对比度与连乘上界的差距", color=c["ink"],
                 fontweight="bold", loc="left", pad=8)
    leg = ax.legend(loc="upper left", frameon=False, fontsize=9.2)
    for t in leg.get_texts():
        t.set_color(c["secondary"])

    # -- panel 3: total visibility ----------------------------------------
    ax = axs[1, 0]
    ax.axhspan(0.0, VIS_LIMIT, color=c["critical"], alpha=0.07, zorder=0)
    for tier, color in zip(TIERS, c["tiers"]):
        x = [r["k"] for r in rows[tier]]
        v = [r["visibility"] for r in rows[tier]]
        ax.plot(x, v, color=color, linewidth=2.2, label=TIER_CN[tier])
    ax.axhline(VIS_LIMIT, color=c["critical"], linewidth=1.7, linestyle="--")
    ax.text(0.012, 0.515, "硬下限：可见数 ≥ 总量一半", color=c["critical"],
            fontsize=10, fontweight="bold")
    ax.set_xlim(0, xs[-1])
    ax.set_ylim(0.25, 1.03)
    ax.set_xlabel("深度系数 κ")
    ax.set_ylabel("可见粒子数 / 总粒子数（每档 512）")
    ax.set_title("③ 总可见度曲线", color=c["ink"], fontweight="bold",
                 loc="left", pad=8)
    leg = ax.legend(loc="lower left", frameon=False, fontsize=9.2)
    for t in leg.get_texts():
        t.set_color(c["secondary"])

    # -- panel 4: far-band visibility -------------------------------------
    ax = axs[1, 1]
    for tier, color in zip(TIERS, c["tiers"]):
        x = [r["k"] for r in rows[tier]]
        f = [r["far_visibility"] for r in rows[tier]]
        ax.plot(x, f, color=color, linewidth=2.2, label=TIER_CN[tier])
    deep_kstar = next(r for r in rows["deep"] if abs(r["k"] - kstar) < 1e-9)
    ax.scatter([kstar], [deep_kstar["far_visibility"]], s=52,
               color=c["tiers"][2], edgecolor=c["ax"], linewidth=1.6, zorder=5)
    ax.annotate(f"κ*=0.35 时深档远端带仅剩 "
                f"{deep_kstar['far_visibility']*100:.0f}%",
                xy=(kstar, deep_kstar["far_visibility"]),
                xytext=(12, 26), textcoords="offset points",
                fontsize=9.6, color=c["ink"],
                arrowprops=dict(arrowstyle="->", color=c["muted"], lw=1.1))
    ax.set_xlim(0, xs[-1])
    ax.set_ylim(-0.03, 1.05)
    ax.set_xlabel("深度系数 κ")
    ax.set_ylabel("远端 1/4 深度带的可见比例")
    ax.set_title("④ 远端可见度曲线（系数的代价）", color=c["ink"],
                 fontweight="bold", loc="left", pad=8)
    leg = ax.legend(loc="lower left", frameon=True, fontsize=9.2,
                    facecolor=c["ax"], edgecolor=c["grid"], framealpha=0.92)
    for t in leg.get_texts():
        t.set_color(c["secondary"])

    fig.suptitle("深度衰减系数取舍：重叠区对比度 vs 远端可见度"
                 "　|　选定 κ* = %.2f（点划线），首个硬门失效 κ = %.2f"
                 % (kstar, kfail if kfail is not None else float("nan")),
                 color=c["ink"], fontsize=14.5, fontweight="bold", x=0.012,
                 ha="left", y=0.985)
    fig.text(0.012, 0.006,
             "模型 a(Δz)=e^(−κΔz)，ε=0.0296（sRGB 48 码），N=8 层；"
             "确定性场景，数据见 sweep.csv",
             color=c["muted"], fontsize=9)
    fig.tight_layout(rect=(0, 0.02, 1, 0.965))
    fig.savefig(out_path, facecolor=fig.get_facecolor(), bbox_inches="tight")
    plt.close(fig)


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    rows = read_sweep(argv[1])
    kstar = selected_k(rows)
    kfail = first_gate_failure(rows)
    if kstar is None:
        raise SystemExit("no admissible coefficient in sweep")
    prefix = argv[2]
    render(rows, kstar, kfail, False, prefix + "_light.png")
    render(rows, kstar, kfail, True, prefix + "_dark.png")
    print("wrote %s_light.png %s_dark.png (kstar=%.2f, first_fail=%s)"
          % (prefix, prefix, kstar, kfail))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
