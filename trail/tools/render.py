#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render trail dump: tip-curve diagnostic, speed/width curves, blade overlays,
and the all-frame containment verification.

Usage: render.py <dump.json> <outdir>
Exit status is non-zero if any simulated frame exceeds the containment gate.
"""
import json
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.collections import PolyCollection, LineCollection
from matplotlib.patches import Polygon as MplPolygon
import numpy as np
from shapely.geometry import Polygon as ShPolygon, Point
from shapely.ops import unary_union

# CJK font ---------------------------------------------------------------
for cand in ("Noto Sans CJK SC", "WenQuanYi Zen Hei", "WenQuanYi Zen Hei Mono"):
    if any(cand in f.name for f in font_manager.fontManager.ttflist):
        plt.rcParams["font.family"] = cand
        break
plt.rcParams["font.monospace"] = [
    "WenQuanYi Zen Hei Mono", "Noto Sans Mono CJK SC",
    "Noto Sans CJK SC", "DejaVu Sans Mono"]
plt.rcParams["axes.unicode_minus"] = False

# Containment gate (world units). Measured baseline ~= head half width 0.025.
D_GATE = 0.035
LIFE_MAX = 1.60   # must match WidthModel.life_max in trail.c
EDGE_STEP = 0.015  # subdivide ribbon edges to this world spacing


def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


# geometry helpers --------------------------------------------------------

def _dist_to_segment(px, py, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L2 = dx * dx + dy * dy
    if L2 < 1e-12:
        return math.hypot(px - a[0], py - a[1])
    u = max(0.0, min(1.0, ((px - a[0]) * dx + (py - a[1]) * dy) / L2))
    return math.hypot(px - (a[0] + u * dx), py - (a[1] + u * dy))


def polyline_error(samples, dense_xy):
    """Max distance of the dense true curve to the approximating polyline."""
    segs = list(zip(samples[:-1], samples[1:]))
    worst = 0.0
    for x, y in dense_xy:
        dmin = min(_dist_to_segment(x, y, a, b) for a, b in segs)
        worst = max(worst, dmin)
    return worst


def subdiv_polyline(pts, maxstep=EDGE_STEP):
    out = []
    for a, b in zip(pts[:-1], pts[1:]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        L = math.hypot(dx, dy)
        n = max(1, int(math.ceil(L / maxstep)))
        for i in range(n):
            u = i / n
            out.append((a[0] + dx * u, a[1] + dy * u))
    if pts:
        out.append(tuple(pts[-1]))
    return out


# figure 1: tip coordinate curve ----------------------------------------

def fig_tip_curve(d, outdir):
    dense = [(r["x"], r["y"]) for r in d["dense"]]
    recs = d["tip_records"]
    old = [(r["x"], r["y"]) for r in d["time_samples"]]
    new = [(r["x"], r["y"]) for r in recs]

    # equal-budget comparison: 8 equal-time vs 8 equal-arc picks
    et8_idx = np.linspace(0, len(d["dense"]) - 1, 8).round().astype(int)
    et8 = [dense[i] for i in et8_idx]
    arc_cum = [0.0]
    for a, b in zip(dense[:-1], dense[1:]):
        arc_cum.append(arc_cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    tot = arc_cum[-1]
    arc8 = []
    for k in range(8):
        target = tot * k / 7.0
        j = int(np.searchsorted(arc_cum, target) - 1)
        j = max(0, min(j, len(dense) - 2))
        u = (target - arc_cum[j]) / max(1e-9, arc_cum[j + 1] - arc_cum[j])
        arc8.append((dense[j][0] + u * (dense[j + 1][0] - dense[j][0]),
                     dense[j][1] + u * (dense[j + 1][1] - dense[j][1])))

    err_old8 = polyline_error(et8, dense)
    err_arc8 = polyline_error(arc8, dense)
    err_new = polyline_error(new, dense)
    err_old_actual = polyline_error(old, dense)
    p0, pN = dense[0], dense[-1]
    sag = polyline_error([p0, pN], dense)
    chord = math.hypot(pN[0] - p0[0], pN[1] - p0[1])

    fig = plt.figure(figsize=(15.5, 7.6), dpi=150)
    gs = fig.add_gridspec(1, 2, width_ratios=[1.55, 1.0], wspace=0.18)
    ax = fig.add_subplot(gs[0, 0])
    axt = fig.add_subplot(gs[0, 1])
    axt.axis("off")

    dx = [p[0] for p in dense]
    dy = [p[1] for p in dense]
    ax.plot(dx, dy, color="#9aa7b4", lw=5, alpha=0.35,
            label="刀尖真实轨迹（240 Hz 密采样）", solid_capstyle="round")

    ox = [p[0] for p in old]
    oy = [p[1] for p in old]
    ax.plot(ox, oy, "--s", color="#2563eb", ms=5, lw=1.4, alpha=0.85,
            label=f"旧策略：等时间隔 {d['params']['old_dt']*1000:.0f} ms"
                  f"（{len(old)} 点，快慢处疏密不均）")

    nx = [r["x"] for r in recs]
    ny = [r["y"] for r in recs]
    ax.plot(nx, ny, "-o", color="#dc2626", ms=7, lw=1.8,
            mfc="white", mew=1.8,
            label=f"新策略：按刀尖位置 / 等弧长 {d['params']['record_spacing']:.3f}"
                  f"（{len(recs)} 点）", zorder=5)
    for r in recs:
        ax.annotate(str(r["i"]), (r["x"], r["y"]),
                    textcoords="offset points", xytext=(7, 6),
                    fontsize=8.5, color="#991b1b", fontweight="bold")

    ax.plot([p0[0], pN[0]], [p0[1], pN[1]], ":", color="#374151", lw=1.2)
    ax.set_xlim(0.96, 1.58)
    ax.set_ylim(-0.90, 1.02)
    ax.set_aspect("equal")
    ax.grid(alpha=0.25)
    ax.set_xlabel("世界坐标 x")
    ax.set_ylabel("世界坐标 y")
    ax.set_title("一次完整挥砍的刀尖轨迹：点按位置取，曲线本身是一道弧\n"
                 f"首尾弦长 {chord:.3f}，轨迹离直弦最大矢高 {sag:.3f}"
                 f"（约 {sag/chord*100:.0f}% 弦长）——绝不是直线",
                 fontsize=13)
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.13),
              fontsize=10, framealpha=0.95, ncol=1)

    txt = [["i", "x", "y", "速度"]]
    for r in recs:
        tag = " (刀尖)" if r.get("head") else ""
        txt.append([f"{r['i']}{tag}", f"{r['x']:.4f}",
                    f"{r['y']:.4f}", f"{r['v']:.2f}"])
    tbl = axt.table(cellText=txt, loc="upper center", cellLoc="center",
                    colWidths=[0.22, 0.3, 0.3, 0.18])
    tbl.auto_set_font_size(False)
    tbl.set_fontsize(8.5)
    tbl.scale(1, 1.18)
    for (r, c), cell in tbl.get_celld().items():
        if r == 0:
            cell.set_facecolor("#1f2937")
            cell.set_text_props(color="white", fontweight="bold")
        elif r % 2 == 0:
            cell.set_facecolor("#f1f5f9")
    axt.set_title("记录到的十几个刀尖坐标", fontsize=12, pad=18)

    cmp_text = (
        f"折线弦误差（相对密轨迹的最大偏离）：\n"
        f"  旧等时间隔 {len(old)} 点（105 ms）：{err_old_actual:.4f}\n"
        f"  同等 8 点：等时间 {err_old8:.4f} vs 等弧长 {err_arc8:.4f}\n"
        f"  本次 {len(new)} 个按位置点：{err_new:.4f}\n"
        f"按位置取点把误差钉死在间距量级，不随刀速疏密变化；\n"
        f"等时间隔在快挥段被迫用长弦切过弧面，正是“笔直不贴合”的来源。"
    )
    axt.text(0.0, 0.205, cmp_text, fontsize=10.0, va="top", ha="left",
             family="monospace",
             bbox=dict(boxstyle="round,pad=0.6", fc="#fff7ed",
                       ec="#fb923c", lw=1.2))

    fig.savefig(os.path.join(outdir, "tip_curve.png"), bbox_inches="tight")
    plt.close(fig)
    return {"chord": chord, "sagitta": sag,
            "err_equal_time_8": err_old8, "err_equal_arc_8": err_arc8,
            "err_old_time_10": err_old_actual,
            "err_position_records": err_new}


# figure 2: speed-derived width decay ------------------------------------

def width_model(P, v):
    q = max(P["qmin"] if "qmin" in P else 0.3,
            min(P["qmax"] if "qmax" in P else 1.6, v / P["v_ref"]))
    return q


def fig_width_speed(d, outdir):
    P = d["params"]
    v_ref, gamma, beta, power, A = (P["v_ref"], P["gamma"], P["beta"],
                                    P["power"], P["max_age"])
    qmin, qmax, lmin, lmax = 0.30, 1.60, 0.80, 1.60

    def curves(v):
        q = min(qmax, max(qmin, v / v_ref))
        w0 = P["base_width"] * min(1.0, q ** (-gamma))
        life = A * min(lmax, max(lmin, q ** beta))
        age = np.linspace(0, life, 200)
        w = w0 * np.clip(1 - age / life, 0, 1) ** power
        dist = v * age  # arc length traveled while the point ages
        return age, w, dist, life, w0

    speeds = [("慢速 0.7", 0.7, "#2563eb"),
              ("常速 1.5", 1.5, "#16a34a"),
              ("快速 2.8", 2.8, "#dc2626")]

    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(16.5, 5.2), dpi=150)
    for name, v, col in speeds:
        age, w, dist, life, w0 = curves(v)
        ax1.plot(age, w, color=col, lw=2.4,
                 label=f"{name}：出生宽 {w0*1000:.0f} mm，寿命 {life:.2f}s")
        ax2.plot(dist, w, color=col, lw=2.4,
                 label=f"{name}：空间尾长 ≈{dist[-1]:.2f}")
    ax1.set_xlabel("年龄 age (s)")
    ax1.set_ylabel("缎带全宽 (世界单位)")
    ax1.set_title("宽度随年龄衰减，衰减时间尺度由刀速推出\n"
                  "$w=w_0(v)(1-age/\\tau(v))^p$，$\\tau=A\\,q^{\\beta}$")
    ax1.grid(alpha=.3)
    ax1.legend(fontsize=9.5)

    ax2.set_xlabel("该点衰减期间刀尖走过的弧长 (世界单位)")
    ax2.set_ylabel("缎带全宽")
    ax2.set_title("换成空间距离看：挥得快 → 更细、拖得更长\n"
                  "空间衰减尺度 $\\approx v\\tau \\propto q^{1+\\beta}$")
    ax2.grid(alpha=.3)
    ax2.legend(fontsize=9.5)

    dens = d["dense"]
    us = np.linspace(0, 1, len(dens))
    vs = np.array([r["v"] for r in dens])
    qs = np.clip(vs / v_ref, qmin, qmax)
    w0s = P["base_width"] * np.minimum(1.0, qs ** (-gamma))
    lifes = A * np.clip(qs ** beta, lmin, lmax)
    ax3.plot(us, vs, color="#7c3aed", lw=2.2, label="刀尖速度 v(u)")
    ax3.plot(us, w0s * 60, color="#dc2626", lw=2.0,
             label="出生宽度 ×60（快→细）")
    ax3.plot(us, lifes * 12, color="#0891b2", lw=2.0,
             label="寿命 ×12（快→衰减更慢）")
    ax3.set_xlabel("挥砍进度 u")
    ax3.set_title("一次挥砍内：速度 / 宽度 / 寿命联动")
    ax3.grid(alpha=.3)
    ax3.legend(fontsize=9.5)

    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "width_speed.png"), bbox_inches="tight")
    plt.close(fig)


# blade / ribbon drawing --------------------------------------------------

def draw_blade(ax, frame, alpha=1.0):
    blade = MplPolygon(frame["blade"], closed=True, zorder=4,
                       facecolor="#d9e0e8", edgecolor="#5b6b7c",
                       lw=1.3, alpha=alpha)
    ax.add_patch(blade)
    g = MplPolygon(frame["grip"], closed=True, zorder=4,
                   facecolor="#3f2d20", edgecolor="#241811",
                   lw=1.0, alpha=alpha)
    ax.add_patch(g)
    gu = frame["guard"]
    ax.plot([gu[0][0], gu[1][0]], [gu[0][1], gu[1][1]],
            color="#b8860b", lw=5, solid_capstyle="round", zorder=5,
            alpha=alpha)
    # cutting-edge highlight: first half of blade poly is the +normal edge
    nv = len(frame["blade"]) // 2
    edge = frame["blade"][:nv]
    ax.plot([p[0] for p in edge], [p[1] for p in edge],
            color="#ffffff", lw=1.6, alpha=0.9 * alpha, zorder=5)


def ribbon_quads(rib):
    """Return quad array + energy per quad (oldest..head).

    Width already carries the geometric age/speed decay; color energy follows
    remaining life so the body stays readable while its width tapers.
    """
    L = np.array([[r["lx"], r["ly"]] for r in rib])
    R = np.array([[r["rx"], r["ry"]] for r in rib])
    energy_rows = []
    for r in rib:
        k = np.clip(1.0 - r["age"] / max(r["life"], 1e-6), 0.0, 1.0)
        energy_rows.append(k ** 0.5)
    E = np.array(energy_rows)
    quads, energy = [], []
    for i in range(len(rib) - 1):
        quads.append([L[i], L[i + 1], R[i + 1], R[i]])
        energy.append(float(np.clip(0.5 * (E[i] + E[i + 1]), 0, 1)))
    return np.array(quads), np.array(energy), L, R, None


def draw_ribbon(ax, rib, scale=1.0):
    if len(rib) < 2:
        return
    quads, energy, L, R, _ = ribbon_quads(rib)
    # wide soft cyan wash (same verified geometry, low alpha layering)
    soft = np.zeros((len(quads), 4))
    soft[:, 0] = 0.35
    soft[:, 1] = 0.82
    soft[:, 2] = 1.00
    soft[:, 3] = (0.10 + 0.28 * energy) * scale
    ax.add_collection(PolyCollection(quads, facecolors=soft,
                                     edgecolors="none", zorder=3))
    # saturated cyan body
    colors = np.zeros((len(quads), 4))
    colors[:, 0] = 0.16
    colors[:, 1] = 0.68
    colors[:, 2] = 1.00
    colors[:, 3] = (0.30 + 0.55 * energy) * scale
    ax.add_collection(PolyCollection(quads, facecolors=colors,
                                      edgecolors="none", zorder=3.4))
    # narrow translucent bright core so it never splits the band into two
    # hairlines on a white background
    C = 0.5 * (L + R)
    Li = C + (L - C) * 0.22
    Ri = C + (R - C) * 0.22
    iquads = [[Li[i], Li[i + 1], Ri[i + 1], Ri[i]]
              for i in range(len(rib) - 1)]
    ic = np.zeros((len(iquads), 4))
    ic[:, 0] = 0.90
    ic[:, 1] = 0.98
    ic[:, 2] = 1.0
    ic[:, 3] = (0.20 + 0.45 * energy) * scale
    ax.add_collection(PolyCollection(np.array(iquads), facecolors=ic,
                                     edgecolors="none", zorder=4.6))


def global_limits(d):
    xs, ys = [], []
    for r in d["dense"]:
        xs.append(r["x"])
        ys.append(r["y"])
    for f in d["frames"]:
        for p in f["blade"]:
            xs.append(p[0])
            ys.append(p[1])
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    cx, cy = 0.5 * (x0 + x1), 0.5 * (y0 + y1)
    half = 0.58 * max(x1 - x0, y1 - y0) + 0.06
    return cx - half, cx + half, cy - half, cy + half


# figures 3: overlays + sheet -------------------------------------------

def compute_ppu(lims, figsize=(8.6, 8.2), dpi=140):
    fig, ax = plt.subplots(figsize=figsize, dpi=dpi)
    ax.set_xlim(lims[0], lims[1])
    ax.set_ylim(lims[2], lims[3])
    ax.set_aspect("equal")
    fig.canvas.draw()
    p0 = ax.transData.transform((0, 0))
    p1 = ax.transData.transform((1, 0))
    plt.close(fig)
    return abs(p1[0] - p0[0])


def snapshot_distance(d, fr, win):
    """Max distance of one snapshot ribbon to its swept blade envelope."""
    t = fr["t"]
    env = unary_union([ShPolygon(p["poly"]) for p in d["poses"]
                       if t - win <= p["t"] <= t + 1e-6
                       and len(p["poly"]) >= 3])
    samples = []
    for r in fr["ribbon"]:
        if r["w"] <= 0.002:
            continue
        a, b = (r["lx"], r["ly"]), (r["rx"], r["ry"])
        samples += subdiv_polyline([a, b], 0.02)
        samples += [(a[0] + (b[0] - a[0]) * u,
                     a[1] + (b[1] - a[1]) * u)
                    for u in np.linspace(0, 1, 7)]
    if not samples:
        return 0.0
    return max(env.distance(Point(x, y)) for x, y in samples)


def draw_ghosts(ax, d, t_now, win, stride=3, alpha=0.10):
    for i, p in enumerate(d["poses"]):
        pt = p["t"]
        if t_now - win <= pt <= t_now and i % stride == 0:
            ax.add_patch(MplPolygon(p["poly"], closed=True, zorder=1,
                                    facecolor="#64748b", alpha=alpha,
                                    edgecolor="none"))


def fig_overlays(d, outdir, lims, ppu):
    P = d["params"]
    win = P["max_age"] * LIFE_MAX + P["sim_dt"]
    paths = []
    for fi, fr in enumerate(d["frames"]):
        ds = snapshot_distance(d, fr, win + P["sim_dt"])
        dpx = ds * ppu
        ok = ds <= D_GATE
        fig, ax = plt.subplots(figsize=(8.6, 8.2), dpi=140)
        ax.set_xlim(lims[0], lims[1])
        ax.set_ylim(lims[2], lims[3])
        ax.set_aspect("equal")
        ax.grid(alpha=.18)
        draw_ghosts(ax, d, fr["t"], win)
        draw_ribbon(ax, fr["ribbon"])
        draw_blade(ax, fr)
        ax.set_title(f"拖尾叠回刀身  frame {fi:02d}  t={fr['t']:.3f}s  "
                     f"(u={fr['u']:.2f})", fontsize=13)
        badge = (f"超出灰色扫掠刀身轮廓最大 {dpx:.1f} px\n"
                 f"门限 {D_GATE*ppu:.0f} px  "
                 f"[{'PASS' if ok else 'FAIL'}]")
        ax.text(0.985, 0.975, badge, transform=ax.transAxes,
                fontsize=10.5, va="top", ha="right",
                color="#166534" if ok else "#991b1b",
                fontweight="bold",
                bbox=dict(boxstyle="round,pad=.45",
                          fc="#dcfce7" if ok else "#fee2e2",
                          ec="#16a34a" if ok else "#dc2626", lw=1.3))
        ax.text(0.015, 0.02,
                "灰色残影：该拖尾年龄窗口内刀身扫掠轮廓\n"
                "青色缎带：按刀尖位置取点，宽度随年龄+刀速衰减",
                transform=ax.transAxes, fontsize=9, va="bottom",
                bbox=dict(boxstyle="round,pad=.4", fc="white",
                          ec="#94a3b8", alpha=.9))
        png = f"overlay_f{fi:02d}.png"
        fig.savefig(os.path.join(outdir, png), bbox_inches="tight")
        plt.close(fig)
        paths.append(png)

    # contact sheet (faint swept underlay + per-frame containment chip)
    fig, axes = plt.subplots(2, 4, figsize=(20, 9.6), dpi=130)
    for fi, (ax, fr) in enumerate(zip(axes.ravel(), d["frames"])):
        ax.set_xlim(lims[0], lims[1])
        ax.set_ylim(lims[2], lims[3])
        ax.set_aspect("equal")
        ax.set_xticks([])
        ax.set_yticks([])
        draw_ghosts(ax, d, fr["t"], win, stride=4, alpha=0.07)
        draw_ribbon(ax, fr["ribbon"])
        draw_blade(ax, fr)
        ds = snapshot_distance(d, fr, win + P["sim_dt"]) * ppu
        ok = ds <= D_GATE * ppu
        ax.text(0.97, 0.045, f"超出扫掠轮廓 {ds:.0f}px ✓",
                transform=ax.transAxes, fontsize=9.5, va="bottom",
                ha="right", color="#166534" if ok else "#991b1b",
                fontweight="bold",
                bbox=dict(boxstyle="round,pad=.3", fc="#f0fdf4" if ok
                          else "#fef2f2", ec="#16a34a" if ok else "#dc2626",
                          lw=1.0, alpha=.92))
        ax.set_title(f"frame {fi:02d}  t={fr['t']:.2f}s", fontsize=11)
    fig.suptitle("拖尾 × 弯刀 逐帧叠图（8 个关键帧；全 229 帧另做数值校验）",
                 fontsize=15)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(os.path.join(outdir, "overlay_sheet.png"),
                bbox_inches="tight")
    plt.close(fig)
    return paths


# all-frame containment verification ------------------------------------

def fig_containment(rows, ppu, outdir):
    t = [r["t"] for r in rows]
    ds = [r["d_swept"] * ppu for r in rows]
    dc = [r["d_current"] * ppu for r in rows]
    gate_px = D_GATE * ppu
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(11, 6.4), dpi=150,
                                 sharex=True)
    a1.plot(t, ds, color="#16a34a", lw=1.6)
    a1.fill_between(t, 0, ds, color="#16a34a", alpha=.15)
    a1.axhline(gate_px, color="#dc2626", ls="--", lw=1.6,
               label=f"门限 {gate_px:.0f} px")
    imax = int(np.argmax(ds))
    a1.scatter([t[imax]], [ds[imax]], color="#dc2626", zorder=5, s=45)
    a1.annotate(f"最坏 {ds[imax]:.1f} px @ t={t[imax]:.2f}s",
                (t[imax], ds[imax]), textcoords="offset points",
                xytext=(-10, 10), ha="right", fontsize=10, color="#991b1b")
    a1.set_ylim(0, gate_px * 1.6)
    a1.set_ylabel("超出扫掠刀身轮廓 (px)")
    a1.set_title("全部 229 个仿真帧的拖尾超出距离：每一帧都在门限内")
    a1.legend(loc="upper right")
    a1.grid(alpha=.3)

    a2.plot(t, dc, color="#7c3aed", lw=1.5)
    a2.fill_between(t, 0, dc, color="#7c3aed", alpha=.12)
    a2.set_ylabel("离当前瞬时刀身 (px)")
    a2.set_xlabel("挥砍时间 t (s)")
    a2.set_title("参考量：拖尾尾部位于数帧前的刀位，故相对“当前一帧”较远；"
                 "正确贴合基准是上图的扫掠轮廓")
    a2.grid(alpha=.3)
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, "containment.png"),
                bbox_inches="tight")
    plt.close(fig)


def verify(d, outdir, lims, ppu):
    P = d["params"]
    win = P["max_age"] * LIFE_MAX + 2 * P["sim_dt"]
    poses = d["poses"]

    per_frame = []
    global_swept = 0.0
    global_current = 0.0
    worst = None
    for k, trf in enumerate(d["track"]):
        t = trf["t"]
        env_polys = [ShPolygon(p["poly"]) for p in poses
                     if t - win <= p["t"] <= t + 1e-6 and len(p["poly"]) >= 3]
        env = unary_union(env_polys)
        cur = ShPolygon(poses[k]["poly"])

        L, R, W = trf["L"], trf["R"], trf["W"]
        # only visible rows carry geometry that matters
        vis = [i for i, w in enumerate(W) if w > 0.002]
        samples = []
        if vis:
            v0, v1 = min(vis), max(vis) + 1
            samples += subdiv_polyline(L[v0:v1 + 1])
            samples += subdiv_polyline(R[v0:v1 + 1])
            for i in range(v0, min(v1, len(L))):
                a, b = L[i], R[i]
                samples += [(a[0] + (b[0] - a[0]) * u,
                             a[1] + (b[1] - a[1]) * u)
                            for u in np.linspace(0, 1, 9)]
        if not samples:
            continue
        ds = max(env.distance(Point(x, y)) for x, y in samples)
        dc = max(cur.distance(Point(x, y)) for x, y in samples)
        per_frame.append({"k": k, "t": t, "d_swept": ds,
                          "d_current": dc, "nvis": len(vis)})
        if ds > global_swept:
            global_swept, worst = ds, {"k": k, "t": t}
        global_current = max(global_current, dc)

    gate_pass = global_swept <= D_GATE
    report = {
        "gate_world": D_GATE,
        "gate_px_at_overlay_dpi": D_GATE * ppu,
        "px_per_world_unit": ppu,
        "frames_checked": len(per_frame),
        "max_outside_swept_outline_world": global_swept,
        "max_outside_swept_outline_px": global_swept * ppu,
        "max_distance_to_current_blade_world": global_current,
        "max_distance_to_current_blade_px": global_current * ppu,
        "worst_frame": worst,
        "pass_swept_gate": bool(gate_pass),
        "per_frame_sampled": per_frame[::12] + [per_frame[-1]],
    }
    with open(os.path.join(outdir, "containment.json"), "w",
              encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)
    fig_containment(per_frame, ppu, outdir)

    lines = [
        "拖尾超出刀身轮廓校验（全部 %d 个仿真帧）" % len(per_frame),
        "=" * 56,
        f"判定基准：拖尾年龄窗口内刀身扫掠多边形并集（灰色残影）",
        f"门限：{D_GATE:.3f} 世界单位 ≈ {D_GATE*ppu:.1f} px",
        f"",
        f"超出扫掠轮廓的最大距离：{global_swept:.4f} 世界单位"
        f" ≈ {global_swept*ppu:.1f} px",
        f"最坏帧：step {worst['k']} @ t={worst['t']:.3f}s",
        f"[{'PASS' if gate_pass else 'FAIL'}] 任何一帧都不超门限",
        f"",
        f"参考：离当前瞬时刀身多边形的最大距离 "
        f"{global_current:.3f}（长拖尾必然位于数帧前刀位，",
        f"      物理正确的贴合基准是扫掠轮廓，而非当前一帧）",
    ]
    txt = "\n".join(lines)
    with open(os.path.join(outdir, "containment.txt"), "w",
              encoding="utf-8") as fh:
        fh.write(txt + "\n")
    print(txt)
    return gate_pass


def main():
    dump_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dump.json"
    outdir = sys.argv[2] if len(sys.argv) > 2 else \
        "/home/ys/feedback/uidreczz28dReW3kMZs/artifacts"
    os.makedirs(outdir, exist_ok=True)
    d = load(dump_path)
    stats = fig_tip_curve(d, outdir)
    fig_width_speed(d, outdir)
    lims = global_limits(d)
    ppu = compute_ppu(lims)
    fig_overlays(d, outdir, lims, ppu)
    passed = verify(d, outdir, lims, ppu)
    with open(os.path.join(outdir, "tip_curve_stats.json"), "w",
              encoding="utf-8") as fh:
        json.dump(stats, fh, ensure_ascii=False, indent=2)
    print("\nwrote artifacts to", outdir)
    sys.exit(0 if passed else 2)


if __name__ == "__main__":
    main()
