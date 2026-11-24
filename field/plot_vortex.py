#!/usr/bin/env python3
"""Plot escape/curl trade-off curves produced by field/vortex_lab.c."""
import csv
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / "artifacts"

INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
SURF = "#fcfcfb"
ESCAPE = "#d03b3b"
CURL = "#2a78d6"
GOOD = "#0ca30c"

plt.rcParams.update(
    {
        "font.family": ["Noto Sans CJK SC", "WenQuanYi Zen Hei", "DejaVu Sans"],
        "axes.unicode_minus": False,
        "figure.facecolor": "white",
        "axes.facecolor": SURF,
        "axes.edgecolor": "#c3c2b7",
        "axes.labelcolor": INK2,
        "xtick.color": MUTED,
        "ytick.color": MUTED,
        "text.color": INK,
        "axes.titlecolor": INK,
        "font.size": 10,
    }
)

with (ART / "vortex_curves.csv").open(newline="", encoding="utf-8") as f:
    curve_rows = list(csv.DictReader(f))
for row in curve_rows:
    for key in ("diameter", "q", "omega", "rmax_formula", "escape_rate",
                "n_escape", "n_outer", "tangential_share", "curl_deficit",
                "balance_gap"):
        row[key] = float(row[key])
    row["n_escape"] = int(row["n_escape"])
results = json.loads((ART / "vortex_results.json").read_text())

fig, axes = plt.subplots(1, 3, figsize=(12.2, 4.7), sharey=True)
fig.subplots_adjust(left=0.07, right=0.985, bottom=0.14, top=0.68, wspace=0.10)

for ax, run in zip(axes, results["runs"]):
    size = run["size"]
    d = run["diameter"]
    df = [
        r for r in curve_rows
        if r["size"] == size and 0.35 <= r["q"] <= 1.12
    ]
    qs = [r["q"] for r in df]
    escapes = [100 * r["escape_rate"] for r in df]
    tangential = [100 * r["tangential_share"] for r in df]
    deficits = [100 * r["curl_deficit"] for r in df]

    ax.plot(qs, escapes, color=ESCAPE, lw=2.4,
            label="外圈逃逸率 E" if size == "S" else None)
    ax.plot(qs, tangential, color=CURL, lw=2.0, alpha=0.72,
            label="切向速度占比 T" if size == "S" else None)
    ax.plot(qs, deficits, color=CURL, lw=2.2, ls="--",
            label="切向缺失 1−T（定强度用）" if size == "S" else None)
    ax.axhline(5, color=GOOD, lw=1.5, ls=":",
               label="5% 报告线" if size == "S" else None)

    q = run["q"]
    ax.axvline(q, color=INK2, lw=1.1, alpha=0.55)
    ax.scatter([q], [run["escape_percent"]], s=58, color=ESCAPE,
               edgecolor="white", linewidth=1.4, zorder=4)
    deficit_pct = 100 * (1 - run["tangential_share"])
    ax.scatter([q], [deficit_pct], s=58, color=CURL,
               edgecolor="white", linewidth=1.4, zorder=4)

    if run["n_escape"]:
        ids = ", ".join(run["escape_labels"])
        verdict = f"逃逸 {run['escape_percent']:.1f}%: {ids}"
        verdict_color = ESCAPE if run["over_five_percent"] else INK2
    else:
        verdict = "逃逸 0%：无编号"
        verdict_color = GOOD
    ax.set_title(f"{size}档  D={d:g}\nq*={q:.3f}, Ω={run['omega']:.2f} s⁻¹",
                 fontsize=10.5)
    ax.text(0.04, 0.96, verdict, transform=ax.transAxes, va="top", ha="left",
            fontsize=9.2, color=verdict_color,
            bbox={"boxstyle": "round,pad=0.32", "fc": "white",
                  "ec": GRID, "lw": 0.8, "alpha": 0.94})

    ax.set_xlim(0.35, 1.12)
    ax.set_ylim(-2, 102)
    ax.set_xlabel("强度倍率 q = Ω / √(2a_grip/D)")
    ax.grid(True, color=GRID, lw=0.8)
    ax.set_axisbelow(True)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)

axes[0].set_ylabel("占外圈粒子比例 / 指标值 (%)")
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.855),
           ncol=4, frameon=False, fontsize=9.6, handlelength=2.6,
           columnspacing=1.4)

fig.suptitle("涡流强度取舍：逃逸曲线上升，切向卷曲缺失下降",
             x=0.07, y=0.965, ha="left", fontsize=15, fontweight=650)
out = ART / "vortex_curves.png"
fig.savefig(out, dpi=180)
print(out)
