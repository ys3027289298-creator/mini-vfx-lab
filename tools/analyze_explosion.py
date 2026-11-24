#!/usr/bin/env python3
"""Explosion particle-lifetime analysis.

Reads the four CSV streams produced by explosion/src/explosion.c for the three
lifetime modes (fixed / uniform / exp) and emits:

  fig1_decay.png            total alive count + mean brightness over time
  fig2_lifetime.png         lifetime histograms + empirical-vs-theoretical CDF
  fig3_batch_grid_<m>.png   one image per mode: per-batch snapshots at four
                            times, each batch labelled with alive count and
                            mean brightness
  fig4_batch0_decay.png     generation-to-extinction decay of the first burst,
                            empirical curves against the theoretical survival
                            functions implied by each lifetime distribution
  batch_snapshots_<m>.csv   per-batch alive/brightness tables at snapshot times
  analysis_summary.json     numeric checks (cliffs, KS stats, life stats)

Run: python tools/analyze_explosion.py --in-dir artifacts/explosion_v2 \
                                       --out-dir artifacts/explosion_v2/analysis
"""
import argparse
import json
import os

import numpy as np
import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# ---------------------------------------------------------------- style ----
plt.rcParams.update({
    "font.family": ["Noto Sans CJK SC", "sans-serif"],
    "axes.unicode_minus": False,
    "figure.facecolor": "#fcfcfb",
    "axes.facecolor": "#fcfcfb",
    "savefig.facecolor": "#fcfcfb",
    "axes.edgecolor": "#c3c2b7",
    "axes.labelcolor": "#0b0b0b",
    "axes.titlecolor": "#0b0b0b",
    "xtick.color": "#52514e",
    "ytick.color": "#52514e",
    "text.color": "#0b0b0b",
    "grid.color": "#e1e0d9",
    "grid.linewidth": 0.8,
    "axes.grid": True,
    "axes.axisbelow": True,
    "font.size": 11,
})

INK, MUTED, SURFACE = "#0b0b0b", "#898781", "#fcfcfb"
SERIES = {
    "fixed":   {"color": "#898781", "ls": (0, (5, 3)), "label": "固定寿命 2.0s（原实现）"},
    "exp":     {"color": "#2a78d6", "ls": "-",          "label": "指数分布（均值 1.5s，截断 12s）"},
    "uniform": {"color": "#eb6834", "ls": "-",          "label": "均匀分布（0.8–3.0s）"},
}
BATCH0_COLOR, LATER_COLOR = "#2a78d6", "#898781"
SNAP_FRAMES = [0, 30, 60, 90]
LIFE_LO, LIFE_HI, EXP_MEAN, LIFE_FIXED = 0.8, 3.0, 1.5, 2.0


def load_mode(in_dir, mode):
    base = os.path.join(in_dir, mode)
    return {
        "frames": pd.read_csv(os.path.join(base, "frames.csv")),
        "parts": pd.read_csv(os.path.join(base, "particles.csv")),
        "snaps": pd.read_csv(os.path.join(base, "snaps.csv")),
        "batches": pd.read_csv(os.path.join(base, "batches.csv")),
    }


def ks_stat(x, cdf):
    """One-sample Kolmogorov-Smirnov distance against a reference CDF."""
    x = np.sort(np.asarray(x, dtype=float))
    n = len(x)
    f = cdf(x)
    return float(max(np.max(np.abs(np.arange(1, n + 1) / n - f)),
                     np.max(np.abs(np.arange(0, n) / n - f))))


def cdf_exp(x):
    return 1.0 - np.exp(-np.asarray(x, dtype=float) / EXP_MEAN)


def cdf_uniform(x):
    x = np.asarray(x, dtype=float)
    return np.clip((x - LIFE_LO) / (LIFE_HI - LIFE_LO), 0.0, 1.0)


