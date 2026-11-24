#!/usr/bin/env python3
"""录制 before/after 两段 10s 轨迹，计算重力/阻力/涡流的速度增量贡献，
并生成箭头图、贡献占比图和扩散范围对比图。

用法:
  python3 analyze.py            # 录制 params_before.json + params.json 并出全部产物
  python3 analyze.py before     # 只录制/分析基线（诊断第一步）
"""
import csv
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ART = ROOT / "artifacts"
ART.mkdir(exist_ok=True)

# ---- dataviz 参考色板（前三分类色，all-pairs 验证通过） ----
C_GRAV = "#2a78d6"   # 重力 蓝
C_DRAG = "#eb6834"   # 阻力 橙
C_SWIRL = "#1baf7a"  # 涡流 青
INK = "#0b0b0b"
INK2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
BASELINE = "#c3c2b7"
SURF = "#fcfcfb"
BAD = "#d03b3b"
GOOD = "#0ca30c"
SPEED_RAMP = ["#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#0d366b"]

plt.rcParams.update({
    "font.family": ["Noto Sans CJK SC", "WenQuanYi Zen Hei", "DejaVu Sans"],
    "axes.unicode_minus": False,
    "figure.facecolor": "white",
    "axes.facecolor": SURF,
    "axes.edgecolor": BASELINE,
    "axes.labelcolor": INK2,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "text.color": INK,
    "axes.titlecolor": INK,
    "font.size": 10.5,
})

COLS = ["frame", "t", "id", "x", "y", "vx", "vy",
        "dvx_g", "dvy_g", "dvx_d", "dvy_d", "dvx_s", "dvy_s"]


def record(preset: Path, csv_path: Path) -> None:
    subprocess.run([sys.executable, str(HERE / "record_run.py"),
                    str(preset), str(csv_path)], check=True)


