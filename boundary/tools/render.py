#!/usr/bin/env python3
"""Render the four boundary-strategy screenshots and the comparison figure.

Inputs  : artifacts/boundary/data/{mode}_final.csv, {mode}_log.csv
Outputs : artifacts/boundary/shot_*.png, comparison.png, out_of_box_timeseries.png
"""
import csv
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

# --- design tokens (validated dark palette, dataviz skill) -------------------
SURFACE   = "#1a1a19"
PAGE      = "#0d0d0d"
INK       = "#f0efec"
INK2      = "#c3c2b7"
MUTED     = "#898781"
GRID      = "#2c2c2a"
COLORS    = {
    "none":    "#d55181",   # magenta — defect state
    "kill":    "#3987e5",   # blue
    "reflect": "#199e70",   # aqua
    "fade":    "#d95926",   # orange
}
TITLES = {
    "none":    "NONE（现状：边界处理为空）",
    "kill":    "KILL（越界即销毁）",
    "reflect": "REFLECT（边界反射）",
    "fade":    "FADE（边缘带淡出，硬边销毁）",
}
BOX = (-4.0, 4.0, -4.0, 4.0)
MARGIN = 1.0

plt.rcParams.update({
    "font.family": ["Noto Sans CJK SC", "sans-serif"],
    "axes.facecolor": SURFACE,
    "figure.facecolor": PAGE,
    "axes.edgecolor": "#383835",
    "axes.labelcolor": INK2,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "text.color": INK,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "savefig.facecolor": PAGE,
})

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(ROOT, "artifacts", "boundary", "data")
OUT  = os.path.join(ROOT, "artifacts", "boundary")


def load_final(mode):
    xs, ys, al = [], [], []
    with open(os.path.join(DATA, f"{mode}_final.csv")) as f:
        for row in csv.DictReader(f):
            xs.append(float(row["x"]))
            ys.append(float(row["y"]))
            al.append(float(row["alpha"]))
    return xs, ys, al


def load_log(mode):
    rows = []
    with open(os.path.join(DATA, f"{mode}_log.csv")) as f:
        for row in csv.DictReader(f):
            rows.append({k: float(v) for k, v in row.items()})
    return rows


def summary(mode):
    with open(os.path.join(DATA, f"{mode}_log.csv")) as f:
        last = list(csv.DictReader(f))[-1]
    return {
        "alive": int(last["alive"]),
        "out": int(last["out_of_box"]),
        "share": float(last["out_share"]),
        "killed": int(last["killed_cum"]),
        "bounces": int(last["bounces_cum"]),
        "max_jump": float(last["max_abs_alpha_jump"]),
        "max_coord": max(float(last["max_x"]), float(last["max_y"])),
    }


def draw_box(ax, margin=False):
    xmin, xmax, ymin, ymax = BOX
    ax.add_patch(Rectangle((xmin, ymin), xmax - xmin, ymax - ymin,
                           fill=False, edgecolor=MUTED, linewidth=1.2,
                           linestyle="--", alpha=0.9, zorder=2))
    if margin:
        band = dict(facecolor=COLORS["fade"], alpha=0.08,
                    edgecolor="none", zorder=1)
        ax.add_patch(Rectangle((xmin, ymin), MARGIN, ymax - ymin, **band))
        ax.add_patch(Rectangle((xmax - MARGIN, ymin), MARGIN, ymax - ymin, **band))
        ax.add_patch(Rectangle((xmin, ymin), xmax - xmin, MARGIN, **band))
        ax.add_patch(Rectangle((xmin, ymax - MARGIN), xmax - xmin, MARGIN, **band))
    ax.plot(0, 0, marker="+", color=MUTED, markersize=9, zorder=3)


def stat_lines(mode):
    s = summary(mode)
    if mode == "none":
        return [f"存活 {s['alive']}  ·  越界仍存活 {s['out']}（{s['share']*100:.1f}%）",
                f"最远坐标 |c| = {s['max_coord']:.1f}（发射范围 ±4）"]
    base = [f"存活 {s['alive']}  ·  越界仍存活 {s['out']}",
            f"边界销毁累计 {s['killed']}  ·  反弹 {s['bounces']}"]
    if mode == "fade":
        base.append(f"相邻帧最大 alpha 跳变 {s['max_jump']:.3f}（无闪烁）")
    return base


