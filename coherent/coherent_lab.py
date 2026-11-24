#!/usr/bin/env python3
"""Coherent position-noise particle diffusion laboratory.

The experiment keeps the projectile parabola from the C modules as the
force-free baseline, then adds a deterministic low-frequency vector field.
Unlike per-particle random draws, the field is sampled from position, so
neighbouring particles receive nearly the same perturbation.

Outputs a machine-readable verification JSON, trajectory CSV and PNG charts
under ../artifacts by default.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.family"] = [
    "Noto Sans CJK SC",
    "WenQuanYi Zen Hei",
    "DejaVu Sans",
]
plt.rcParams["axes.unicode_minus"] = False
import numpy as np

# Validated light-mode categorical slots 1-3.
BLUE = "#2a78d6"
ORANGE = "#eb6834"
GREEN = "#1baf7a"
INK = "#0b0b0b"
SECONDARY = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
SURFACE = "#fcfcfb"
CRITICAL = "#d03b3b"

TIER_COLORS = [BLUE, ORANGE, GREEN]


def build_world(seed: int = 43, n: int = 512):
    """Create the deterministic burst and narrow-band noise modes."""
    rng = np.random.default_rng(seed)

    speed = 9.0
    launch = math.radians(55.0)
    spawn_radius = 0.18

    # A common launch direction makes the noise-induced angular spread direct.
    angles = np.full(n, launch)
    rr = spawn_radius * np.sqrt(rng.random(n))
    theta = rng.uniform(0.0, 2.0 * np.pi, n)
    x0 = rr * np.cos(theta)
    y0 = rr * np.sin(theta)
    vx0 = speed * np.cos(angles)
    vy0 = speed * np.sin(angles)

    # Narrow-band Fourier noise. q is a multiplier on the base wave number;
    # keeping it in [0.82, 1.25] makes ell a genuine characteristic scale.
    k_modes = 6
    q = np.array([0.82, 0.90, 1.00, 1.08, 1.16, 1.25])
    alpha = rng.uniform(0.0, 2.0 * np.pi, k_modes)
    kx = q * np.cos(alpha)
    ky = q * np.sin(alpha)
    phase_x = rng.uniform(0.0, 2.0 * np.pi, k_modes)
    phase_y = rng.uniform(0.0, 2.0 * np.pi, k_modes)
    weights = np.ones(k_modes) / math.sqrt(k_modes)

    gravity = -9.8
    flight_time = 2.0 * speed * np.sin(launch) / 9.8

    return {
        "n": n,
        "seed": seed,
        "speed": speed,
        "launch_deg": 55.0,
        "spawn_radius": spawn_radius,
        "x0": x0,
        "y0": y0,
        "vx0": vx0,
        "vy0": vy0,
        "gravity": gravity,
        "flight_time": flight_time,
        "kx": kx,
        "ky": ky,
        "phase_x": phase_x,
        "phase_y": phase_y,
        "weights": weights,
        "q_max": float(q.max()),
    }


def noise_field(world, x, y, ell):
    """Two isotropic-ish component channels with component RMS about one."""
    k0 = 2.0 * math.pi / ell
    phase = k0 * (world["kx"][None, :] * x[:, None] + world["ky"][None, :] * y[:, None])
    nx = math.sqrt(2.0) * np.sum(
        world["weights"][None, :] * np.sin(phase + world["phase_x"][None, :]), axis=1
    )
    ny = math.sqrt(2.0) * np.sum(
        world["weights"][None, :] * np.sin(phase + world["phase_y"][None, :]), axis=1
    )
    return nx, ny


def simulate(world, amplitude, ell=1.0, dt=1.0 / 1200.0, record_stride=None):
    """Semi-implicit Euler with the same gravity convention as field.c."""
    x = world["x0"].copy()
    y = world["y0"].copy()
    vx = world["vx0"].copy()
    vy = world["vy0"].copy()

    steps = int(round(world["flight_time"] / dt))
    frames = []
    if record_stride is not None:
        frames.append(_frame(0, 0.0, x, y, vx, vy))

    for step in range(1, steps + 1):
        nx, ny = noise_field(world, x, y, ell)
        vx += amplitude * nx * dt
        vy += (amplitude * ny + world["gravity"]) * dt
        x += vx * dt
        y += vy * dt

        if record_stride is not None and (
            step % record_stride == 0 or step == steps
        ):
            frames.append(_frame(len(frames), step * dt, x, y, vx, vy))

    return {
        "x": x,
        "y": y,
        "vx": vx,
        "vy": vy,
        "frames": frames,
        "steps": steps,
        "dt": dt,
    }


def _frame(index, t, x, y, vx, vy):
    return {
        "frame": index,
        "t": t,
        "x": x.copy(),
        "y": y.copy(),
        "vx": vx.copy(),
        "vy": vy.copy(),
    }


def group_radius(x, y):
    return float(np.sqrt(np.mean((x - x.mean()) ** 2 + (y - y.mean()) ** 2)))


def direction_stats(vx, vy):
    angles = np.arctan2(vy, vx)
    concentration = float(np.abs(np.mean(np.exp(1j * angles))))
    concentration = min(1.0, max(1e-12, concentration))
    circular_sigma = math.degrees(math.sqrt(-2.0 * math.log(concentration)))
    linear_sigma = float(np.std(np.rad2deg(angles)))
    mean_angle = float(np.degrees(np.angle(np.mean(np.exp(1j * angles)))))
    return {
        "sigma_circular_deg": circular_sigma,
        "sigma_linear_deg": linear_sigma,
        "concentration": concentration,
        "mean_angle_deg": mean_angle,
    }


def shape_diffusion(baseline, run):
    """RMS added centered displacement; excludes common translation."""
    dx = (run["x"] - run["x"].mean()) - (
        baseline["x"] - baseline["x"].mean()
    )
    dy = (run["y"] - run["y"].mean()) - (
        baseline["y"] - baseline["y"].mean()
    )
    return float(np.sqrt(np.mean(dx * dx + dy * dy)))


def neighbor_scale(world, ell):
    x = world["x0"]
    y = world["y0"]
    positions = np.column_stack([x, y])
    delta = positions[:, None, :] - positions[None, :, :]
    distances = np.sqrt(np.sum(delta * delta, axis=2))
    np.fill_diagonal(distances, np.inf)
    nearest = np.argmin(distances, axis=1)
    h = distances[np.arange(len(x)), nearest]

    nx, ny = noise_field(world, x, y, ell)
    dot = nx * nx[nearest] + ny * ny[nearest]
    norm = np.sqrt((nx * nx + ny * ny) * (nx[nearest] ** 2 + ny[nearest] ** 2))
    cosine = np.clip(dot / norm, -1.0, 1.0)
    angle = np.degrees(np.arccos(cosine))

    phi_budget = math.radians(5.0)
    ell_min_conservative = (
        2.0 * math.pi * world["q_max"] * float(np.median(h)) / phi_budget
    )
    return {
        "h_median": float(np.median(h)),
        "h_mean": float(np.mean(h)),
        "h_p90": float(np.percentile(h, 90)),
        "ell_over_h_median": float(ell / np.median(h)),
        "neighbor_noise_angle_median_deg": float(np.median(angle)),
        "neighbor_noise_angle_p90_deg": float(np.percentile(angle, 90)),
        "neighbor_noise_angle_max_deg": float(np.max(angle)),
        "neighbor_cosine_median": float(np.median(cosine)),
        "conservative_ell_min_for_5deg": ell_min_conservative,
    }


def scaling_demo():
    """Numerically check RSS sqrt(m) versus coherent linear/block scaling."""
    ms = np.array([1, 2, 4, 8, 16, 32, 64], dtype=float)
    trials = 4000
    rng = np.random.default_rng(20261125)

    independent = []
    coherent_raw = []
    coherent_block = []
    for m in ms.astype(int):
        z = rng.standard_normal((trials, m, 2))
        sums = z.sum(axis=1)
        # Per-component variance one, hence the sqrt(2) radial constant.
        independent.append(float(np.mean(np.linalg.norm(sums, axis=1))))

        z0 = rng.standard_normal((trials, 2))
        raw_sums = m * z0
        coherent_raw.append(float(np.mean(np.linalg.norm(raw_sums, axis=1))))
        block_sums = z0  # block amplitude A_block = m * raw amplitude
        coherent_block.append(float(np.mean(np.linalg.norm(block_sums, axis=1))))

    fit_ind = np.polyfit(np.log(ms), np.log(np.asarray(independent)), 1)[0]
    fit_coh_raw = np.polyfit(np.log(ms), np.log(np.asarray(coherent_raw)), 1)[0]
    fit_coh_block = np.polyfit(np.log(ms), np.log(np.asarray(coherent_block)), 1)[0]

    # m=4 equal-weight inversion, radial-RMS normalized to E||z||^2 = 1.
    target = 1.0
    m4 = 4
    a_ind_m4 = target / math.sqrt(m4)
    a_coh_raw_m4 = target / m4
    return {
        "m": ms.astype(int).tolist(),
        "independent_norm": independent,
        "coherent_raw_sample_norm": coherent_raw,
        "coherent_block_norm": coherent_block,
        "independent_log_slope": float(fit_ind),
        "coherent_raw_log_slope": float(fit_coh_raw),
        "coherent_block_log_slope": float(fit_coh_block),
        "m4_inversion": {
            "target_radius": target,
            "independent_per_sample_amplitude": a_ind_m4,
            "coherent_per_sample_amplitude": a_coh_raw_m4,
            "coherent_block_amplitude": target,
            "coherent_over_independent_per_sample": a_coh_raw_m4 / a_ind_m4,
        },
    }


def write_trajectory_csv(path, world, tiers, sample_count=64):
    ids = np.linspace(0, world["n"] - 1, sample_count, dtype=int)
    lines = ["tier,target_radius,amplitude,t_s,frame,particle_id,x,y,vx,vy"]
    for tier in tiers:
        label = tier["label"]
        target = tier["target_radius"]
        amplitude = tier["amplitude"]
        for frame in tier["run"]["frames"]:
            for pid in ids:
                lines.append(
                    f"{label},{target:.2f},{amplitude:.3f},{frame['t']:.4f},"
                    f"{frame['frame']},{pid},{frame['x'][pid]:.6f},"
                    f"{frame['y'][pid]:.6f},{frame['vx'][pid]:.6f},"
                    f"{frame['vy'][pid]:.6f}"
                )
    path.write_text("\n".join(lines) + "\n")


def plot_curves(path, sweep, tiers, gain):
    fig, (ax_r, ax_h) = plt.subplots(
        2, 1, figsize=(8.2, 6.2), sharex=True, constrained_layout=True
    )
    fig.patch.set_facecolor(SURFACE)
    for ax in (ax_r, ax_h):
        ax.set_facecolor(SURFACE)
        ax.grid(True, color=GRID, linewidth=0.8)
        ax.spines[["top", "right"]].set_visible(False)
        ax.spines[["left", "bottom"]].set_color(MUTED)
        ax.tick_params(colors=SECONDARY, labelsize=9)

    a = np.asarray(sweep["amplitude"])
    total = np.asarray(sweep["total_radius"])
    shape = np.asarray(sweep["shape_radius"])
    retention = np.asarray(sweep["retention"])

    ax_r.plot(a, total, color=BLUE, linewidth=2.2, label="实际群体 RMS 半径")
    ax_r.plot(a, shape, color=GREEN, linewidth=1.8, linestyle="--", label="相干新增扩散半径")
    ax_r.axline(
        (0, 0), slope=gain, color=MUTED, linewidth=1.4, linestyle=":",
        label=f"解析反解线 R={gain:.2f}A",
    )
    ax_r.set_ylabel("半径（世界单位）", color=INK, fontsize=10)
    ax_r.legend(loc="upper left", frameon=False, fontsize=9)

    ax_h.plot(a, retention, color=ORANGE, linewidth=2.2, label="方向保持裕度 1−σθ/5°")
    ax_h.axhline(0.0, color=CRITICAL, linewidth=1.4, linestyle="--")
    ax_h.set_ylim(-0.08, 1.05)
    ax_h.set_ylabel("方向保持裕度", color=INK, fontsize=10)
    ax_h.set_xlabel("相干噪声加速度幅度 A", color=INK, fontsize=10)
    ax_h.legend(loc="upper right", frameon=False, fontsize=9)

    for tier, color in zip(tiers, TIER_COLORS):
        av = tier["amplitude"]
        for ax in (ax_r, ax_h):
            ax.axvline(av, color=color, linewidth=1.0, alpha=0.35)
        ax_r.scatter(
            [av], [tier["group_radius"]], s=58, color=color,
            edgecolor=SURFACE, linewidth=1.4, zorder=5
        )
        ax_h.scatter(
            [av], [tier["retention"]], s=58, color=color,
            edgecolor=SURFACE, linewidth=1.4, zorder=5
        )
        ax_r.annotate(
            f"R*={tier['target_radius']:.2f}\nσ={tier['direction']['sigma_circular_deg']:.2f}°",
            (av, tier["group_radius"]),
            xytext=(7, -4), textcoords="offset points",
            fontsize=8.5, color=INK,
        )

    fig.suptitle("用扩散半径与方向保持度共同确定噪声幅度", fontsize=13, color=INK)
    fig.savefig(path, dpi=170, facecolor=SURFACE)
    plt.close(fig)


def plot_trajectories(path, world, baseline, tiers):
    sample_ids = np.linspace(0, world["n"] - 1, 72, dtype=int)
    base_frames = baseline["frames"]
    bx = np.array([f["x"][sample_ids] for f in base_frames])
    by = np.array([f["y"][sample_ids] for f in base_frames])

    fig, axes = plt.subplots(1, 3, figsize=(13.2, 4.6), sharex=True, sharey=True)
    fig.patch.set_facecolor(SURFACE)
    for ax, tier, color in zip(axes, tiers, TIER_COLORS):
        ax.set_facecolor(SURFACE)
        for j in range(len(sample_ids)):
            ax.plot(bx[:, j], by[:, j], color=MUTED, linewidth=0.7, alpha=0.35)
        frames = tier["run"]["frames"]
        tx = np.array([f["x"][sample_ids] for f in frames])
        ty = np.array([f["y"][sample_ids] for f in frames])
        for j in range(len(sample_ids)):
            ax.plot(tx[:, j], ty[:, j], color=color, linewidth=0.9, alpha=0.72)
        ax.scatter(world["x0"][sample_ids], world["y0"][sample_ids], s=7, color=INK, alpha=0.55)
        ax.set_title(
            f"{tier['label']}：R*={tier['target_radius']:.2f}, "
            f"σθ={tier['direction']['sigma_circular_deg']:.2f}°",
            fontsize=10, color=INK,
        )
        ax.grid(True, color=GRID, linewidth=0.8)
        ax.spines[["top", "right"]].set_visible(False)
        ax.spines[["left", "bottom"]].set_color(MUTED)
        ax.tick_params(colors=SECONDARY, labelsize=9)
        ax.set_aspect("equal", adjustable="box")

    axes[0].set_xlabel("x（世界单位）")
    axes[0].set_ylabel("y（世界单位）")
    for ax in axes[1:]:
        ax.set_xlabel("x（世界单位）")
    fig.suptitle("灰色为无扰动抛物线基线，彩色为低频位置相干扰动", fontsize=13, color=INK)
    fig.tight_layout()
    fig.savefig(path, dpi=170, facecolor=SURFACE)
    plt.close(fig)


def plot_scaling(path, scaling):
    m = np.asarray(scaling["m"], dtype=float)
    fig, ax = plt.subplots(figsize=(7.4, 4.8), constrained_layout=True)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)
    ax.grid(True, color=GRID, linewidth=0.8, which="both")
    ax.spines[["top", "right"]].set_visible(False)
    ax.spines[["left", "bottom"]].set_color(MUTED)
    ax.tick_params(colors=SECONDARY)

    ax.loglog(m, scaling["independent_norm"], "o-", color=BLUE, linewidth=2,
              label="独立样本 RSS：斜率 1/2")
    ax.loglog(m, scaling["coherent_raw_sample_norm"], "s-", color=ORANGE, linewidth=2,
              label="相干逐样本幅度：斜率 1")
    ax.loglog(m, scaling["coherent_block_norm"], "^--", color=GREEN, linewidth=2,
              label="相干块幅度归一化：斜率 0")
    ax.set_xlabel("样本数 m（对数）")
    ax.set_ylabel("群体/RSS 范数（对数）")
    ax.set_title("独立随机的 √m 与相干场的线性/块幅度口径", color=INK, fontsize=12)
    ax.legend(frameon=False, fontsize=9)
    fig.savefig(path, dpi=170, facecolor=SURFACE)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser()
    default_out = Path(__file__).resolve().parents[1] / "artifacts"
    parser.add_argument("--out", type=Path, default=default_out)
    parser.add_argument("--ell", type=float, default=1.0)
    parser.add_argument("--gain", type=float, default=0.25)
    args = parser.parse_args()
    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    world = build_world()
    baseline = simulate(world, 0.0, ell=args.ell, record_stride=12)
    baseline_directions = direction_stats(baseline["vx"], baseline["vy"])
    scale = neighbor_scale(world, args.ell)

    targets = [0.35, 0.60, 0.90]
    # Closed-form inverse in the linear response band: A* = R*/G.
    amplitudes = [r / args.gain for r in targets]
    labels = ["small", "medium", "large"]

    sweep_a = np.linspace(0.0, 5.0, 26)
    sweep = {"amplitude": [], "total_radius": [], "shape_radius": [],
             "sigma_deg": [], "concentration": [], "retention": []}
    for amplitude in sweep_a:
        result = simulate(world, float(amplitude), ell=args.ell)
        directions = direction_stats(result["vx"], result["vy"])
        sweep["amplitude"].append(float(amplitude))
        sweep["total_radius"].append(group_radius(result["x"], result["y"]))
        sweep["shape_radius"].append(shape_diffusion(baseline, result))
        sweep["sigma_deg"].append(directions["sigma_circular_deg"])
        sweep["concentration"].append(directions["concentration"])
        sweep["retention"].append(
            max(0.0, 1.0 - directions["sigma_circular_deg"] / 5.0)
        )

    # Independent slope estimate for reporting; the declared design gain is
    # intentionally rounded so A = 4R* is easy to audit.
    a_arr = np.asarray(sweep["amplitude"])
    r_arr = np.asarray(sweep["total_radius"])
    band = (a_arr >= 1.4) & (a_arr <= 3.6)
    gain_fit, intercept_fit = np.polyfit(a_arr[band], r_arr[band], 1)

    tiers = []
    all_pass = True
    for label, target, amplitude, color in zip(labels, targets, amplitudes, TIER_COLORS):
        run = simulate(world, amplitude, ell=args.ell, record_stride=12)
        directions = direction_stats(run["vx"], run["vy"])
        actual_radius = group_radius(run["x"], run["y"])
        added_radius = shape_diffusion(baseline, run)
        radius_error = actual_radius / target - 1.0
        radius_ok = abs(radius_error) <= 0.10
        angle_ok = directions["sigma_circular_deg"] <= 5.0
        passed = radius_ok and angle_ok
        all_pass &= passed
        tiers.append({
            "label": label,
            "target_radius": target,
            "amplitude": amplitude,
            "color": color,
            "run": run,
            "group_radius": actual_radius,
            "added_diffusion_radius": added_radius,
            "radius_relative_error": radius_error,
            "radius_within_10pct": radius_ok,
            "direction": directions,
            "angle_within_5deg": angle_ok,
            "retention": max(0.0, 1.0 - directions["sigma_circular_deg"] / 5.0),
            "pass": passed,
        })

    scaling = scaling_demo()

    result = {
        "all_pass": all_pass,
        "interpretation": {
            "radius_metric": "RMS radius about the batch centroid at the common landing-time sample",
            "added_radius_metric": "RMS of centered paired displacement from the A=0 parabola",
            "angle_metric": "circular std of final velocity directions; linear std is also recorded because angles do not wrap in this scene",
            "sample_count_warning": "a sqrt(N) law is the unnormalized RSS norm; RMS over N particles divides by sqrt(N)",
        },
        "parameters": {
            "n": world["n"],
            "seed": world["seed"],
            "speed": world["speed"],
            "launch_deg": world["launch_deg"],
            "spawn_radius": world["spawn_radius"],
            "gravity": world["gravity"],
            "flight_time_s": world["flight_time"],
            "dt_s": tiers[0]["run"]["dt"],
            "steps": tiers[0]["run"]["steps"],
            "noise_scale_ell": args.ell,
            "design_gain": args.gain,
            "fitted_gain_1p4_to_3p6": float(gain_fit),
            "fitted_intercept": float(intercept_fit),
        },
        "baseline": {
            "group_radius": group_radius(baseline["x"], baseline["y"]),
            **baseline_directions,
        },
        "scale": scale,
        "scaling_laws": scaling,
        "sweep": sweep,
        "tiers": [
            {k: v for k, v in tier.items() if k not in {"run", "color"}}
            for tier in tiers
        ],
    }

    json_path = out_dir / "coherent_verification.json"
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")

    csv_path = out_dir / "coherent_trajectories.csv"
    write_trajectory_csv(csv_path, world, tiers)

    curves_path = out_dir / "amplitude_tradeoff.png"
    plot_curves(curves_path, sweep, tiers, args.gain)
    trajectories_path = out_dir / "coherent_three_runs.png"
    plot_trajectories(trajectories_path, world, baseline, tiers)
    scaling_path = out_dir / "sample_scaling_laws.png"
    plot_scaling(scaling_path, scaling)

    print(json.dumps({
        "all_pass": all_pass,
        "json": str(json_path),
        "csv": str(csv_path),
        "charts": [str(curves_path), str(trajectories_path), str(scaling_path)],
        "tiers": [
            {
                "label": t["label"],
                "A": round(t["amplitude"], 4),
                "R": round(t["group_radius"], 6),
                "target": t["target_radius"],
                "error_pct": round(100 * t["radius_relative_error"], 3),
                "sigma_deg": round(t["direction"]["sigma_circular_deg"], 4),
                "pass": t["pass"],
            }
            for t in tiers
        ],
    }, ensure_ascii=False, indent=2))

    if not all_pass:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
