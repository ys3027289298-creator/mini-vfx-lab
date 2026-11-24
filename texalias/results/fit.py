#!/usr/bin/env python3 -I
"""Scaling laws and texture sizing from results/edge_error.csv.

Two regimes, verified from the sweep:
  minification / near-unity (r <= 1): e_px ~ C(theta)/r, i.e. the texel-space
    amplitude A = e*r is constant, giving N_min(1 px) = C(theta) * S.
  magnification (r > 1): bilinear error vanishes at texel nodes, so the screen
    error settles to a sub-pixel phase floor independent of r; larger textures
    buy nothing once that floor is below the threshold.

The C check uses the r=1 operating point directly: C(theta) = e(r=1).
"""
import csv
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
rows = []
with open(os.path.join(HERE, "edge_error.csv")) as f:
    for r in csv.DictReader(f):
        rows.append({
            "probe": r["probe"], "kind": r["kind"],
            "ang": float(r["angle_deg"]), "S": int(r["S"]),
            "N": int(r["N"]), "r": float(r["r"]),
            "emax": float(r["edge_max_px"]),
            "erms": float(r["edge_rms_px"]),
        })

sweep = [r for r in rows if r["kind"] == "sweep"]
S = 256.0
ANGS = (0.0, 5.0, 30.0, 45.0)
PROBES = ("area", "hardsmooth", "hard", "line")


def get(probe, ang, r):
    m = [x for x in sweep if x["probe"] == probe and x["ang"] == ang
         and abs(x["r"] - r) < 1e-9]
    return m[0] if m else None


def fit_power(probe, ang, metric, rlo, rhi):
    pts = [(math.log(x["r"]), math.log(x[metric]))
           for x in sweep
           if x["probe"] == probe and x["ang"] == ang
           and rlo <= x["r"] <= rhi and x[metric] > 1e-6]
    n = len(pts)
    if n < 2:
        return None
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    p = sum((a - mx) * (b - my) for a, b in pts) / sxx
    C = math.exp(my - p * mx)
    pred = [C * math.exp(a) ** p for a, _ in pts]
    got = [math.exp(b) for _, b in pts]
    spread = max(g / q for g, q in zip(got, pred))
    return C, p, spread


print("== exponent p in e = C r^p ==")
print(f"{'probe':10} {'ang':>4} {'p (r<=1)':>10} {'p (r>=1)':>10}")
for probe in ("area", "hardsmooth"):
    for ang in ANGS:
        lo = fit_power(probe, ang, "emax", 0.125, 1.0)
        hi = fit_power(probe, ang, "emax", 1.0, 2.0)
        plo = f"{lo[1]:10.3f}" if lo else f"{'-':>10}"
        phi = f"{hi[1]:10.3f}" if hi else f"{'-':>10}"
        print(f"{probe:10} {ang:4.0f} {plo} {phi}")

print("\n== texel-space amplitude A = emax*r (constancy check, r<=1) ==")
print(f"{'probe':10} {'ang':>4} " +
      " ".join(f"r={r:5.3f}" for r in (0.125, 0.25, 0.5, 0.75, 1.0)))
for probe in ("area", "hardsmooth"):
    for ang in ANGS:
        vals = []
        for rr in (0.125, 0.25, 0.5, 0.75, 1.0):
            x = get(probe, ang, rr)
            vals.append(x["emax"] * rr if x else float("nan"))
        print(f"{probe:10} {ang:4.0f} " + " ".join(f"{v:8.4f}" for v in vals))

print("\n== magnification phase floor (mean emax over r>=1) ==")
for probe in ("area", "hardsmooth"):
    for ang in (5.0, 30.0, 45.0):
        xs = [x["emax"] for x in sweep
              if x["probe"] == probe and x["ang"] == ang and x["r"] >= 1.0]
        print(f"{probe:10} {ang:4.0f} floor={sum(xs)/len(xs):.4f} px")

print("\n== sizing coefficient C(theta) = emax(r=1), N_min = C*S ==")
print(f"{'probe':10} {'ang':>4} {'C_emax':>8} {'C_rms':>8} {'N_min(emax)':>12}")
for probe in ("area", "hardsmooth"):
    for ang in ANGS:
        x1 = get(probe, ang, 1.0)
        nmin = None
        for rr in sorted({x["r"] for x in sweep}):
            x = get(probe, ang, rr)
            if x and x["emax"] <= 1.0:
                nmin = x["N"]
                break
        print(f"{probe:10} {ang:4.0f} {x1['emax']:8.4f} {x1['erms']:8.4f} "
              f"{str(nmin):>12}")

print("\n== model prediction of N_min for arbitrary S (area, worst 30 deg) ==")
C30 = get("area", 30.0, 1.0)["emax"]
for s in (64, 128, 256, 512, 1024):
    print(f"S={s:5d}  N_min={C30*s:7.1f}  -> round {math.ceil(C30*s)}")