def load(csv_path: Path) -> dict:
    with open(csv_path, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    d = {k: np.array([float(r[k]) for r in rows]) for k in COLS}
    d["frame"] = d["frame"].astype(int)
    d["id"] = d["id"].astype(int)
    return d


def contribution(d: dict) -> dict:
    """三种力对每帧速度变化的贡献。主口径：逐帧冲量模长求和。"""
    g = np.hypot(d["dvx_g"], d["dvy_g"])
    dr = np.hypot(d["dvx_d"], d["dvy_d"])
    sw = np.hypot(d["dvx_s"], d["dvy_s"])
    tot = g.sum() + dr.sum() + sw.sum()

    # 口径二：每帧增量的 RMS
    rms_g = float(np.sqrt(np.mean(g ** 2)))
    rms_d = float(np.sqrt(np.mean(dr ** 2)))
    rms_s = float(np.sqrt(np.mean(sw ** 2)))
    rms_tot = rms_g + rms_d + rms_s

    # 口径三：每一颗粒子每一帧，谁是最大单项
    stack = np.vstack([g, dr, sw])
    argmax = np.argmax(stack, axis=0)
    am = [(argmax == i).mean() for i in range(3)]

    # 竖直方向的有符号贡献（解释“往下拽”来自谁）
    signed_y = {
        "gravity": float(d["dvy_g"].sum()),
        "drag": float(d["dvy_d"].sum()),
        "swirl": float(d["dvy_s"].sum()),
    }

    return {
        "l1_total": float(tot),
        "l1": {"gravity": float(g.sum()), "drag": float(dr.sum()), "swirl": float(sw.sum())},
        "share": {
            "gravity": float(g.sum() / tot),
            "drag": float(dr.sum() / tot),
            "swirl": float(sw.sum() / tot),
        },
        "rms": {"gravity": rms_g, "drag": rms_d, "swirl": rms_s},
        "rms_share": {
            "gravity": rms_g / rms_tot, "drag": rms_d / rms_tot, "swirl": rms_s / rms_tot
        },
        "argmax_share": {"gravity": float(am[0]), "drag": float(am[1]), "swirl": float(am[2])},
        "signed_dvy_sum": signed_y,
    }


def direction_stats(d: dict) -> dict:
    ang = np.degrees(np.arctan2(d["vy"], d["vx"]))  # 90° = 竖直向上
    return {
        "up_fraction_all": float((d["vy"] > 0).mean()),
        "up_fraction_t_ge_2": float((d["vy"][d["t"] >= 2.0] > 0).mean()),
        "median_angle_deg": float(np.median(ang)),
        "median_vy": float(np.median(d["vy"])),
        "mean_speed": float(np.hypot(d["vx"], d["vy"]).mean()),
    }


def spread_curves(d: dict) -> dict:
    frames = np.unique(d["frame"])
    out = {"frame": [], "t": [], "n": [], "mean_x": [], "mean_y": [],
           "std_x": [], "std_y": [], "rms_r": [], "p90_r": [], "max_r": []}
    for fi in frames:
        m = d["frame"] == fi
        x, y = d["x"][m], d["y"][m]
        r = np.hypot(x, y)
        out["frame"].append(int(fi))
        out["t"].append(float(d["t"][m][0]))
        out["n"].append(int(m.sum()))
        out["mean_x"].append(float(x.mean()))
        out["mean_y"].append(float(y.mean()))
        out["std_x"].append(float(x.std()))
        out["std_y"].append(float(y.std()))
        out["rms_r"].append(float(np.sqrt((x ** 2 + y ** 2).mean())))
        out["p90_r"].append(float(np.percentile(r, 90)))
        out["max_r"].append(float(r.max()))
    return {k: np.array(v) for k, v in out.items()}


def snapshot_rows(d: dict, t_target: float):
    idx = np.where(np.isclose(d["t"], t_target, atol=1 / 120))[0]
    if len(idx) == 0:  # 找最近帧
        fi = int(round(t_target * 60))
        idx = np.where(d["frame"] == fi)[0]
    return idx


def trails_until(d: dict, t_max: float):
    """按 id 组织 t<=t_max 的轨迹线。"""
    m = d["t"] <= t_max + 1e-9
    ids = np.unique(d["id"][m])
    for pid in ids:
        mm = m & (d["id"] == pid)
        yield d["x"][mm], d["y"][mm]


def style_axes(ax):
    ax.grid(True, color=GRID, lw=0.7, alpha=0.9)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(BASELINE)


def make_quiver_facets(d: dict, preset: dict, out_png: Path, title: str,
                       times=(0.5, 1.0, 2.0, 3.0, 4.0, 8.0)) -> None:
    """2x3 速度箭头快照。

    每个面板按该时刻烟柱范围自适应取景 + 等纵横比，保证箭头方向不被
    坐标轴拉伸；箭头用统一的数据长度增益，长度仍与速度成正比。
    各面板尺度不同，由坐标轴读数与标题中的统计负责跨时刻比较。
    """
    forces = preset["forces"]
    fig, axes = plt.subplots(2, 3, figsize=(17.0, 9.4))
    fig.subplots_adjust(left=0.055, right=0.905, top=0.86, bottom=0.08,
                        wspace=0.40, hspace=0.34)
    axes = axes.ravel()

    speed = np.hypot(d["vx"], d["vy"])
    smax = np.percentile(speed, 95)
    cmap = LinearSegmentedColormap.from_list("spd", SPEED_RAMP[1:])
    gain = 0.16  # 箭头数据长度 = gain * |v|

    last_q = None
    for ax, tt in zip(axes, times):
        idx = snapshot_rows(d, tt)
        ax.scatter([0], [0], marker="s", s=58, color=INK2, zorder=6,
                   label="发射口 (0,0)")
        if len(idx):
            last_q = ax.quiver(
                d["x"][idx], d["y"][idx],
                gain * d["vx"][idx], gain * d["vy"][idx],
                speed[idx], cmap=cmap, norm=plt.Normalize(0, smax),
                angles="xy", scale_units="xy", scale=1.0,
                width=0.0055, headwidth=4.0, headlength=4.8,
                headaxislength=3.8, edgecolor="white", linewidth=0.35, zorder=5,
            )
            xp, yp = d["x"][idx], d["y"][idx]
            yspan = max(float(np.ptp(yp)), 1.5)
            xspan = max(float(np.ptp(xp)), 1.0)
            ymid = 0.5 * (yp.max() + yp.min())
            h = yspan * 0.5 + max(yspan * 0.18, 0.6)
            xh = max(xspan * 0.5 + xspan * 0.35, h * 0.42, 1.1)
        else:
            ymid, h, xh = 0.0, 2.0, 1.5
        ax.set_xlim(-xh, xh)
        ax.set_ylim(ymid - h, ymid + h)
        ax.set_aspect("equal", adjustable="box")
        style_axes(ax)
        n = len(idx)
        up = (d["vy"][idx] > 0).mean() if n else 0.0
        med_ang = np.degrees(np.arctan2(
            np.median(d["vy"][idx]) if n else 0.0,
            np.median(d["vx"][idx]) if n else 1.0))
        ax.set_title(f"t = {tt:g} s    n = {n}    向上箭头 {up:.0%}    "
                     f"中位方向 {med_ang:.0f}°", fontsize=11, pad=6)
        ax.set_xlabel("x / m")
        ax.set_ylabel("y / m")

    if last_q is not None:
        cax = fig.add_axes([0.925, 0.16, 0.014, 0.62])
        cb = fig.colorbar(last_q, cax=cax)
        cb.set_label("速率 |v| / (m·s⁻¹)\n箭头长度 = "
                     f"{gain:g} × 速度", color=INK2, fontsize=10)
        cb.ax.tick_params(color=MUTED)

    gy = forces["gravity_y"]
    gy_note = "正值＝向上净浮力" if gy > 0 else "负值＝向下重力"
    fig.suptitle(title, fontsize=16, fontweight="bold", y=0.965)
    fig.text(0.5, 0.915,
             f"重力净场 g=(0, {gy:g})，坐标 +y 朝上，{gy_note}    "
             f"阻力 λ={forces['drag']:g}    涡流 ε={forces['swirl']:g}    "
             "等纵横坐标，箭头方向即真实速度方向",
             ha="center", fontsize=11, color=INK2)
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png, dpi=150)
    plt.close(fig)
    print(f"wrote {out_png}")


