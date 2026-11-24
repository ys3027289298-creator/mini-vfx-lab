#!/usr/bin/env python3
"""Plot rain attenuation curves and the three-distance validation table."""
import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
SECONDARY = "#52514e"
GRID = "#e1e0d9"
BLUE = "#2a78d6"
ORANGE = "#eb6834"
GOOD = "#0ca30c"


def read_csv(path):
    with Path(path).open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def main(argv):
    if len(argv) != 4:
        print(f"usage: {Path(argv[0]).name} curves.csv tiers.csv out.png",
              file=sys.stderr)
        return 2

    curves = read_csv(argv[1])
    tiers = read_csv(argv[2])
    c = [float(r["c_per_m"]) for r in curves]
    ratio = [100.0 * float(r["ratio_model"]) for r in curves]
    ratio_th = 100.0 * float(curves[0]["ratio_perspective"])
    coverage = [100.0 * float(r["far_coverage"]) for r in curves]
    selected = next(r for r in curves if r["perspective_point"] == "1")
    selected_c = float(selected["c_per_m"])
    selected_ratio = 100.0 * float(selected["ratio_model"])
    selected_cov = 100.0 * float(selected["far_coverage"])

    plt.rcParams.update({
        "font.family": "DejaVu Sans",
        "axes.edgecolor": "#c3c2b7",
        "axes.labelcolor": INK,
        "xtick.color": SECONDARY,
        "ytick.color": SECONDARY,
        "text.color": INK,
    })

    fig = plt.figure(figsize=(11.5, 7.0), dpi=170, facecolor=SURFACE)
    gs = fig.add_gridspec(2, 2, height_ratios=[3.2, 1.05],
                          hspace=0.38, wspace=0.22,
                          left=0.075, right=0.975, top=0.80, bottom=0.075)
    axr = fig.add_subplot(gs[0, 0], facecolor=SURFACE)
    axc = fig.add_subplot(gs[0, 1], facecolor=SURFACE)
    axt = fig.add_subplot(gs[1, :], facecolor=SURFACE)
    axt.axis("off")

    fig.suptitle("Rain perspective attenuation",
                 fontsize=17, fontweight="bold", x=0.075, ha="left", y=0.985)
    fig.text(0.075, 0.925,
             "Geometry fixes c and the far/near ratio; far coverage checks whether the choice reads as rain.",
             fontsize=10.5, color=SECONDARY, ha="left")

    # Ratio panel.
    axr.axhspan(8, 12, color=GOOD, alpha=0.12, linewidth=0)
    axr.axhline(ratio_th, color=INK, linestyle=(0, (5, 3)), linewidth=1.3)
    axr.plot(c, ratio, color=BLUE, linewidth=2.6)
    axr.axvline(selected_c, color=SECONDARY, linestyle=":", linewidth=1.8)
    axr.scatter([selected_c], [selected_ratio], s=70, color=BLUE,
                edgecolor="white", linewidth=1.6, zorder=5)
    axr.annotate("inverse solution\nc = 1/near = 0.25 m$^{-1}$",
                 xy=(selected_c, selected_ratio), xytext=(0.39, 27),
                 fontsize=9.5, color=INK,
                 arrowprops=dict(arrowstyle="->", color=SECONDARY, lw=1.1))
    axr.text(0.80, 12.4, "±20% theory band", color="#006300",
             fontsize=9, ha="right", va="bottom")
    axr.set_title("Far/near size ratio vs attenuation coefficient",
                  fontsize=11.5, fontweight="bold", loc="left", pad=10)
    axr.set_xlabel("near-anchored attenuation c (m$^{-1}$)")
    axr.set_ylabel("far width / near width (%)")
    axr.set_xlim(0, 0.8)
    axr.set_ylim(0, 78)
    axr.grid(axis="y", color=GRID, linewidth=0.8)
    axr.set_axisbelow(True)
    for spine in ("top", "right"):
        axr.spines[spine].set_visible(False)
    handles = [
        Line2D([0], [0], color=BLUE, lw=2.6, label="1 / (1 + cL), L = 36 m"),
        Line2D([0], [0], color=INK, lw=1.3, ls=(0, (5, 3)),
               label="perspective theory = 10%"),
    ]
    axr.legend(handles=handles, frameon=False, fontsize=8.8, loc="upper right")

    # Coverage panel.
    axc.axhspan(4, 20, color=GOOD, alpha=0.12, linewidth=0)
    axc.plot(c, coverage, color=ORANGE, linewidth=2.6)
    axc.axvline(selected_c, color=SECONDARY, linestyle=":", linewidth=1.8)
    axc.scatter([selected_c], [selected_cov], s=70, color=ORANGE,
                edgecolor="white", linewidth=1.6, zorder=5)
    axc.annotate("selected point\n10.5% far coverage",
                 xy=(selected_c, selected_cov), xytext=(0.39, 31),
                 fontsize=9.5, color=INK,
                 arrowprops=dict(arrowstyle="->", color=SECONDARY, lw=1.1))
    axc.text(0.80, 20.8, "coverage target 4–20%", color="#006300",
             fontsize=9, ha="right", va="bottom")
    axc.set_title("Far-plane coverage vs attenuation coefficient",
                  fontsize=11.5, fontweight="bold", loc="left", pad=10)
    axc.set_xlabel("near-anchored attenuation c (m$^{-1}$)")
    axc.set_ylabel("far coverage (%)")
    axc.set_xlim(0, 0.8)
    axc.set_ylim(0, 60)
    axc.grid(axis="y", color=GRID, linewidth=0.8)
    axc.set_axisbelow(True)
    for spine in ("top", "right"):
        axc.spines[spine].set_visible(False)
    axc.legend(handles=[
        Line2D([0], [0], color=ORANGE, lw=2.6,
               label="Poisson streak coverage"),
    ], frameon=False, fontsize=8.8, loc="upper right")

    # Tier table.
    axt.text(0.0, 0.92, "Three visible-distance runs",
             fontsize=11.5, fontweight="bold", transform=axt.transAxes)
    col_labels = ["tier", "near→far (m)", "theory ρ", "measured ρ",
                  "relative error", "A = L/near", "far width (px)",
                  "far coverage", "20% check"]
    cell_text = []
    for r in tiers:
        cell_text.append([
            r["tier"],
            f'{float(r["near_z"]):.0f}→{float(r["far_z"]):.0f}',
            f'{float(r["ratio_theory"]):.3f}',
            f'{float(r["ratio_meas"]):.3f}',
            f'{100*float(r["rel_err"]):.1f}%',
            f'{float(r["A_normalized"]):.0f}',
            f'{float(r["far_width_px"]):.2f}',
            f'{100*float(r["far_coverage"]):.1f}%',
            "PASS" if r["ratio_pass"] == "1" else "FAIL",
        ])
    table = axt.table(cellText=cell_text, colLabels=col_labels,
                      bbox=[0.0, 0.0, 1.0, 0.72], cellLoc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(9.2)
    for (row, col), cell in table.get_celld().items():
        cell.set_edgecolor(GRID)
        cell.set_facecolor(SURFACE)
        if row == 0:
            cell.set_text_props(weight="bold", color=INK)
            cell.set_facecolor("#f0efec")
        elif col == 8:
            cell.set_text_props(weight="bold",
                                color="#006300" if cell_text[row - 1][8] == "PASS"
                                else "#d03b3b")
    out = Path(argv[3])
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, facecolor=fig.get_facecolor(), bbox_inches="tight")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
