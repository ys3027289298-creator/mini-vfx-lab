#!/usr/bin/env python3
"""Plot flash tuning curves and the three half-life verification traces."""
from pathlib import Path
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = Path(__file__).resolve().parent

SURFACE = "#fcfcfb"
PAGE = "#f9f9f7"
INK = "#0b0b0b"
SECONDARY_INK = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
AQUA = "#1baf7a"
CRITICAL = "#d03b3b"

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": [
        "Noto Sans CJK SC", "WenQuanYi Zen Hei", "system-ui",
        "-apple-system", "Segoe UI", "DejaVu Sans"
    ],
    "axes.unicode_minus": False,
    "font.size": 10,
    "axes.edgecolor": BASELINE,
    "axes.labelcolor": SECONDARY_INK,
    "axes.titlecolor": INK,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "text.color": INK,
    "axes.facecolor": SURFACE,
    "figure.facecolor": PAGE,
})


def read_curves():
    a, depth, flicker = [], [], []
    with (HERE / "curves.csv").open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            if row["depth"] and row["flicker"]:
                a.append(float(row["amp"]))
                depth.append(float(row["depth"]))
                flicker.append(float(row["flicker"]))
    return map(np.asarray, (a, depth, flicker))


def read_trace():
    rows = {}
    with (HERE / "trace.csv").open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            h = float(row["half_life"])
            rows.setdefault(h, []).append([
                float(row["x"]), float(row["t"]), float(row["main"]),
                float(row["secondary"]), float(row["total"])
            ])
    return {h: np.asarray(v) for h, v in rows.items()}