def make_contribution_chart(cb: dict, ca: dict | None, out_png: Path) -> None:
    labels = ["重力", "阻力", "涡流"]
    keys = ["gravity", "drag", "swirl"]
    colors = [C_GRAV, C_DRAG, C_SWIRL]

    if ca is None:
        fig, ax = plt.subplots(figsize=(8.6, 5.0))
        vals = [cb["share"][k] * 100 for k in keys]
        bars = ax.bar(labels, vals, color=colors, width=0.56, zorder=3)
        for b, k, v in zip(bars, keys, vals):
            xc = b.get_x() + b.get_width() / 2
            ax.text(xc, b.get_height() + 1.2, f"{v:.1f}%",
                    ha="center", va="bottom",
                    fontsize=13, fontweight="bold", color=INK)
            if v >= 14:  # 高柱：总量白字放柱内
                ax.text(xc, b.get_height() / 2,
                        f"Σ|Δv|\n{cb['l1'][k]:.0f}", ha="center", va="center",
                        fontsize=9.5, color="white", fontweight="bold")
            else:        # 矮柱：总量放到百分比上方，避免溢出柱体
                ax.text(xc, b.get_height() + 6.2,
                        f"Σ|Δv|={cb['l1'][k]:.0f}", ha="center", va="bottom",
                        fontsize=9.5, color=INK2)
        ax.set_ylim(0, max(vals) * 1.22)
        ax.set_ylabel("占总速度增量预算比例 / %")
        ax.set_title("基线：三项力场对每帧速度变化的贡献占比", fontweight="bold")
        style_axes(ax)
        fig.tight_layout()
        fig.savefig(out_png, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"wrote {out_png}")
        return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13.2, 5.0),
                                   gridspec_kw={"width_ratios": [1.05, 1.25]})
    # 左：基线占比
    vals = [cb["share"][k] * 100 for k in keys]
    bars = ax1.bar(labels, vals, color=colors, width=0.58, zorder=3)
    for b, k in zip(bars, keys):
        ax1.text(b.get_x() + b.get_width() / 2, b.get_height() + 1.3,
                 f"{cb['share'][k]:.1%}", ha="center", va="bottom",
                 fontsize=12.5, fontweight="bold")
    ax1.set_ylim(0, max(vals) * 1.2)
    ax1.set_ylabel("占总速度增量预算比例 / %")
    ax1.set_title("① 诊断前贡献占比（L1 冲量口径）", fontweight="bold")
    style_axes(ax1)

    # 右：三项 L1 总量 before vs after（同色，实心=前，浅描边=后）
    x = np.arange(3)
    w = 0.36
    before_v = [cb["l1"][k] for k in keys]
    after_v = [ca["l1"][k] for k in keys]
    b1 = ax2.bar(x - w / 2, before_v, w, color=colors, zorder=3, label="诊断前")
    b2 = ax2.bar(x + w / 2, after_v, w, facecolor="white",
                 edgecolor=colors, linewidth=2.2, zorder=3, label="只改重力后")
    for bars_, vals_ in ((b1, before_v), (b2, after_v)):
        for b, v in zip(bars_, vals_):
            ax2.text(b.get_x() + b.get_width() / 2, b.get_height() + max(before_v) * 0.015,
                     f"{v:.0f}", ha="center", va="bottom", fontsize=9.5, color=INK2)
    ax2.set_xticks(x)
    ax2.set_xticklabels(labels)
    ax2.set_ylabel("10 s 内 Σ|Δv·term| 总量")
    ax2.set_title("② 改动前后三项贡献总量（后为空心柱）", fontweight="bold")
    ax2.legend(frameon=False, loc="upper right")
    style_axes(ax2)
    fig.suptitle("重力是占大头的一项 → 只调重力参数", fontsize=14.5, fontweight="bold", y=1.02)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_png}")


