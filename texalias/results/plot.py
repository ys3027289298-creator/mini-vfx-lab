#!/usr/bin/env python3 -I
"""Render the two sizing curves from results/edge_error.csv.

Left  : jaggedness amplitude e vs texture edge N (area-baked disc, 3 angles).
Right: sampling cost vs N (mem footprint + bilinear taps), with the N that
       brings each angle under 1 px marked.
Palette: dataviz skill categorical slots 1-3, validated all-pairs (light).
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
S = 256
ANGS = (5.0, 30.0, 45.0)
COLORS = {5.0: "#2a78d6", 30.0: "#eb6834", 45.0: "#1baf7a"}
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
MUTED = "#898781"
GRID = "#e1e0d9"
THRESH = "#d03b3b"

rows = []
with open(os.path.join(HERE, "edge_error.csv")) as f:
    for r in csv.DictReader(f):
        if r["kind"] != "sweep" or r["probe"] != "area":
            continue
        rows.append((float(r["angle_deg"]), int(r["N"]), float(r["r"]),
                     float(r["edge_max_px"]), float(r["edge_rms_px"])))

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["system-ui", "Segoe UI", "DejaVu Sans"],
    "text.color": INK, "axes.edgecolor": "#c3c2b7",
    "axes.labelcolor": INK, "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.axisbelow": True, "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
})

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.5, 5.4), dpi=150,
                               gridspec_kw={"width_ratios": [1.15, 1.0]})

# --- panel 1: jaggedness vs texture size -----------------------------------
for ang in ANGS:
    pts = sorted((n, e) for a, n, r, e, rms in rows if a == ang)
    ns = [p[0] for p in pts]
    es = [p[1] for p in pts]
    ax1.plot(ns, es, marker="o", markersize=5, linewidth=2,
             color=COLORS[ang], label=f"theta = {ang:g} deg")
    # direct label at the right end (relief for the sub-3:1 aqua slot)
    ax1.annotate(f"{ang:g} deg", xy=(ns[-1], es[-1]),
                 xytext=(6, 0), textcoords="offset points",
                 va="center", fontsize=9, color=COLORS[ang])

ax1.axhline(1.0, color=THRESH, linewidth=1.6, linestyle="--")
ax1.annotate("1 px acceptability threshold", xy=(32, 1.05),
             fontsize=9, color=THRESH)
ax1.axvline(S, color=MUTED, linewidth=1.2, linestyle=":")
ax1.annotate("N = S (r = 1)", xy=(S, 3.2), fontsize=9, color=MUTED,
             ha="center")
ax1.set_xscale("log", base=2)
ax1.set_yscale("log")
ax1.set_xlabel("texture edge N (texels)")
ax1.set_ylabel("edge error max (screen px)")
ax1.set_title("Jaggedness amplitude vs texture size (S=256)", fontsize=11)
ax1.legend(frameon=False, fontsize=9, loc="lower left")

# mark N_min per angle and annotate
for ang in ANGS:
    pts = sorted((n, e) for a, n, r, e, rms in rows if a == ang)
    nmin = next((n for n, e in pts if e <= 1.0), None)
    if nmin:
        em = next(e for n, e in pts if n == nmin)
        ax1.scatter([nmin], [em], s=110, facecolors="none",
                    edgecolors=COLORS[ang], linewidths=1.8, zorder=5)

# --- panel 2: memory cost vs N, N_min markers (single axis; bilinear tap
# count is a flat 4 and is stated in the title rather than given a scale) ---
ns = sorted({n for _, n, _, _, _ in rows})
mem = [n * n * 4 / 1024.0 for n in ns]          # RGBA8 bytes -> KiB
ax2.plot(ns, mem, marker="s", markersize=5, linewidth=2, color="#256abf",
         label="memory 4*N^2 bytes (RGBA8, KiB)")
ax2.set_xscale("log", base=2)
ax2.set_yscale("log")
ax2.set_xlabel("texture edge N (texels)")
ax2.set_ylabel("memory (KiB)")
ax2.set_title("Sampling cost vs texture size\nbilinear: 4 taps/fetch at "
              "every N; bandwidth grows via cache footprint",
              fontsize=10)

for ang in ANGS:
    pts = sorted((n, e) for a, n, r, e, rms in rows if a == ang)
    nmin = next((n for n, e in pts if e <= 1.0), None)
    if nmin:
        k = nmin * nmin * 4 / 1024.0
        ax2.scatter([nmin], [k], s=110, facecolors="none",
                    edgecolors=COLORS[ang], linewidths=1.8, zorder=5)
        # 5deg and 30deg share N=48: split labels above/below the point
        dy = {5.0: -16, 30.0: 8, 45.0: -14}[ang]
        ax2.annotate(f"Nmin {ang:g}deg={nmin}", xy=(nmin, k),
                     xytext=(8, dy), textcoords="offset points",
                     fontsize=8.5, color=COLORS[ang])

ax2.legend(frameon=False, fontsize=9, loc="lower right")

fig.suptitle("Rotated particle sprite: jaggedness / sampling-cost trade-off",
             fontsize=13, y=0.98)
fig.tight_layout(rect=[0, 0, 1, 0.95], w_pad=4.0)
out = os.path.join(HERE, "sizing_curves.png")
fig.savefig(out)
print("wrote", out)