def scatter_panel(ax, mode):
    xs, ys, al = load_final(mode)
    c = COLORS[mode]
    if mode == "fade":
        ax.scatter(xs, ys, s=55, c=c, alpha=al, edgecolors="none", zorder=4)
    else:
        ax.scatter(xs, ys, s=55, c=c, edgecolors=SURFACE,
                   linewidths=0.7, zorder=4)
    draw_box(ax, margin=(mode == "fade"))
    ax.set_title(TITLES[mode], fontsize=13, pad=10, loc="left")
    ax.set_aspect("equal", adjustable="box")
    if mode == "none":
        ax.set_xlim(-92, 92)
        ax.set_ylim(-92, 92)
    else:
        ax.set_xlim(-4.6, 4.6)
        ax.set_ylim(-4.6, 4.6)
    ax.set_xlabel("x", fontsize=10)
    ax.set_ylabel("y", fontsize=10)
    txt = "\n".join(stat_lines(mode))
    ax.text(0.03, 0.03, txt, transform=ax.transAxes, fontsize=9.5,
            color=INK2, va="bottom", ha="left", linespacing=1.6,
            bbox=dict(facecolor=PAGE, edgecolor=(1.0, 1.0, 1.0, 0.10),
                      boxstyle="round,pad=0.5"), zorder=5)


def main():
    # four individual screenshots
    for mode in ("none", "kill", "reflect", "fade"):
        fig, ax = plt.subplots(figsize=(7.2, 7.2), dpi=150)
        scatter_panel(ax, mode)
        fig.tight_layout()
        fig.savefig(os.path.join(OUT, f"shot_{mode}.png"))
        plt.close(fig)

    # combined 2x2 comparison
    fig, axes = plt.subplots(2, 2, figsize=(13.5, 12.5), dpi=150)
    for ax, mode in zip(axes.flat, ("none", "kill", "reflect", "fade")):
        scatter_panel(ax, mode)
    fig.suptitle("粒子边界策略对比（同一模拟：250 粒子、同种子、t = 10s）",
                 fontsize=15, y=0.985)
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(os.path.join(OUT, "comparison.png"))
    plt.close(fig)

    # quantitative evidence: out-of-box-alive count over time
    fig, ax = plt.subplots(figsize=(11, 5.2), dpi=150)
    for mode in ("none", "kill", "reflect", "fade"):
        rows = load_log(mode)
        t = [r["time"] for r in rows]
        o = [r["out_of_box"] for r in rows]
        ax.plot(t, o, color=COLORS[mode], linewidth=2.0, label=TITLES[mode])
    # the three handling strategies overlap exactly at zero — one honest
    # annotation instead of three stacked "0" labels
    ax.text(10.12, 201, "NONE = 201", color=COLORS["none"],
            va="center", fontsize=10.5, fontweight="bold")
    ax.text(10.12, 6, "KILL / REFLECT / FADE = 0（全程）", color=INK2,
            va="bottom", fontsize=10)
    ax.set_xlim(0, 12.6)
    ax.set_ylim(-8, 215)
    ax.set_xlabel("时间 t (s)", fontsize=11)
    ax.set_ylabel("存活但越界的粒子数", fontsize=11)
    ax.set_title("越界存活粒子数随时间变化（三种策略全程为 0）",
                 fontsize=13, loc="left", pad=10)
    ax.legend(frameon=False, fontsize=10, loc="upper left")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "out_of_box_timeseries.png"))
    plt.close(fig)

    print("rendered:")
    for name in ("shot_none.png", "shot_kill.png", "shot_reflect.png",
                 "shot_fade.png", "comparison.png",
                 "out_of_box_timeseries.png"):
        p = os.path.join(OUT, name)
        print(" ", p, os.path.getsize(p), "bytes")


if __name__ == "__main__":
    main()