def frame_drop_stats(frames):
    """Largest relative single-frame loss, and any frame losing >= half."""
    alive = frames["alive"].to_numpy()
    prev, cur = alive[:-1], alive[1:]
    idxs = np.flatnonzero(prev >= 10)
    if len(idxs) == 0:
        return -1, 0.0, []
    ratios = (prev[idxs] - cur[idxs]) / prev[idxs]
    worst_pos = int(np.argmax(ratios))
    worst_frame = int(idxs[worst_pos] + 1)
    worst_val = float(ratios[worst_pos])
    cliffs = [int(i + 1) for i in idxs if (prev[i] - cur[i]) >= 0.5 * prev[i]]
    return worst_frame, worst_val, cliffs


def summarize(mode, data):
    frames, parts = data["frames"], data["parts"]
    life = parts["life"].to_numpy()
    peak_idx = int(frames["alive"].idxmax())
    last_alive = frames.loc[frames["alive"] > 0]
    wf, wr, cliffs = frame_drop_stats(frames)
    out = {
        "label": SERIES[mode]["label"],
        "pool_capacity": 200,
        "spawn_requested": int(frames["spawned"].sum() + frames["dropped"].max()),
        "emitted": int(frames["spawned"].sum()),
        "dropped_total": int(frames["dropped"].max()),
        "peak_alive": int(frames["alive"].max()),
        "peak_frame": int(frames.loc[peak_idx, "frame"]),
        "peak_t": float(frames.loc[peak_idx, "t"]),
        "last_alive_frame": int(last_alive["frame"].max()),
        "last_alive_t": float(last_alive["t"].max()),
        "life_min": float(life.min()),
        "life_max": float(life.max()),
        "life_mean": float(life.mean()),
        "life_std": float(life.std(ddof=1)),
        "life_cv": float(life.std(ddof=1) / life.mean()),
        "worst_drop_frame": wf,
        "worst_drop_ratio": wr,
        "cliff_frames_ge_50pct": cliffs,
        "smooth": len(cliffs) == 0,
    }
    if mode == "exp":
        out["ks_against_exp_cdf"] = round(ks_stat(life, cdf_exp), 4)
        out["ks_against_uniform_cdf"] = round(ks_stat(life, cdf_uniform), 4)
    elif mode == "uniform":
        out["ks_against_uniform_cdf"] = round(ks_stat(life, cdf_uniform), 4)
        out["ks_against_exp_cdf"] = round(ks_stat(life, cdf_exp), 4)
    return out


# ------------------------------------------------------------- figures ----
def fig1_decay(all_data, out):
    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(11, 7.2), sharex=True,
        gridspec_kw={"hspace": 0.12})
    for mode in ("fixed", "exp", "uniform"):
        f = all_data[mode]["frames"]
        st = SERIES[mode]
        ax1.plot(f["t"], f["alive"], color=st["color"], ls=st["ls"], lw=2,
                 label=st["label"])
        ax2.plot(f["t"], f["mean_brightness"], color=st["color"], ls=st["ls"],
                 lw=2, label=st["label"])
    ax1.set_ylabel("存活粒子数")
    ax2.set_ylabel("存活粒子平均亮度")
    ax2.set_xlabel("时间 t（秒）")
    ax1.set_title("爆炸粒子总量衰减：固定寿命出现整批断崖，分布寿命平滑衰减",
                  loc="left", fontsize=13, pad=10)
    box = dict(boxstyle="round,pad=0.35", fc="#f0efec", ec="none", alpha=0.9)
    ax1.annotate("容量 200：800 颗爆发请求在第 0 帧\n即被丢弃 600 颗，可见数立刻掉到 200",
                 xy=(0, 200), xytext=(0.35, 360),
                 fontsize=9.5, color="#52514e", bbox=box,
                 arrowprops=dict(arrowstyle="->", color=MUTED, lw=1))
    ax1.annotate("第 60 帧（t=2.0s）200 颗同时消亡\n单帧掉 100%，这就是断崖",
                 xy=(2.0, 0), xytext=(2.7, 250),
                 fontsize=9.5, color="#52514e", bbox=box,
                 arrowprops=dict(arrowstyle="->", color=MUTED, lw=1))
    ax1.set_xlim(-0.2, 8)
    ax1.set_ylim(-15, 470)
    ax1.legend(frameon=False, loc="upper right", fontsize=10)
    ax2.legend(frameon=False, loc="upper right", fontsize=10)
    for ax in (ax1, ax2):
        ax.spines[["top", "right"]].set_visible(False)
    fig.savefig(os.path.join(out, "fig1_decay.png"), dpi=150, bbox_inches="tight")
    plt.close(fig)


