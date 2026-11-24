#!/usr/bin/env python3
"""Plot splash-module diagnostics from the CSV logs written by splash.c.

Outputs (all under --out):
  01_ground_impact_diagnosis.png  before/after frame traces vs ground plane
  02_splash_density.png           radial density around a single impact
  03_pool_budget.png              pool occupancy, caps and drop counters
  frame_capture.md                per-frame table for the first traced drop
  density_bins.csv                annulus statistics behind figure 2
Run with -I so the working directory cannot shadow stdlib modules.
"""
import argparse
import csv
import math
import os
import site
import sys

# -I keeps cwd off sys.path (planted-module protection); user site is only
# added explicitly here, by absolute path rather than inherited environment.
_user_site = site.getusersitepackages()
if os.path.isdir(_user_site) and _user_site not in sys.path:
    site.addsitedir(_user_site)

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D

# ---- validated dataviz palette (light mode) ----
SURFACE = "#fcfcfb"
PAGE = "#f9f9f7"
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASE = "#c3c2b7"
BLUE, ORANGE, AQUA, RED = "#2a78d6", "#eb6834", "#1baf7a", "#e34948"
BLUE_RAMP = ["#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b"]

plt.rcParams.update({
    "font.sans-serif": ["Noto Sans CJK SC", "WenQuanYi Zen Hei", "DejaVu Sans"],
    "font.size": 10,
    "axes.edgecolor": BASE,
    "axes.labelcolor": INK2,
    "axes.titlecolor": INK,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "figure.facecolor": PAGE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": PAGE,
    "axes.unicode_minus": False,
})


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def style_ax(ax):
    ax.grid(True, axis="both", alpha=0.7)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.tick_params(length=3)


def load_tracks(run_dir):
    """drop_id -> list of rows, ordered by frame."""
    drops = {}
    for row in read_csv(os.path.join(run_dir, "rain_trace.csv")):
        drops.setdefault(int(row["drop_id"]), []).append(row)
    for rows in drops.values():
        rows.sort(key=lambda r: int(r["frame"]))
    return drops