def make_spread_chart(sb: dict, sa: dict, out_png: Path) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(16.5, 4.7))

    # 1) 质心高度 + std_y 带
    ax = axes[0]
    for s, name, col, ls in ((sb, "诊断前", BAD, "--"), (sa, "只改重力后", GOOD, "-")):
        t = s["t"]
        ax.plot(t, s["mean_y"], color=col, ls=ls, lw=2.2, label=f"{name} 平均高度", zorder=3)
        ax.fill_between(t, s["mean_y"] - s["std_y"], s["mean_y"] + s["std_y"],
                        color=col, alpha=0.13, lw=0)
    ax.axhline(0, color=BASELINE, lw=1.1)
    ax.set_title("烟柱质心高度（阴影 = ±σ_y）", fontweight="bold")
    ax.set_xlabel("t / s"); ax.set_ylabel("y / m")
    ax.legend(frameon=False, loc="lower left")
    style_axes(ax)

    # 2) 水平扩散 sigma_x
    ax = axes[1]
    ax.plot(sb["t"], sb["std_x"], color=BAD, ls="--", lw=2.2, label="诊断前 σ_x", zorder=3)
    ax.plot(sa["t"], sa["std_x"], color=GOOD, ls="-", lw=2.2, label="改后 σ_x", zorder=3)
    ax.set_title("水平扩散 σ_x（越缓越好）", fontweight="bold")
    ax.set_xlabel("t / s"); ax.set_ylabel("σ_x / m")
    ax.legend(frameon=False)
    style_axes(ax)

    # 3) 整体扩散范围 p90 半径
    ax = axes[2]
    ax.plot(sb["t"], sb["p90_r"], color=BAD, ls="--", lw=2.2, label="诊断前 p90 半径", zorder=3)
    ax.plot(sb["t"], sb["max_r"], color=BAD, ls=":", lw=1.4, alpha=0.7, label="诊断前 最大半径")
    ax.plot(sa["t"], sa["p90_r"], color=GOOD, ls="-", lw=2.2, label="改后 p90 半径", zorder=3)
    ax.plot(sa["t"], sa["max_r"], color=GOOD, ls=":", lw=1.4, alpha=0.7, label="改后 最大半径")
    ax.set_title("整体扩散范围（到发射口半径）", fontweight="bold")
    ax.set_xlabel("t / s"); ax.set_ylabel("r / m")
    ax.legend(frameon=False, fontsize=9)
    style_axes(ax)

    fig.suptitle("「上飘、缓散」量化核对：改前 vs 只调重力之后",
                 fontsize=14.5, fontweight="bold", y=1.02)
    fig.tight_layout()
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_png}")


def make_trajectory_overview(db: dict, da: dict, out_png: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(15, 7.2))
    for ax, d, name, col in ((axes[0], db, "诊断前", BAD), (axes[1], da, "只改重力后", GOOD)):
        for pid in np.unique(d["id"]):
            m = d["id"] == pid
            ax.plot(d["x"][m], d["y"][m], lw=0.8, alpha=0.5, color=C_GRAV)
        ax.scatter([0], [0], marker="s", s=70, color=INK2, zorder=5)
        ax.set_title(f"{name}：全部粒子 10 s 轨迹", fontweight="bold")
        ax.set_xlabel("x / m"); ax.set_ylabel("y / m")
        style_axes(ax)
        # 各自按数据范围取景 + 等比例（坐标轴框会自动收窄，不补空白尺度）
        xspan = max(d["x"].max() - d["x"].min(), 1.0)
        yspan = max(d["y"].max() - d["y"].min(), 1.0)
        cx = 0.5 * (d["x"].max() + d["x"].min())
        cy = 0.5 * (d["y"].max() + d["y"].min())
        ax.set_xlim(cx - 0.85 * xspan - 0.6, cx + 0.85 * xspan + 0.6)
        ax.set_ylim(cy - 0.58 * yspan - 0.4, cy + 0.58 * yspan + 0.4)
        # 概览图用 auto 比例，避免细长烟柱在 equal 比例下把 x 轴刻度挤重叠；
        # 方向真实性由 arrows_*.png 的等纵横箭头图负责。
        ax.set_aspect("auto")
        ax.tick_params(axis="x", labelsize=9.5)
    fig.text(0.5, 0.012, "注：本概览图横、纵轴比例独立；箭头真实角度见 arrows_before / after.png",
             ha="center", fontsize=9.5, color=MUTED)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(out_png, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_png}")