def fig2_lifetime(all_data, out):
    fig = plt.figure(figsize=(13, 8))
    gs = fig.add_gridspec(2, 3, hspace=0.35, wspace=0.25)
    bins = np.linspace(0, 3.2, 33)
    grid = np.linspace(0, 3.2, 400)

    for col, mode in enumerate(("fixed", "exp", "uniform")):
        ax = fig.add_subplot(gs[0, col])
        life = all_data[mode]["parts"]["life"].to_numpy()
        st = SERIES[mode]
        ax.hist(life, bins=bins, density=True, color=st["color"], alpha=0.85,
                edgecolor=SURFACE, linewidth=0.8)
        if mode == "fixed":
            ax.axvline(LIFE_FIXED, color=INK, lw=1.6)
            theory = "退化分布：全部寿命 = 2.0s，方差为 0"
        elif mode == "exp":
            ax.plot(grid, np.exp(-grid / EXP_MEAN) / EXP_MEAN, color=INK,
                    lw=1.8, label="指数理论密度")
            theory = f"指数理论密度 f(t)=λe^(-λt)，λ=1/{EXP_MEAN}"
            ax.legend(frameon=False, fontsize=9, loc="upper right")
        else:
            ax.plot(grid, np.full_like(grid, 1 / (LIFE_HI - LIFE_LO)),
                    color=INK, lw=1.8, label="均匀理论密度")
            theory = "均匀理论密度：区间内恒定"
            ax.legend(frameon=False, fontsize=9, loc="upper right")
        ax.set_title(f"{st['label']}\n{theory}", fontsize=10.5, loc="left")
        ax.set_xlabel("寿命（秒）")
        ax.set_ylabel("概率密度")
        ax.spines[["top", "right"]].set_visible(False)

    # bottom row: empirical CDF vs both theoretical CDFs, one panel each for
    # the two real distributions — KS distance says which family is right
    for col, mode in enumerate(("exp", "uniform")):
        ax = fig.add_subplot(gs[1, col])
        life = np.sort(all_data[mode]["parts"]["life"].to_numpy())
        ecdf = np.arange(1, len(life) + 1) / len(life)
        ax.step(life, ecdf, where="post", color=SERIES[mode]["color"], lw=2,
                label="经验 CDF（样本）")
        ax.plot(grid, cdf_exp(grid), color=INK, lw=1.4, ls="--",
                label=f"指数 CDF，KS={ks_stat(life, cdf_exp):.3f}")
        ax.plot(grid, cdf_uniform(grid), color=MUTED, lw=1.4, ls=":",
                label=f"均匀 CDF，KS={ks_stat(life, cdf_uniform):.3f}")
        ax.set_title(f"{'指数' if mode == 'exp' else '均匀'}模式的分布拟合检验",
                     fontsize=10.5, loc="left")
        ax.set_xlabel("寿命（秒）")
        ax.set_ylabel("累积概率")
        ax.set_xlim(0, 3.2)
        ax.legend(frameon=False, fontsize=9, loc="lower right")
        ax.spines[["top", "right"]].set_visible(False)

    ax = fig.add_subplot(gs[1, 2])
    ax.axis("off")
    exp_life = all_data["exp"]["parts"]["life"].to_numpy()
    uni_life = all_data["uniform"]["parts"]["life"].to_numpy()
    ks_exp_vs_exp = ks_stat(exp_life, cdf_exp)
    ks_exp_vs_uni = ks_stat(exp_life, cdf_uniform)
    ks_uni_vs_uni = ks_stat(uni_life, cdf_uniform)
    ks_uni_vs_exp = ks_stat(uni_life, cdf_exp)
    msg = (
        "读法\n\n"
        "• KS 距离越小，经验分布与该理论族越吻合。\n"
        f"• 指数模式：对指数 CDF 的 KS={ks_exp_vs_exp:.3f}，"
        f"对均匀 CDF 的 KS={ks_exp_vs_uni:.3f}；\n"
        "  右偏、众数贴近 0、长尾，指数族正确。\n"
        f"• 均匀模式：对均匀 CDF 的 KS={ks_uni_vs_uni:.3f}，"
        f"对指数 CDF 的 KS={ks_uni_vs_exp:.3f}。\n"
        "• 固定寿命是方差为 0 的退化分布，没有任何连续 CDF 能拟合，\n"
        "  对应衰减曲线只能是断崖。"
    )
    ax.text(0.0, 0.95, msg, va="top", ha="left", fontsize=10,
            color="#52514e", linespacing=1.7)
    fig.suptitle("寿命分布检验：样本直方图与理论 CDF 对照", fontsize=14, x=0.06,
                 ha="left", y=0.98)
    fig.savefig(os.path.join(out, "fig2_lifetime.png"), dpi=150,
                bbox_inches="tight")
    plt.close(fig)