# ---------- figure 1: ground-impact diagnosis ----------
def fig_diagnosis(base_dir, fixed_dir, out):
    base = load_tracks(base_dir)
    fixed = load_tracks(fixed_dir)
    ids = sorted(set(base) & set(fixed))[:6]

    fig, axes = plt.subplots(2, 2, figsize=(12.5, 8.5), sharex="col")
    configs = [
        (axes[0, 0], base, ids, "修复前 · 地面附近（无落地判定）", (-2, 18)),
        (axes[0, 1], fixed, ids, "修复后 · 地面附近（线段穿越判定）", (-2, 18)),
        (axes[1, 0], base, ids, "修复前 · 全量程", (-150, 20)),
        (axes[1, 1], fixed, ids, "修复后 · 全量程", (-2, 18)),
    ]
    for ax, drops, d_ids, title, ylim in configs:
        style_ax(ax)
        ax.set_title(title, fontsize=12, pad=8)
        ax.axhline(0, color=INK, linewidth=1.6)
        ax.text(0.99, 0.06, "地面 y = 0", transform=ax.transAxes,
                ha="right", va="bottom", color=INK2, fontsize=9)
        for k, d_id in enumerate(d_ids):
            rows = drops[d_id]
            fr = np.array([int(r["frame"]) for r in rows])
            h = np.array([float(r["height_above_ground"]) for r in rows])
            c = BLUE_RAMP[min(k * 2, len(BLUE_RAMP) - 1)]
            ax.plot(fr, h, color=c, linewidth=1.8,
                    label=f"雨滴 {d_id}" if k < 3 else None)
            last = rows[-1]
            if last["event"] == "expired":
                ax.plot(int(last["frame"]), float(last["height_above_ground"]),
                        marker="x", color=RED, markersize=9, markeredgewidth=2)
            elif last["event"] == "ground_hit":
                ax.plot(int(last["frame"]), 0, marker="D", color=AQUA,
                        markersize=7, markeredgecolor=SURFACE, markeredgewidth=1.2)
        ax.set_ylim(*ylim)
        ax.set_ylabel("离地高度 h = y − y_ground (m)")

    # annotate the two failure / fix signatures once each
    b0 = base[ids[0]][-1]
    axes[1, 0].annotate(
        f"寿命到期才消失\n帧 {b0['frame']}，h = {float(b0['height_above_ground']):.1f} m\n"
        f"（穿过地面 {abs(float(b0['height_above_ground'])):.1f} m，全程无碰撞事件）",
        xy=(int(b0["frame"]), float(b0["height_above_ground"])),
        xytext=(12, -95), textcoords="data", color=RED, fontsize=9,
        arrowprops=dict(arrowstyle="->", color=RED, lw=1.4))
    f0 = fixed[ids[0]][-1]
    axes[0, 1].annotate(
        "落地瞬间：撞击点钳制在 y = 0\n雨滴销毁并二次发射 6 个溅射粒子",
        xy=(int(f0["frame"]), 0), xytext=(2.5, 9), textcoords="data",
        color=AQUA, fontsize=9,
        arrowprops=dict(arrowstyle="->", color=AQUA, lw=1.4))

    legend_elems = [
        Line2D([0], [0], marker="x", color=RED, lw=0, markersize=9,
               markeredgewidth=2, label="寿命过期消失（无判定）"),
        Line2D([0], [0], marker="D", color=AQUA, lw=0, markersize=7,
               label="落地撞击事件（修复后）"),
        Line2D([0], [0], color=BLUE, lw=2, label="雨滴轨迹"),
    ]
    axes[0, 0].legend(handles=legend_elems, loc="upper right", fontsize=8.5,
                      framealpha=0.95, edgecolor=BASE)
    for ax in axes[1]:
        ax.set_xlabel("帧号（dt = 0.1 s/帧）")
    fig.suptitle("落地判定诊断：雨滴消失位置与地面的高度差",
                 fontsize=14, color=INK, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(os.path.join(out, "01_ground_impact_diagnosis.png"), dpi=150)
    plt.close(fig)


# ---------- figure 2: radial splash density ----------
def fig_density(single_dir, out, bin_w=0.25):
    rows = read_csv(os.path.join(single_dir, "density.csv"))
    impacts = read_csv(os.path.join(single_dir, "splash_trace.csv"))
    impact_xy = [(float(r["x"]), float(r["y"])) for r in impacts]

    # source_impact is written by the simulator, so each sample is attributed
    # to its own burst and converted to impact-relative coordinates
    frames, xs, ys, src = [], [], [], []
    for r in rows:
        if int(r["kind"]) != 1:
            continue
        j = int(r["source_impact"])
        ix, iy = impact_xy[j]
        frames.append(int(r["frame"]))
        src.append(j)
        xs.append(float(r["x"]) - ix)
        ys.append(float(r["y"]) - iy)
    frames = np.array(frames)
    src = np.array(src)
    xs, ys = np.array(xs), np.array(ys)
    radii = np.hypot(xs, ys)
    n_impacts = len(impact_xy)
    # every burst runs the same number of frames; exposure = impacts x frames
    n_frames = len(np.unique(frames[src == 0]))

    edges = np.arange(0.0, radii.max() + bin_w, bin_w)
    counts, _ = np.histogram(radii, bins=edges)
    centers = 0.5 * (edges[:-1] + edges[1:])
    areas = math.pi * (edges[1:] ** 2 - edges[:-1] ** 2)
    # pool the N independent impacts: divide exposure by (impacts x frames)
    density = counts / areas / (n_frames * n_impacts)
    with open(os.path.join(out, "density_bins.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["r_inner", "r_outer", "pooled_count", "area_m2",
                    "density_per_m2_per_impact_per_frame"])
        for j in range(len(counts)):
            w.writerow([f"{edges[j]:.3f}", f"{edges[j+1]:.3f}", counts[j],
                        f"{areas[j]:.4f}", f"{density[j]:.4f}"])

    # exponential fit on non-empty bins, log-linear regression
    m = counts > 0
    coef = np.polyfit(centers[m], np.log(density[m]), 1)
    lam, log_a = -coef[0], coef[1]
    fit = np.exp(log_a) * np.exp(-lam * centers)
    ss_res = np.sum((np.log(density[m]) - (log_a - lam * centers[m])) ** 2)
    ss_tot = np.sum((np.log(density[m]) - np.mean(np.log(density[m]))) ** 2)
    r2 = 1 - ss_res / ss_tot
    # Spearman rank correlation: robust overall trend signal (no shape assumed)
    rank_r = np.argsort(np.argsort(centers[m]))
    rank_d = np.argsort(np.argsort(density[m]))
    spearman = np.corrcoef(rank_r, rank_d)[0, 1]
    mono = np.mean(np.diff(density[m]) < 0)
    peak, far = density[m][0], density[m][-1]
    shoulder = density[m][1:9].mean()      # mid-range ballistic-dwell plateau

    # map panel shows only the first impact, so the arcs stay readable
    fmask = src == 0
    x1, y1, fr1 = xs[fmask], ys[fmask], frames[fmask]

    fig = plt.figure(figsize=(12.5, 7.6))
    gs = fig.add_gridspec(2, 2, height_ratios=[1, 1], width_ratios=[1, 1.15])
    ax_map = fig.add_subplot(gs[:, 0])
    ax_cnt = fig.add_subplot(gs[0, 1])
    ax_den = fig.add_subplot(gs[1, 1], sharex=ax_cnt)

    style_ax(ax_map)
    sc = ax_map.scatter(x1, y1, c=fr1, cmap="Blues", s=26,
                        edgecolor=SURFACE, linewidth=0.5, zorder=3)
    ax_map.plot(0, 0, marker="D", color=AQUA, markersize=9,
                markeredgecolor=SURFACE, markeredgewidth=1.2, zorder=4)
    ax_map.axhline(0, color=INK, linewidth=1.4)
    ax_map.text(0.99, 0.04, "地面", transform=ax_map.transAxes,
                ha="right", color=INK2, fontsize=9)
    ax_map.set_title("单次落地的溅射弹道（颜色=帧，浅早深晚）", fontsize=12)
    ax_map.set_xlabel("相对撞击点 x (m)")
    ax_map.set_ylabel("相对撞击点 y (m)")
    ax_map.set_aspect("equal", adjustable="datalim")
    cb = fig.colorbar(sc, ax=ax_map, fraction=0.046, pad=0.04)
    cb.set_label("帧号", color=INK2)
    cb.ax.tick_params(colors=MUTED, length=3)

    style_ax(ax_cnt)
    ax_cnt.bar(centers, counts, width=bin_w * 0.88, color=ORANGE,
               edgecolor=SURFACE, linewidth=1.5, zorder=3)
    ax_cnt.set_title(f"环形区间采样计数（{n_impacts} 次独立落地合计，"
                     f"{len(frames)} 条采样）", fontsize=12)
    ax_cnt.set_ylabel("粒子·帧 样本数")
    ax_cnt.tick_params(labelbottom=False)

    style_ax(ax_den)
    ax_den.semilogy(centers[m], density[m], "o", color=BLUE, markersize=7,
                    markeredgecolor=SURFACE, markeredgewidth=1.0, zorder=3,
                    label="实测密度 ρ(r)")
    ax_den.semilogy(centers, fit, "--", color=RED, linewidth=1.8, zorder=2,
                    label=f"指数参考线 λ = {lam:.2f} /m（R² = {r2:.2f}，非纯指数律）")
    ax_den.axhspan(shoulder * 0.7, shoulder * 1.3, color=ORANGE,
                   alpha=0.12, zorder=1)
    ax_den.set_title("面积归一化密度：ρ(r) = 计数 / (环面积 × 落地数 × 帧数)",
                     fontsize=12)
    ax_den.set_xlabel("距落地点径向距离 r (m)")
    ax_den.set_ylabel("密度 (个/m²/落地/帧，对数轴)")
    ax_den.legend(loc="upper right", fontsize=8.5, framealpha=0.95,
                  edgecolor=BASE)
    ax_den.text(0.02, 0.05,
                f"Spearman 秩相关 = {spearman:.2f}（负，整体随距离衰减）\n"
                f"相邻环递减占比 {mono*100:.0f}%；近场峰 ρ = {peak:.2f}\n"
                f"中段弹道滞留平台 ρ ≈ {shoulder:.2f}（橙色带）\n"
                f"最远环 ρ = {far:.1e}，近场/远端 ≈ {peak / far:.0f} 倍",
                transform=ax_den.transAxes, fontsize=8.5, color=INK2,
                va="bottom",
                bbox=dict(boxstyle="round,pad=0.4", fc=SURFACE, ec=BASE))

    fig.suptitle(f"落地点周围溅射粒子密度分布（{n_impacts} 次独立落地，"
                 f"每次 6 个粒子）",
                 fontsize=14, color=INK, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(os.path.join(out, "02_splash_density.png"), dpi=150)
    plt.close(fig)
    return lam, r2, peak, far, mono, spearman


# ---------- figure 3: pool occupancy and drops ----------
def fig_budget(normal_dir, stress_dir, out):
    fig, axes = plt.subplots(2, 2, figsize=(12.5, 8.5), sharex="col")
    for row, (run_dir, name) in enumerate(
            ((normal_dir, "常规场景：30 滴/秒"),
             (stress_dir, "压力场景：120 滴/秒 + 链式溅射"))):
        rows = read_csv(os.path.join(run_dir, "series.csv"))
        fr = np.array([int(r["frame"]) for r in rows])
        rain = np.array([int(r["rain_alive"]) for r in rows])
        splash = np.array([int(r["splash_alive"]) for r in rows])
        emitted = np.array([int(r["emitted"]) for r in rows])
        s_emitted = np.array([int(r["splash_emitted"]) for r in rows])
        dropped = np.array([int(r["dropped"]) for r in rows])
        s_dropped = np.array([int(r["splash_dropped"]) for r in rows])

        ax = axes[row, 0]
        style_ax(ax)
        ax.stackplot(fr, rain, splash, colors=[BLUE, ORANGE],
                     edgecolor=SURFACE, linewidth=1.2, labels=["雨滴", "溅射粒子"])
        ax.axhline(256, color=RED, ls="--", linewidth=1.8)
        ax.axhline(48, color=AQUA, ls="--", linewidth=1.8)
        ax.text(0.01, 252, "总池上限 POOL_MAX = 256", va="top",
                ha="left", color=RED, fontsize=9,
                bbox=dict(boxstyle="round,pad=0.25", fc=SURFACE, ec="none",
                          alpha=0.85))
        ax.text(0.01, 44, "溅射上限 SPLASH_BUDGET = 48", va="top",
                ha="left", color=AQUA, fontsize=9,
                bbox=dict(boxstyle="round,pad=0.25", fc=SURFACE, ec="none",
                          alpha=0.85))
        ax.set_ylim(0, 285)
        ax.set_title(f"{name} · 在场粒子数（峰值 {rain.max() + splash.max()}）",
                     fontsize=12)
        ax.set_ylabel("存活粒子数")
        ax.legend(loc="center right", fontsize=9, framealpha=0.95,
                  edgecolor=BASE)

        ax2 = axes[row, 1]
        style_ax(ax2)
        ax2.plot(fr, emitted, color=BLUE, lw=2, label="累计发射（全部）")
        ax2.plot(fr, s_emitted, color=ORANGE, lw=2, label="累计发射（溅射）")
        ax2.plot(fr, dropped, color=RED, lw=2, label="累计丢弃（全部）")
        ax2.plot(fr, s_dropped, color=RED, lw=2, ls=":",
                 label="累计丢弃（溅射，与全部重合）")
        end = -1
        for arr, c, dy in ((emitted, BLUE, 8), (s_emitted, ORANGE, 8),
                           (dropped, RED, -14)):
            ax2.annotate(str(arr[end]), xy=(fr[end], arr[end]),
                         xytext=(6, dy), textcoords="offset points",
                         color=c, fontsize=9, va="center")
        ax2.set_title(f"{name} · 发射与丢弃计数", fontsize=12)
        ax2.set_ylabel("累计计数")
        ax2.legend(loc="upper left", fontsize=8.5, framealpha=0.95,
                   edgecolor=BASE)
        ax2.text(0.99, 0.06,
                 f"最终：发射 {emitted[-1]}，丢弃 {dropped[-1]}\n"
                 f"（丢弃 = 被预算拒绝的发射尝试，故可超过实际发射数；"
                 f"全部为溅射，雨滴 0 丢弃）",
                 transform=ax2.transAxes, ha="right", va="bottom",
                 fontsize=8.2, color=INK2,
                 bbox=dict(boxstyle="round,pad=0.35", fc=SURFACE, ec=BASE))

    for ax in axes[1]:
        ax.set_xlabel("帧号（dt = 0.033 s/帧）")
    fig.suptitle("总粒子数控制：共享池占用、溅射上限与丢弃计数",
                 fontsize=14, color=INK, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(os.path.join(out, "03_pool_budget.png"), dpi=150)
    plt.close(fig)


# ---------- frame-capture table ----------
def first_drop_rows(run_dir):
    """rows of the first tracked drop only, up to and including its end event"""
    drops = load_tracks(run_dir)
    rows = drops[0]
    out = []
    for r in rows:
        out.append(r)
        if r["event"] in ("expired", "ground_hit"):
            break
    return out


def write_frames(base_dir, fixed_dir, out):
    head = ["# 落地瞬间逐帧记录（首颗被追踪雨滴，drop_id = 0）\n",
            "dt = 0.1 s/帧，地面 y = 0。\n"]

    def block(title, rows):
        lines = [f"## {title}\n",
                 "| 帧 | x | y | 离地高度 h (m) | 事件 |",
                 "|---|---|---|---|---|"]
        for r in rows:
            lines.append(f"| {r['frame']} | {float(r['x']):.3f} | "
                         f"{float(r['y']):.3f} | {float(r['height_above_ground']):.3f} "
                         f"| {r['event']} |")
        return lines

    lines = head + block("修复前：无落地判定（穿过地面直到寿命过期）",
                         first_drop_rows(base_dir)) + [""]
    lines += block("修复后：线段穿越判定 + 溅射二次发射",
                   first_drop_rows(fixed_dir))
    with open(os.path.join(out, "frame_capture.md"), "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", required=True, help="dir holding run subfolders")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    r = args.runs
    fig_diagnosis(os.path.join(r, "baseline"), os.path.join(r, "fixed"), args.out)
    lam, r2, peak, far, mono, spearman = fig_density(
        os.path.join(r, "single"), args.out)
    fig_budget(os.path.join(r, "normal"), os.path.join(r, "stress"), args.out)
    write_frames(os.path.join(r, "baseline"), os.path.join(r, "fixed"), args.out)
    print(f"lambda={lam:.4f} r2={r2:.4f} spearman={spearman:.4f} "
          f"peak={peak:.4f} far={far:.2e} decay_x={peak / far:.1f} "
          f"monotone_frac={mono:.3f}")


if __name__ == "__main__":
    main()
