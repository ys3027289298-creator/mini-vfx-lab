#!/usr/bin/env python3
"""Plot the two gain-constraint curves from sweep.csv (produced by `make csv`).

Left panel : bright-tier visibility rate vs exposure gain (floor 0.70).
Right panel: dark-tier overexposed pixel fraction vs gain (ceiling 0.02).
Two policies are compared on every panel: the per-particle adaptive floor and
the single uniform floor. A vertical marker shows the smallest gain at which
the adaptive policy meets both hard constraints.

Run:  python3 tools/plot_curves.py [sweep.csv] [out_stem]
Produces <out_stem>_light.png and <out_stem>_dark.png.
"""
import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import PercentFormatter

# Validated categorical pair (dataviz palette, slot 1 blue / slot 2 orange).
THEMES = {
    "light": {
        "surface": "#fcfcfb",
        "ink": "#0b0b0b",
        "secondary": "#52514e",
        "muted": "#898781",
        "grid": "#e1e0d9",
        "baseline": "#c3c2b7",
        "series": {"adaptive": "#2a78d6", "uniform": "#eb6834"},
    },
    "dark": {
        "surface": "#1a1a19",
        "ink": "#f0efec",
        "secondary": "#c3c2b7",
        "muted": "#898781",
        "grid": "#2c2c2a",
        "baseline": "#383835",
        "series": {"adaptive": "#3987e5", "uniform": "#d95926"},
    },
}
CHOSEN_GAIN = 1.0775  # smallest feasible gain reported by the C self-test
POLICY_LABEL = {"adaptive": "Adaptive per-particle floor",
                "uniform": "Uniform floor"}
VIS_REQ, OVER_LIMIT = 0.70, 0.02


def chosen_gain(rows):
    """Smallest adaptive gain meeting both hard constraints, from the sweep."""
    by_gain = {}
    for r in rows:
        if r["policy"] != "adaptive":
            continue
        d = by_gain.setdefault(r["gain"], {})
        d[r["tier"]] = r
    for g in sorted(by_gain):
        d = by_gain[g]
        if d["bright"]["vis"] >= VIS_REQ and d["dark"]["over"] <= OVER_LIMIT:
            return g
    raise SystemExit("no feasible adaptive gain found in sweep data")


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({
                "gain": float(r["gain"]),
                "policy": r["policy"],
                "tier": r["tier"],
                "vis": float(r["visibility"]),
                "over": float(r["overexposure"]),
            })
    return rows


def series(rows, policy, tier, field):
    pts = sorted((r["gain"], r[field]) for r in rows
                 if r["policy"] == policy and r["tier"] == tier)
    return [p[0] for p in pts], [p[1] for p in pts]


def render(rows, out_path, theme_name):
    t = THEMES[theme_name]
    pick = chosen_gain(rows)
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["system-ui", "Segoe UI", "DejaVu Sans"],
        "text.color": t["ink"],
        "axes.edgecolor": t["baseline"],
        "axes.labelcolor": t["secondary"],
        "xtick.color": t["muted"],
        "ytick.color": t["muted"],
        "axes.titlecolor": t["ink"],
    })
    fig, (ax_v, ax_o) = plt.subplots(1, 2, figsize=(11, 4.2), dpi=150)
    fig.patch.set_facecolor(t["surface"])
    for ax in (ax_v, ax_o):
        ax.set_facecolor(t["surface"])
        ax.set_xscale("log")
        ax.grid(True, color=t["grid"], linewidth=0.8)
        for spine in ax.spines.values():
            spine.set_color(t["baseline"])
        ax.axvline(pick, color=t["muted"], linewidth=1.2,
                   linestyle=(0, (1, 2)), zorder=1)

    # left: bright-tier visibility, requirement floor at 0.70
    ax_v.axhline(0.70, color=t["muted"], linewidth=1.2, linestyle="--", zorder=1)
    ax_v.text(ax_v.get_xlim()[0], 0.715, "requirement  0.70",
              color=t["secondary"], fontsize=8.5, va="bottom")
    for policy in ("adaptive", "uniform"):
        g, v = series(rows, policy, "bright", "vis")
        ax_v.plot(g, v, color=t["series"][policy], linewidth=2.0,
                  label=POLICY_LABEL[policy], zorder=3)
    ax_v.set_title("Bright background (Lb 0.70–0.95): visibility rate",
                   fontsize=10.5)
    ax_v.set_xlabel("Exposure gain")
    ax_v.set_ylabel("Visible particles (rate)")
    ax_v.yaxis.set_major_formatter(PercentFormatter(1.0))
    ax_v.set_ylim(0, 1.02)

    # right: dark-tier overexposure, ceiling at 0.02
    ax_o.axhline(0.02, color=t["muted"], linewidth=1.2, linestyle="--", zorder=1)
    ax_o.text(ax_o.get_xlim()[0], 0.022, "limit  0.02",
              color=t["secondary"], fontsize=8.5, va="bottom")
    for policy in ("adaptive", "uniform"):
        g, o = series(rows, policy, "dark", "over")
        ax_o.plot(g, o, color=t["series"][policy], linewidth=2.0,
                  label=POLICY_LABEL[policy], zorder=3)
    ax_o.set_title("Dark background (Lb 0.02–0.10): overexposed sprite pixels",
                   fontsize=10.5)
    ax_o.set_xlabel("Exposure gain")
    ax_o.set_ylabel("Overexposed pixels (fraction of total)")
    ax_o.yaxis.set_major_formatter(PercentFormatter(1.0))
    ax_o.set_ylim(0, 0.12)

    # annotate the chosen gain once, on the panel where it binds
    ax_v.annotate(f"chosen gain {pick:.3f}",
                  xy=(pick, 0.70), xytext=(pick * 1.25, 0.30),
                  color=t["secondary"], fontsize=8.5,
                  arrowprops=dict(arrowstyle="->", color=t["muted"], lw=1.0))

    handles, labels = ax_v.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False,
               bbox_to_anchor=(0.5, 1.02), fontsize=9.5)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out_path, facecolor=t["surface"], bbox_inches="tight")
    plt.close(fig)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "sweep.csv"
    stem = sys.argv[2] if len(sys.argv) > 2 else "constraint_curves"
    rows = load(src)
    for theme in ("light", "dark"):
        out = f"{stem}_{theme}.png"
        render(rows, out, theme)
        print("wrote", out)


if __name__ == "__main__":
    main()
