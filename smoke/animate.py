#!/usr/bin/env python3
"""把两段 10s 录制中所有含粒子的帧（frame 5..600，共 596 帧）做成并排回放（MP4）。

这是“每颗粒子、每一帧都画箭头”的完整形态：静态 PNG 只抽样了 6 个时刻，
视频逐帧回放全部数据。用法: python3 animate.py
"""
import csv
import shutil
import subprocess
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation, FFMpegWriter
from matplotlib.colors import LinearSegmentedColormap

HERE = Path(__file__).resolve().parent
ART = HERE.parent / "artifacts"

plt.rcParams.update({
    "font.family": ["Noto Sans CJK SC", "WenQuanYi Zen Hei", "DejaVu Sans"],
    "axes.unicode_minus": False,
})
C_GRAV = "#2a78d6"; INK2 = "#52514e"; GRID = "#e1e0d9"; BASELINE = "#c3c2b7"
SPEED_RAMP = ["#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#0d366b"]
GAIN = 0.16
FPS_OUT = 60          # 与录制同帧率，600 个数据帧逐帧编码，回放正好 10s
STRIDE = 1


def load_groups(name: str):
    with open(ART / name, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    col = lambda k: np.array([float(r[k]) for r in rows])
    frames = col("frame").astype(int)
    data = {k: col(k) for k in ("x", "y", "vx", "vy")}
    groups = {}
    for fi in np.unique(frames):
        m = frames == fi
        groups[int(fi)] = {k: v[m] for k, v in data.items()}
    return groups


def style(ax):
    ax.grid(True, color=GRID, lw=0.7)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.scatter([0], [0], marker="s", s=70, color=INK2, zorder=6)


def main():
    gb = load_groups("before_10s.csv")
    ga = load_groups("after_10s.csv")
    fids = sorted(set(gb) & set(ga))[::STRIDE]

    cmap = LinearSegmentedColormap.from_list("spd", SPEED_RAMP[1:])
    fig = plt.figure(figsize=(15.5, 7.6))
    gs = fig.add_gridspec(1, 2, width_ratios=[1.0, 4.4],
                          left=0.05, right=0.93, top=0.86, bottom=0.08, wspace=0.18)
    axb = fig.add_subplot(gs[0, 0])
    axa = fig.add_subplot(gs[0, 1])

    axb.set_xlim(-5.5, 5.5); axb.set_ylim(-78, 7)
    axa.set_xlim(-4.5, 3.5); axa.set_ylim(-1.5, 12.5)
    for ax, t in ((axb, "诊断前 g_y=-16：箭头整体向下、烟柱急速拉长"),
                  (axa, "只调重力大头后 g_y=+1.4：箭头整体向上、范围收敛")):
        ax.set_aspect("equal", adjustable="box")
        ax.set_xlabel("x / m"); ax.set_ylabel("y / m")
        ax.set_title(t, fontsize=12.5, fontweight="bold", pad=10)
        style(ax)

    qb = axb.quiver(np.zeros(64), np.zeros(64), np.zeros(64), np.zeros(64), np.zeros(64),
                    cmap=cmap, norm=plt.Normalize(0, 27),
                    angles="xy", scale_units="xy", scale=1,
                    width=0.014, headwidth=4.6, headlength=5.4,
                    headaxislength=4.4, edgecolor="white", linewidth=0.4)
    qa = axa.quiver(np.zeros(64), np.zeros(64), np.zeros(64), np.zeros(64), np.zeros(64),
                    cmap=cmap, norm=plt.Normalize(0, 27),
                    angles="xy", scale_units="xy", scale=1,
                    width=0.006, headwidth=4.2, headlength=5,
                    headaxislength=4, edgecolor="white", linewidth=0.35)
    NSLOT = 64  # 稳态每帧最多 56 颗粒子；空位用零向量，Quiver 不支持运行中改箭头数
    title = fig.suptitle("", fontsize=15, fontweight="bold", y=0.965)
    cax = fig.add_axes([0.945, 0.18, 0.013, 0.6])
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=plt.Normalize(0, 27))
    cb = fig.colorbar(sm, cax=cax); cb.set_label("速率 |v| / (m·s⁻¹)")

    def update(k):
        fi = fids[k]
        for q, g in ((qb, gb[fi]), (qa, ga[fi])):
            n = len(g["x"])
            ox = np.zeros(NSLOT); oy = np.zeros(NSLOT)
            ou = np.zeros(NSLOT); ov = np.zeros(NSLOT); oc = np.zeros(NSLOT)
            sp = np.hypot(g["vx"], g["vy"])
            ox[:n] = g["x"]; oy[:n] = g["y"]
            ou[:n] = GAIN * g["vx"]; ov[:n] = GAIN * g["vy"]; oc[:n] = sp
            q.set_offsets(np.column_stack([ox, oy]))
            q.set_UVC(ou, ov, oc)
        title.set_text(f"每颗粒子每帧速度矢量回放　t = {fi / 60:5.2f} s　（60 fps 逐帧）")
        return qb, qa, title

    anim = FuncAnimation(fig, update, frames=len(fids), interval=1000 / FPS_OUT, blit=False)
    mp4 = ART / "quiver_playback.mp4"
    ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
    plt.rcParams["animation.ffmpeg_path"] = ffmpeg
    writer = FFMpegWriter(fps=FPS_OUT, codec="libx264",
                          extra_args=["-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2",
                                      "-pix_fmt", "yuv420p", "-crf", "23"])
    anim.save(mp4, writer=writer, dpi=130)
    print("wrote", mp4)

    # 压一个轻量 GIF 便于直接预览
    gif = ART / "quiver_playback.gif"
    pal = ART / ".palette.png"
    subprocess.run(["ffmpeg", "-y", "-i", str(mp4),
                    "-vf", "fps=12,scale=900:-1:flags=lanczos,palettegen=stats_mode=diff",
                    str(pal)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["ffmpeg", "-y", "-i", str(mp4), "-i", str(pal),
                    "-lavfi", "fps=12,scale=900:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3",
                    str(gif)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pal.unlink(missing_ok=True)
    print("wrote", gif)


if __name__ == "__main__":
    main()