def fig3_batch_grids(all_data, out):
    """One image per mode: scatter snapshots + per-batch tables."""
    snaps_all = pd.concat([d["snaps"] for d in all_data.values()])
    lim = max(np.abs(snaps_all[["x", "y"]].to_numpy()).max() * 1.05, 1.0)

    for mode in ("fixed", "exp", "uniform"):
        data = all_data[mode]
        fig = plt.figure(figsize=(15, 6.6))
        gs = fig.add_gridspec(2, len(SNAP_FRAMES), height_ratios=[4, 1.6],
                              hspace=0.08, wspace=0.18)
        for j, fr in enumerate(SNAP_FRAMES):
            ax = fig.add_subplot(gs[0, j])
            sub = data["snaps"][data["snaps"]["frame"] == fr]
            t = fr / 30.0
            is_b0 = sub["batch"] == 0
            ax.scatter(sub.loc[is_b0, "x"], sub.loc[is_b0, "y"],
                       s=22, color=BATCH0_COLOR, alpha=0.8,
                       edgecolor=SURFACE, linewidth=0.6, label="第 0 批（爆发批）")
            ax.scatter(sub.loc[~is_b0, "x"], sub.loc[~is_b0, "y"],
                       s=18, color=LATER_COLOR, alpha=0.7,
                       edgecolor=SURFACE, linewidth=0.6, label="后续批次")
            ax.set_xlim(-lim, lim)
            ax.set_ylim(-lim, lim)
            ax.set_aspect("equal", adjustable="box")
            ax.set_title(f"t = {t:.1f}s（帧 {fr}）", fontsize=11)
            ax.set_xlabel("x")
            if j == 0:
                ax.set_ylabel("y")
            ax.spines[["top", "right"]].set_visible(False)
            if j == 0:
                ax.legend(frameon=False, fontsize=9, loc="upper left")

            # table cell: per-batch alive count + mean brightness
            tax = fig.add_subplot(gs[1, j])
            tax.axis("off")
            bt = data["batches"]
            rows = bt[bt["frame"] == fr].sort_values("batch")
            cell_text, show_idx = [], []
            for _, r in rows.iterrows():
                if r["alive"] == 0 and r["batch"] > 0 and fr == 0:
                    continue
                show_idx.append(int(r["batch"]))
                cell_text.append([str(int(r["batch"])), str(int(r["alive"])),
                                  f"{r['mean_brightness']:.3f}"])
            tbl = tax.table(
                cellText=cell_text,
                colLabels=["批次", "存活", "平均亮度"],
                cellLoc="center", loc="upper left",
                colWidths=[0.28, 0.28, 0.44])
            tbl.auto_set_font_size(False)
            tbl.set_fontsize(9)
            tbl.scale(1.0, 1.25)
            for (r, c), cell in tbl.get_celld().items():
                cell.set_edgecolor("#e1e0d9")
                cell.set_linewidth(0.7)
                cell.set_facecolor(SURFACE)
                if r == 0:
                    cell.set_facecolor("#f0efec")
                    cell.get_text().set_color("#52514e")
                else:
                    cell.get_text().set_color("#52514e")

        fig.suptitle(f"按生成帧分批快照 — {SERIES[mode]['label']}",
                     fontsize=14, x=0.04, ha="left", y=1.0)
        fig.savefig(os.path.join(out, f"fig3_batch_grid_{mode}.png"),
                    dpi=150, bbox_inches="tight")
        plt.close(fig)

        # machine-readable per-batch snapshot table
        bt = data["batches"][data["batches"]["frame"].isin(SNAP_FRAMES)].copy()
        bt.insert(1, "t", bt["frame"] / 30.0)
        bt.to_csv(os.path.join(out, f"batch_snapshots_{mode}.csv"), index=False)