def style_axis(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(BASELINE)


def main():
    amp, depth, flicker = read_curves()
    traces = read_trace()

    a_star = 0.5928609
    d_star = 0.4
    phi_star = 0.8616629
    a_flicker = 0.6719659

    fig = plt.figure(figsize=(13.2, 8.4), dpi=170)
    gs = fig.add_gridspec(
        2, 3, height_ratios=[1.05, 1.0],
        hspace=0.48, wspace=0.30,
        left=0.065, right=0.985, top=0.875, bottom=0.095
    )
    fig.suptitle("爆炸闪光：主衰减 + 延迟回弹次项", x=0.065, y=0.965,
                 ha="left", fontsize=17, fontweight="650")
    fig.text(0.065, 0.925,
             "次峰时刻 $t_p=3h$，由主项 1/8 亮度反解；三档半衰期均得到深度 0.400，闪烁斜率比 0.862。",
             ha="left", fontsize=10.5, color=SECONDARY_INK)

    axd = fig.add_subplot(gs[0, 0])
    style_axis(axd)
    axd.axhspan(0.30, 0.50, color=BLUE, alpha=0.08, linewidth=0)
    axd.axhline(0.30, color=BASELINE, linewidth=1.0)
    axd.axhline(0.50, color=BASELINE, linewidth=1.0)
    axd.plot(amp, depth, color=BLUE, linewidth=2.2, solid_capstyle="round")
    axd.scatter([a_star], [d_star], s=78, color=AQUA, edgecolor=SURFACE,
                linewidth=2, zorder=5)
    axd.annotate("选定 A=0.593\n深度 D=0.400",
                 xy=(a_star, d_star), xytext=(0.255, 0.535),
                 fontsize=9.2, color=INK,
                 arrowprops=dict(arrowstyle="-", color=MUTED, lw=1.0))
    axd.text(0.805, 0.315, "30–50% 验收带", fontsize=8.8, color=SECONDARY_INK,
             ha="right", va="bottom")
    axd.set_title("回弹深度曲线 D(A)", loc="left", fontsize=12, pad=8)
    axd.set_xlabel("次项幅度 A（主峰=1）")
    axd.set_ylabel("次峰−最小值 / 主峰")
    axd.set_xlim(0.20, 0.80)
    axd.set_ylim(0.0, 0.62)
    axd.set_xticks(np.arange(0.2, 0.81, 0.1))
    axd.set_yticks(np.arange(0.0, 0.61, 0.1))

    axf = fig.add_subplot(gs[0, 1])
    style_axis(axf)
    axf.axhspan(0.0, 1.0, color=AQUA, alpha=0.07, linewidth=0)
    axf.plot(amp, flicker, color=ORANGE, linewidth=2.2, solid_capstyle="round")
    axf.axhline(1.0, color=CRITICAL, linewidth=1.8)
    axf.scatter([a_star], [phi_star], s=78, color=AQUA, edgecolor=SURFACE,
                linewidth=2, zorder=5)
    axf.scatter([a_flicker], [1.0], s=58, color=CRITICAL,
                edgecolor=SURFACE, linewidth=1.8, zorder=5)
    axf.annotate("A=0.672 处越线", xy=(a_flicker, 1.0),
                 xytext=(0.36, 1.18), fontsize=9.0, color=SECONDARY_INK,
                 arrowprops=dict(arrowstyle="-", color=MUTED, lw=1.0))
    axf.annotate("A*=0.593\nΦ=0.862", xy=(a_star, phi_star),
                 xytext=(0.245, 1.23), fontsize=9.2,
                 arrowprops=dict(arrowstyle="-", color=MUTED, lw=1.0))
    axf.text(0.795, 0.965, "闪烁判据 Φ=1", ha="right", va="top",
             fontsize=9.2, color=CRITICAL)
    axf.set_title("闪烁判据曲线 Φ(A)", loc="left", fontsize=12, pad=8)
    axf.set_xlabel("次项幅度 A（主峰=1）")
    axf.set_ylabel("最大上升斜率 / 主项初始衰减斜率")
    axf.set_xlim(0.20, 0.80)
    axf.set_ylim(0.0, 1.42)
    axf.set_xticks(np.arange(0.2, 0.81, 0.1))
    axf.set_yticks(np.arange(0.0, 1.41, 0.2))

    axinfo = fig.add_subplot(gs[0, 2])
    axinfo.set_facecolor(SURFACE)
    axinfo.set_xlim(0, 1)
    axinfo.set_ylim(0, 1)
    axinfo.axis("off")
    for side in ("left", "bottom"):
        axinfo.spines[side].set_visible(False)
    axinfo.add_patch(plt.Rectangle((0.02, 0.03), 0.96, 0.94,
                                   transform=axinfo.transAxes,
                                   facecolor=SURFACE, edgecolor=GRID,
                                   linewidth=1.0))
    axinfo.text(0.09, 0.84, "定幅结果", fontsize=12.5, fontweight="650")
    lines = [
        ("A*", "0.593"),
        ("回弹深度 D", "0.400"),
        ("闪烁斜率 Φ", "0.862 < 1"),
        ("主项三成时刻", "1.737 h"),
        ("次峰时刻 $t_p$", "3.000 h"),
        ("快/中/慢 h", "0.10 / 0.20 / 0.40 s"),
    ]
    y = 0.70
    for label, value in lines:
        axinfo.text(0.09, y, label, fontsize=9.3, color=SECONDARY_INK)
        axinfo.text(0.91, y, value, fontsize=9.6, color=INK, ha="right")
        y -= 0.11
    axinfo.text(0.09, 0.065, "三档 D 均在 0.30–0.50", fontsize=9.4,
                color="#006300", fontweight="600")

    hs = sorted(traces)
    for j, h in enumerate(hs):
        ax = fig.add_subplot(gs[1, j])
        style_axis(ax)
        data = traces[h]
        x, t, main_l, secondary_l, total = data.T

        t30 = h * np.log(1.0 / 0.30) / np.log(2.0)
        tp = 3.0 * h
        ax.axvspan(t30, t.max(), color=AQUA, alpha=0.055, linewidth=0)
        ax.axvline(tp, color=BASELINE, linewidth=1.0)

        ax.plot(t, main_l, color=MUTED, linewidth=1.8, label="主项")
        ax.plot(t, secondary_l, color=ORANGE, linewidth=2.0, label="次项")
        ax.plot(t, total, color=BLUE, linewidth=2.3, label="总亮度")

        # Valley and rebound peak are in normalized windows 1.5h-2.3h and
        # 2.7h-3.1h for the chosen model.
        vmask = (x > 1.5) & (x < 2.3)
        pmask = (x > 2.7) & (x < 3.1)
        iv = np.where(vmask)[0][np.argmin(total[vmask])]
        ip = np.where(pmask)[0][np.argmax(total[pmask])]
        ax.scatter([t[iv]], [total[iv]], s=58, color=BLUE,
                   edgecolor=SURFACE, linewidth=1.8, zorder=5)
        ax.scatter([t[ip]], [total[ip]], s=58, color=BLUE,
                   edgecolor=SURFACE, linewidth=1.8, zorder=5)
        if j == 1:
            ax.annotate("", xy=(t[ip], total[ip]), xytext=(t[ip], total[iv]),
                        arrowprops=dict(arrowstyle="<->", color=INK, lw=1.1))
            ax.text(t[ip] + 0.018 * (t.max() / 6.0),
                    0.5 * (total[iv] + total[ip]),
                    "ΔL=0.400", fontsize=9.2, va="center")
        ax.set_title(f"半衰期 h = {h:.2f} s", loc="left", fontsize=11.5, pad=7)
        ax.set_xlabel("时间 t (s)")
        if j == 0:
            ax.set_ylabel("归一化亮度")
        ax.set_ylim(0, 1.04)
        ax.set_xlim(0, t.max())
        ax.text(tp, 0.98, " $t_p=3h$", fontsize=8.8, color=SECONDARY_INK,
                va="top", ha="left")
        if j == 0:
            ax.legend(loc="upper right", frameon=False, fontsize=8.8,
                      ncol=3, bbox_to_anchor=(1.0, 1.18))

    out = HERE / "flash_tuning.png"
    fig.savefig(out, dpi=170, facecolor=fig.get_facecolor())
    print(out)


if __name__ == "__main__":
    main()