def table_at_times(d: dict, s: dict) -> list[dict]:
    rows = []
    for tt in (1, 2, 3, 4, 8, 10):
        i = np.argmin(np.abs(s["t"] - tt))
        rows.append({
            "t_s": round(float(s["t"][i]), 2),
            "n": int(s["n"][i]),
            "mean_y": round(float(s["mean_y"][i]), 2),
            "std_x": round(float(s["std_x"][i]), 2),
            "std_y": round(float(s["std_y"][i]), 2),
            "p90_r": round(float(s["p90_r"][i]), 2),
            "max_r": round(float(s["max_r"][i]), 2),
        })
    return rows


def print_summary(tag: str, c: dict, dirn: dict) -> None:
    print(f"\n===== {tag} =====")
    print("贡献占比(L1): " + "  ".join(
        f"{k}={c['share'][k]:.1%}" for k in ("gravity", "drag", "swirl")))
    print("RMS 占比:     " + "  ".join(
        f"{k}={c['rms_share'][k]:.1%}" for k in ("gravity", "drag", "swirl")))
    print("最大项帧占比: " + "  ".join(
        f"{k}={c['argmax_share'][k]:.1%}" for k in ("gravity", "drag", "swirl")))
    print(f"向上速度帧: 全部 {dirn['up_fraction_all']:.1%} | t>=2s {dirn['up_fraction_t_ge_2']:.1%}"
          f" | 中位方向角 {dirn['median_angle_deg']:.1f}° (90=竖直向上)"
          f" | 中位 vy {dirn['median_vy']:+.2f}")


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    before_preset = HERE / "params_before.json"
    active_preset = HERE / "params.json"

    before_csv = ART / "before_10s.csv"
    record(before_preset, before_csv)
    db = load(before_csv)
    cb = contribution(db)
    dirb = direction_stats(db)
    sb = spread_curves(db)
    print_summary("诊断前 baseline", cb, dirb)

    result = {
        "before": {
            "params": json.loads(before_preset.read_text(encoding="utf-8")),
            "rows": len(db["t"]),
            "contributions": cb,
            "direction": dirb,
            "table": table_at_times(db, sb),
        }
    }

    make_quiver_facets(db, result["before"]["params"],
                       ART / "arrows_before.png",
                       "诊断前：每颗粒子的速度矢量箭头（10 s 录制，60 fps）")
    make_contribution_chart(cb, None, ART / "contributions_before.png")

    if mode == "before":
        (ART / "analysis.json").write_text(
            json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        return 0

    after_csv = ART / "after_10s.csv"
    record(active_preset, after_csv)
    da = load(after_csv)
    ca = contribution(da)
    dira = direction_stats(da)
    sa = spread_curves(da)
    print_summary("改后 after", ca, dira)

    result["after"] = {
        "params": json.loads(active_preset.read_text(encoding="utf-8")),
        "rows": len(da["t"]),
        "contributions": ca,
        "direction": dira,
        "table": table_at_times(da, sa),
    }
    # 只允许重力项变化的自动核对
    fb = result["before"]["params"]["forces"]
    fa = result["after"]["params"]["forces"]
    changes = {k: (fb[k], fa[k]) for k in fb if float(fb[k]) != float(fa[k])}
    result["changed_force_keys"] = list(changes)
    result["force_changes"] = {k: {"from": v[0], "to": v[1]} for k, v in changes.items()}
    print("参数变化:", changes)

    make_quiver_facets(da, result["after"]["params"],
                       ART / "arrows_after.png",
                       "只调重力大头后：每颗粒子的速度矢量箭头（10 s 复测，60 fps）")
    make_contribution_chart(cb, ca, ART / "contributions.png")
    make_spread_chart(sb, sa, ART / "spread_verify.png")
    make_trajectory_overview(db, da, ART / "trajectories_compare.png")

    (ART / "analysis.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nwrote {ART / 'analysis.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