def fig4_batch0_decay(all_data, out):
    """Generation-to-extinction survival of the first burst (200 particles)."""
    fig, ax = plt.subplots(figsize=(11, 6.4))
    grid = np.linspace(0, 12.2, 500)

    # theory first, thin ink lines
    ax.plot(grid, (grid < LIFE_FIXED).astype(float), color=INK, lw=1.2,
            ls=(0, (2, 2)), alpha=0.8,
            label="理论：固定寿命阶跃 S(t)=1[t<2.0]")
    ax.plot(grid, np.exp(-grid / EXP_MEAN), color=INK, lw=1.2, ls="--",
            alpha=0.8, label="理论：指数存活 S(t)=e^(-t/1.5)")
    ax.plot(grid, np.clip((LIFE_HI - grid) / (LIFE_HI - LIFE_LO), 0, 1),
            color=INK, lw=1.2, ls=":", alpha=0.8,
            label="理论：均匀存活 S(t) 线性降到 0")

    for mode in ("fixed", "exp", "uniform"):
        b0 = all_data[mode]["batches"]
        b0 = b0[b0["batch"] == 0].sort_values("frame")
        st = SERIES[mode]
        ax.plot(b0["frame"] / 30.0, b0["alive"] / 200.0, color=st["color"],
                ls=st["ls"], lw=2.2,
                label=f"实测：{st['label'].split('（')[0]}")

    ax.set_xlabel("距本批生成的时间（秒）")
    ax.set_ylabel("第 0 批存活比例 S(t)")
    ax.set_title("单批粒子从生成到消亡的数量衰减曲线：实测与寿命分布的理论存活函数",
                 loc="left", fontsize=13)
    ax.set_xlim(-0.1, 8)
    ax.set_ylim(-0.03, 1.05)
    ax.legend(frameon=False, fontsize=9.5, loc="upper right")
    ax.spines[["top", "right"]].set_visible(False)
    ax.annotate("固定寿命：t=2.0s 一帧之内 1.0 → 0（断崖）",
                xy=(2.0, 0.0), xytext=(2.8, 0.35), fontsize=10, color=MUTED,
                arrowprops=dict(arrowstyle="->", color=MUTED, lw=1))
    fig.savefig(os.path.join(out, "fig4_batch0_decay.png"), dpi=150,
                bbox_inches="tight")
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-dir", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)

    all_data = {m: load_mode(args.in_dir, m) for m in ("fixed", "exp", "uniform")}
    summary = {m: summarize(m, all_data[m]) for m in all_data}

    fig1_decay(all_data, args.out_dir)
    fig2_lifetime(all_data, args.out_dir)
    fig3_batch_grids(all_data, args.out_dir)
    fig4_batch0_decay(all_data, args.out_dir)

    with open(os.path.join(args.out_dir, "analysis_summary.json"), "w",
              encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    for m, s in summary.items():
        print(f"\n== {m} ==")
        for k, v in s.items():
            print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
