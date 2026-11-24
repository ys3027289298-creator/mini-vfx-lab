#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build overlay_swing.gif from the full per-frame track in the dump.

Usage: make_gif.py <dump.json> <out.gif>
Reuses styling helpers from render.py.
"""
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
import numpy as np
from PIL import Image
from shapely.geometry import Polygon as ShPolygon, Point
from shapely.ops import unary_union

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render as R  # noqa: E402


def track_quads(trf):
    L = np.array(trf["L"])
    Ri = np.array(trf["R"])
    n = len(L)
    quads = np.array([[L[i], L[i + 1], Ri[i + 1], Ri[i]]
                      for i in range(n - 1)])
    # oldest row fades, head stays bright
    e = np.array([(i / max(1, n - 1)) ** 0.6 for i in range(n - 1)])
    return quads, e, L, Ri


def draw_track_ribbon(ax, trf):
    if len(trf["L"]) < 2:
        return
    quads, e, L, Ri = track_quads(trf)
    c = np.zeros((len(quads), 4))
    c[:, 1] = 0.72
    c[:, 2] = 1.0
    c[:, 3] = 0.28 + 0.55 * e
    ax.add_collection(PolyCollection(quads, facecolors=c,
                                     edgecolors="none", zorder=3.4))
    C = 0.5 * (L + Ri)
    Li = C + (L - C) * 0.22
    Rii = C + (Ri - C) * 0.22
    iq = np.array([[Li[i], Li[i + 1], Rii[i + 1], Rii[i]]
                   for i in range(len(L) - 1)])
    ic = np.zeros((len(iq), 4))
    ic[:, :3] = (0.90, 0.98, 1.0)
    ic[:, 3] = 0.15 + 0.4 * e
    ax.add_collection(PolyCollection(iq, facecolors=ic,
                                     edgecolors="none", zorder=4.6))


def frame_distance(d, k, win):
    trf = d["track"][k]
    t = trf["t"]
    env = unary_union([ShPolygon(p["poly"]) for p in d["poses"]
                       if t - win <= p["t"] <= t + 1e-6])
    worst = 0.0
    for l, r, w in zip(trf["L"], trf["R"], trf["W"]):
        if w <= 0.002:
            continue
        for a, b in ((l, r),):
            for u in np.linspace(0, 1, 9):
                x = a[0] + (b[0] - a[0]) * u
                y = a[1] + (b[1] - a[1]) * u
                worst = max(worst, env.distance(Point(x, y)))
    return worst


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else "/tmp/trail_dump.json"
    outgif = sys.argv[2] if len(sys.argv) > 2 else \
        "/home/ys/feedback/uidreczz28dReW3kMZs/artifacts/overlay_swing.gif"
    d = json.load(open(dump, encoding="utf-8"))
    lims = R.global_limits(d)
    ppu = R.compute_ppu(lims, figsize=(7.6, 7.2), dpi=110)
    win = d["params"]["max_age"] * R.LIFE_MAX + 2 * d["params"]["sim_dt"]

    ks = list(range(0, len(d["track"]), 5))
    if ks[-1] != len(d["track"]) - 1:
        ks.append(len(d["track"]) - 1)
    images = []
    for gi, k in enumerate(ks):
        trf = d["track"][k]
        t = trf["t"]
        fr_like = {"blade": d["poses"][k]["poly"],
                   "grip": trf["grip"], "guard": trf["guard"]}
        ds = frame_distance(d, k, win)
        ok = ds <= R.D_GATE
        fig, ax = plt.subplots(figsize=(7.6, 7.2), dpi=110)
        ax.set_xlim(lims[0], lims[1])
        ax.set_ylim(lims[2], lims[3])
        ax.set_aspect("equal")
        ax.set_xticks([]); ax.set_yticks([])
        R.draw_ghosts(ax, d, t, win, stride=4)
        draw_track_ribbon(ax, trf)
        R.draw_blade(ax, fr_like)
        ax.set_title(f"弯刀拖尾全流程  t={t:.2f}s  "
                     f"超出扫掠轮廓 {ds*ppu:.1f}px "
                     f"[{'PASS' if ok else 'FAIL'}]", fontsize=12,
                     color="#166534" if ok else "#991b1b")
        fig.canvas.draw()
        buf = np.asarray(fig.canvas.buffer_rgba())[:, :, :3]
        images.append(Image.fromarray(buf).convert(
            "P", palette=Image.ADAPTIVE, colors=128))
        plt.close(fig)
        print(f"gif frame {gi+1}/{len(ks)} t={t:.2f} d={ds*ppu:.1f}px")

    images[0].save(outgif, save_all=True, append_images=images[1:],
                  duration=110, loop=0, optimize=True, disposal=2)
    print("wrote", outgif, f"({len(images)} frames)")


if __name__ == "__main__":
    main()
