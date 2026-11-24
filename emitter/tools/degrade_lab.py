#!/usr/bin/env python3
"""Numerical lab for smooth adaptive emission degradation.

The model is intentionally small and explicit:

    dN/dt = rate * scale(N) - N / life

with a power-shaped retention curve.  The CSV/PNG outputs make the steepness
trade-off and the three emission-rate runs reproducible.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

CAPACITY = 64
LIFE_S = 1.0
DT = 1.0 / 60.0
ONSET = 0.75
RATES = (64.0, 128.0, 256.0)
GAMMA_GRID = [0.5 * i for i in range(1, 9)]  # 0.5 .. 4.0
GAMMA_SELECTED = 2.5
BUDGET = 56.0
SETTLE_EPS = 0.5
RUN_FRAMES = 1200


def clamp01(x: float) -> float:
    return max(0.0, min(1.0, x))


def approach(z: float, onset: float = ONSET) -> float:
    """Normalized approach from onset occupancy to capacity."""
    return clamp01((z - onset) / (1.0 - onset))


def retention(z: float, gamma: float, onset: float = ONSET) -> float:
    """Fraction of nominal emission retained at occupancy z=N/K."""
    if z <= onset:
        return 1.0
    return max(0.0, (1.0 - approach(z, onset)) ** gamma)


def cut_fraction(z: float, gamma: float, onset: float = ONSET) -> float:
    return 1.0 - retention(z, gamma, onset)


def steady_z(rho: float, gamma: float, onset: float = ONSET) -> float:
    """Solve z = rho * retention(z, gamma) by bisection."""
    if rho <= onset:
        return rho
    lo, hi = onset, 1.0
    for _ in range(100):
        mid = (lo + hi) * 0.5
        if mid < rho * retention(mid, gamma, onset):
            lo = mid
        else:
            hi = mid
    return (lo + hi) * 0.5


def required_gamma(rho_max: float, z_target: float, onset: float = ONSET) -> float:
    """Inverse steady-state equation at one chosen design point."""
    u = (z_target - onset) / (1.0 - onset)
    return math.log(z_target / rho_max) / math.log(1.0 - u)


def sparsity(gamma: float, u_end: float = 0.5) -> float:
    """Mean emission cut over u in [0, u_end], where u_end maps to BUDGET."""
    integral_cut = u_end - (1.0 - (1.0 - u_end) ** (gamma + 1.0)) / (gamma + 1.0)
    return integral_cut / u_end


def fluid_run(rate: float, gamma: float, frames: int = RUN_FRAMES) -> dict:
    """Euler frame recurrence for the smooth population model."""
    rho = rate * LIFE_S / CAPACITY
    n_ss = steady_z(rho, gamma) * CAPACITY
    z = 0.0
    history: list[float] = []
    start = None
    settle = None

    for f in range(frames):
        scale = retention(z, gamma)
        if start is None and scale < 1.0 - 1e-12:
            start = f

        history.append(z * CAPACITY)
        z = max(0.0, z + DT * (rate / CAPACITY * scale - z / LIFE_S))

    assert start is not None
    for f in range(start, frames):
        if all(abs(n - n_ss) <= SETTLE_EPS for n in history[f:]):
            settle = f
            break
    assert settle is not None

    return {
        "rate": rate,
        "rho": rho,
        "n_ss": n_ss,
        "start": start,
        "settle": settle,
        "frames_to_settle": settle - start,
        "final_n": history[-1],
        "history": history,
    }


def cohort_run(rate: float, gamma: float, frames: int = RUN_FRAMES) -> dict:
    """Integer fixed-life cohort model matching emitter/src/emitter.c."""
    deaths = [0] * (frames + 62)
    alive = 0
    acc = 0.0
    drops = 0
    history: list[int] = []
    scales: list[float] = []

    for f in range(frames):
        alive -= deaths[f]
        scale = retention(alive / CAPACITY, gamma)
        scales.append(scale)
        acc += rate * scale * DT
        want = int(acc)
        acc -= want
        spawned = min(want, CAPACITY - alive)
        drops += want - spawned
        alive += spawned
        deaths[f + 61] += spawned
        history.append(alive)

    start = next(i for i, s in enumerate(scales) if s < 1.0 - 1e-9)
    tail_mean = sum(history[-240:]) / 240.0
    settle = next(
        i
        for i in range(start, frames)
        if all(abs(n - tail_mean) <= 3.0 for n in history[i:])
    )
    return {
        "start": start,
        "settle": settle,
        "frames_to_settle": settle - start,
        "peak": max(history),
        "drops": drops,
        "tail_mean": tail_mean,
        "strict_frames_to_settle": settle - start,
        "history": history,
    }


def write_csvs(outdir: Path, sweep: list[dict], runs: list[dict], cohorts: list[dict]) -> None:
    outdir.mkdir(parents=True, exist_ok=True)

    with (outdir / "degrade_steepness.csv").open("w", newline="", encoding="utf-8") as fh:
        fields = ["gamma", "n_ss_max", "hard_overflow", "budget56_overflow", "sparsity_to_n56"]
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        w.writerows(sweep)

    with (outdir / "degrade_runs.csv").open("w", newline="", encoding="utf-8") as fh:
        fields = [
            "rate", "rho", "analytic_n_ss", "fluid_start_frame",
            "fluid_settle_frame", "fluid_frames_to_settle", "fluid_final_n",
            "cohort_start_frame", "cohort_peak", "cohort_drops",
            "cohort_tail240_mean", "cohort_strict_frames_to_settle",
        ]
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        for r, c in zip(runs, cohorts):
            w.writerow({
                "rate": r["rate"],
                "rho": r["rho"],
                "analytic_n_ss": f"{r['n_ss']:.6f}",
                "fluid_start_frame": r["start"],
                "fluid_settle_frame": r["settle"],
                "fluid_frames_to_settle": r["frames_to_settle"],
                "fluid_final_n": f"{r['final_n']:.6f}",
                "cohort_start_frame": c["start"],
                "cohort_peak": c["peak"],
                "cohort_drops": c["drops"],
                "cohort_tail240_mean": f"{c['tail_mean']:.3f}",
                "cohort_strict_frames_to_settle": c["strict_frames_to_settle"],
            })

    with (outdir / "degrade_trajectories.csv").open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["frame", "n_rate64", "n_rate128", "n_rate256"])
        for f in range(RUN_FRAMES):
            w.writerow([f, *[f"{r['history'][f]:.6f}" for r in runs]])


def make_plot(outdir: Path, sweep: list[dict], runs: list[dict]) -> Path | None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return None

    plt.rcParams.update({
        "font.family": "DejaVu Sans, sans-serif",
        "axes.edgecolor": "#c3c2b7",
        "axes.labelcolor": "#52514e",
        "text.color": "#0b0b0b",
        "xtick.color": "#52514e",
        "ytick.color": "#52514e",
        "grid.color": "#e1e0d9",
        "axes.facecolor": "#fcfcfb",
        "figure.facecolor": "#fcfcfb",
    })
    colors = ["#2a78d6", "#eb6834", "#1baf7a"]

    fig, axes = plt.subplots(1, 3, figsize=(13.2, 4.0), constrained_layout=True)
    gs = axes[0].get_subplotspec().get_gridspec()
    for ax in axes:
        ax.remove()
    ax_over = fig.add_subplot(gs[0, 0])
    ax_sparse = fig.add_subplot(gs[0, 1], sharex=ax_over)
    ax_traj = fig.add_subplot(gs[0, 2])

    gammas = [row["gamma"] for row in sweep]
    nmax = [row["n_ss_max"] for row in sweep]
    sparse = [row["sparsity_to_n56"] for row in sweep]

    for ax in (ax_over, ax_sparse):
        ax.set_facecolor("#fcfcfb")
        ax.grid(True, linewidth=0.8)
        ax.spines[["top", "right"]].set_visible(False)

    ax_over.plot(gammas, nmax, color=colors[0], linewidth=2.2)
    ax_over.axhline(CAPACITY, color="#d03b3b", linewidth=1.6, linestyle="--")
    ax_over.axhline(BUDGET, color="#eda100", linewidth=1.6, linestyle=":")
    ax_over.axvline(GAMMA_SELECTED, color="#898781", linewidth=1.2)
    ax_over.set_title("Steady occupancy vs steepness")
    ax_over.set_xlabel("γ (larger = steeper early cut)")
    ax_over.set_ylabel("worst-rate steady particles")
    ax_over.set_ylim(45, 66)
    ax_over.text(4.02, CAPACITY + 0.2, "hard cap 64", color="#d03b3b", fontsize=9, va="bottom")
    ax_over.text(4.02, BUDGET + 0.2, "design budget 56", color="#a67100", fontsize=9, va="bottom")

    ax_sparse.plot(gammas, sparse, color=colors[1], linewidth=2.2)
    ax_sparse.axvline(GAMMA_SELECTED, color="#898781", linewidth=1.2)
    ax_sparse.scatter([GAMMA_SELECTED], [sparsity(GAMMA_SELECTED)], color=colors[1], s=42, zorder=3)
    ax_sparse.set_title("Sparsity perceptibility")
    ax_sparse.set_xlabel("γ")
    ax_sparse.set_ylabel("mean cut, N=48→56")
    ax_sparse.set_ylim(0, 0.8)

    for r, color in zip(runs, colors):
        frames = range(241)
        ax_traj.plot(frames, [r["history"][f] for f in frames], color=color, linewidth=2.0,
                     label=f"{r['rate']:.0f}/s")
        ax_traj.scatter([r["settle"]], [r["n_ss"]], color=color, s=36, edgecolor="#fcfcfb",
                        linewidth=1.2, zorder=4)
    ax_traj.axhline(CAPACITY, color="#d03b3b", linewidth=1.4, linestyle="--")
    ax_traj.axhline(ONSET * CAPACITY, color="#898781", linewidth=1.2, linestyle=":")
    ax_traj.set_title("Frame runs (fluid recurrence)")
    ax_traj.set_xlabel("frame")
    ax_traj.set_ylabel("particles")
    ax_traj.set_xlim(0, 240)
    ax_traj.set_ylim(0, 68)
    ax_traj.grid(True, linewidth=0.8)
    ax_traj.spines[["top", "right"]].set_visible(False)
    ax_traj.legend(frameon=False, fontsize=9)

    fig.suptitle("Adaptive emission: steady-state inversion and three-rate verification",
                 fontsize=13, fontweight="bold")
    path = outdir / "degrade_curves.png"
    fig.savefig(path, dpi=180)
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--outdir", type=Path, default=root / "artifacts")
    args = parser.parse_args()

    z_budget = BUDGET / CAPACITY
    gamma_req = required_gamma(max(RATES) * LIFE_S / CAPACITY, z_budget)

    sweep = []
    for gamma in GAMMA_GRID:
        n_ss_max = max(steady_z(r * LIFE_S / CAPACITY, gamma) * CAPACITY for r in RATES)
        sweep.append({
            "gamma": gamma,
            "n_ss_max": round(n_ss_max, 6),
            "hard_overflow": round(max(0.0, n_ss_max - CAPACITY), 6),
            "budget56_overflow": round(max(0.0, n_ss_max - BUDGET), 6),
            "sparsity_to_n56": round(sparsity(gamma), 6),
        })

    runs = [fluid_run(r, GAMMA_SELECTED) for r in RATES]
    cohorts = [cohort_run(r, GAMMA_SELECTED) for r in RATES]
    write_csvs(args.outdir, sweep, runs, cohorts)
    plot_path = make_plot(args.outdir, sweep, runs)

    print(f"gamma_required_for_N<=56={gamma_req:.6f}")
    print(f"gamma_selected={GAMMA_SELECTED}")
    print("rate rho n_ss start settle frames final")
    for r in runs:
        print(
            f"{r['rate']:.0f} {r['rho']:.1f} {r['n_ss']:.3f} "
            f"{r['start']} {r['settle']} {r['frames_to_settle']} "
            f"{r['final_n']:.3f}"
        )
    print("cohort rate start peak drops tail_mean strict_frames")
    for rate, c in zip(RATES, cohorts):
        print(
            f"{rate:.0f} {c['start']} {c['peak']} {c['drops']} "
            f"{c['tail_mean']:.3f} {c['strict_frames_to_settle']}"
        )
    print(f"csv_dir={args.outdir}")
    if plot_path:
        print(f"plot={plot_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
