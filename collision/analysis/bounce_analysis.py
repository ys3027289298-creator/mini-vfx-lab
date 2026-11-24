#!/usr/bin/env python3
"""Bounce-series closed forms, coefficient selection and numerical verification.

Runs under `python3 -I bounce_analysis.py` (stdlib only). Writes:
  - bounce_report.json   all numbers behind the report and charts
  - bounce_report.html   self-contained charts (N(e), T(e), speed decay) + table

Convention: y up, floor at y = 0, incidence angle theta measured from the
surface normal (theta = 0 is a vertical drop, theta -> 90 deg is grazing).
"""
import json
import math
import pathlib

G = 9.8                       # gravity magnitude
V0 = 10.0                     # impact speed at the first landing
RATIO = 0.01                  # required speed ratio at rest: v_s / V0
V_SLEEP = RATIO * V0          # sleep cutoff speed
DEADLINE = 1.0                # must be asleep within 1 s of landing
MU = 0.8                      # tangential velocity retained per impact
ANGLES = (15.0, 45.0, 75.0)   # three incidence-angle tiers (deg from normal)
DT = 1e-4                     # time-stepped cross-check step


# ---------------------------------------------------------------- closed forms
def impact_components(theta_deg):
    """Pre-impact (v_n, v_t) at impact #1 for speed V0."""
    a = math.radians(theta_deg)
    return V0 * math.cos(a), V0 * math.sin(a)


def pre_speed_k(k, v1n, v1t, e, mu):
    """Speed just before impact #k (k = 1 is the first landing)."""
    vn = v1n * e ** (k - 1)
    vt = v1t * mu ** (k - 1)
    return math.hypot(vn, vt), vn, vt


def arrival_time_k(k, v1n, e):
    """Time of impact #k relative to the first landing; t_1 = 0."""
    if k <= 1 or e == 0.0:
        return 0.0
    return (2.0 * v1n / G) * e * (1.0 - e ** (k - 1)) / (1.0 - e)


def zeno_time(v1n, e):
    """Limit of the bounce-series clock: T_inf = 2 e v1n / ((1-e) g)."""
    if e >= 1.0:
        return math.inf
    return 2.0 * e * v1n / ((1.0 - e) * G)


def closed_form_run(theta_deg, e, mu=MU):
    """Walk the closed form until the sleep cutoff; return event table + summary."""
    v1n, v1t = impact_components(theta_deg)
    events = []
    k = 1
    while True:
        w, vn, vt = pre_speed_k(k, v1n, v1t, e, mu)
        t = arrival_time_k(k, v1n, e)
        events.append(dict(k=k, t=t, speed=w, ratio=w / V0, vn=vn, vt=vt))
        if w <= V_SLEEP:
            return dict(angle=theta_deg, e=e, mu=mu, v1n=v1n, v1t=v1t,
                        events=events, bounces=k - 1, settle_time=t,
                        slept=True)
        if k > 100000:
            return dict(angle=theta_deg, e=e, mu=mu, v1n=v1n, v1t=v1t,
                        events=events, bounces=math.inf, settle_time=math.inf,
                        slept=False)
        k += 1


def sufficient_e_bound(v1n):
    """Zeno-based sufficient bound: T_inf <= DEADLINE  =>  e <= g/(g+2 v1n)."""
    return G / (G + 2.0 * v1n)


# ------------------------------------------------- time-stepped cross-check
def simulated_run(theta_deg, e, mu=MU, dt=DT):
    """Integrate the same rule the C module uses, launched so the first landing
    has pre-impact speed V0 at the requested angle."""
    v1n, v1t = impact_components(theta_deg)
    x = 0.0
    y = v1n * v1n / (2.0 * G)      # h = v^2/2g gives pre-impact vy = -v1n
    vx, vy = v1t, 0.0
    t = 0.0
    impacts = []
    trace = []                     # downsampled (t, speed ratio) for plotting
    sample = 0
    slept = False
    t0 = None                      # first landing: closed-form time origin
    while not slept:
        vy -= G * dt
        x += vx * dt
        y += vy * dt
        t += dt
        if y <= 0.0 and vy <= 0.0:
            if t0 is None:
                t0 = t
            impacts.append((t - t0, math.hypot(vx, vy)))
            if math.hypot(vx, vy) <= V_SLEEP:
                y, vx, vy = 0.0, 0.0, 0.0
                slept = True
                break
            vy = -e * vy
            vx *= mu
            y = 0.0
        if sample % 50 == 0:       # every 5 ms, relative to first landing
            trace.append((max(0.0, t - (t0 if t0 is not None else t)),
                          math.hypot(vx, vy) / V0))
        sample += 1
        if t > 60.0:
            break
    trace.append((max(0.0, t - t0), math.hypot(vx, vy) / V0))
    return impacts, trace


def cross_check(theta_deg, e):
    cf = closed_form_run(theta_deg, e)
    impacts, trace = simulated_run(theta_deg, e)
    worst_t = worst_w = 0.0
    for ev, (t_sim, w_sim) in zip(cf["events"], impacts):
        worst_t = max(worst_t, abs(ev["t"] - t_sim))
        worst_w = max(worst_w, abs(ev["speed"] - w_sim))
    return cf, impacts, trace, worst_t, worst_w


# ----------------------------------------------------------------- scanning
def scan_e(theta_deg, step=0.002):
    v1n, _ = impact_components(theta_deg)
    rows = []
    e = step
    while e < 1.0:
        r = closed_form_run(theta_deg, e)
        rows.append(dict(e=e, bounces=r["bounces"],
                         settle_time=r["settle_time"],
                         zeno=zeno_time(v1n, e)))
        e += step
    return rows


def max_feasible_e(theta_deg, step=0.0005):
    """Largest e whose discrete sleep event arrives within the deadline."""
    best = 0.0
    e = step
    while e < 1.0:
        r = closed_form_run(theta_deg, e)
        if r["slept"] and r["settle_time"] <= DEADLINE:
            best = e
        e += step
    return best


# --------------------------------------------------------------------- report
def main():
    out = dict(params=dict(g=G, v0=V0, v_sleep=V_SLEEP, ratio=RATIO,
                           deadline=DEADLINE, mu=MU, angles=ANGLES, dt=DT))

    # --- divergence / infeasibility facts ----------------------------------
    v1n_steep, _ = impact_components(ANGLES[0])
    out["bounds"] = {
        "sufficient_e_steep": sufficient_e_bound(v1n_steep),
        "zeno_e_0.9_steep": zeno_time(v1n_steep, 0.9),
        # tangent-only floor with mu = 1 never crosses the sleep cutoff
        "tangent_floor_mu1": [V0 * math.sin(math.radians(a)) for a in ANGLES],
    }

    # --- N(e), T(e) curves and coefficient choice --------------------------
    curves = {}
    for a in ANGLES:
        rows = scan_e(a)
        curves[f"{a:g}"] = rows
    out["curves"] = curves

    feasible = {f"{a:g}": max_feasible_e(a) for a in ANGLES}
    e_ceiling = min(feasible.values())       # hard ceiling: smallest per-angle e_max
    # engineering pick: stay below the ceiling so frame-rate jitter and float
    # error cannot push the steepest case past 1 s; rounded to two decimals
    e_pick = math.floor(e_ceiling * 100.0) / 100.0
    out["feasible_e_per_angle"] = feasible
    out["e_ceiling"] = e_ceiling
    out["chosen_e"] = e_pick
    e_star = e_pick

    # --- three-angle verification at the chosen coefficient ----------------
    verifs = []
    for a in ANGLES:
        cf, impacts, trace, wt, ww = cross_check(a, e_star)
        last = cf["events"][-1]
        verifs.append(dict(angle=a, bounces=cf["bounces"],
                           settle_time=cf["settle_time"],
                           final_ratio=last["ratio"],
                           meets_deadline=cf["settle_time"] <= DEADLINE,
                           meets_ratio=last["ratio"] <= RATIO,
                           sim_impacts=len(impacts),
                           sim_settle=impacts[-1][0],
                           time_err=wt, speed_err=ww,
                           trace=trace,
                           events=[dict(k=e_["k"], t=e_["t"],
                                        ratio=e_["ratio"])
                                   for e_ in cf["events"]]))
    out["verification"] = verifs

    # --- deliberately bad coefficient: which bounce finally stops it? ------
    bad_angle = ANGLES[0]
    bad = closed_form_run(bad_angle, 0.9)
    v1n, _ = impact_components(bad_angle)
    # speed ratio at the deadline: locate the flight containing t = 1 s
    ev = bad["events"]
    t_ratio = None
    for i in range(len(ev) - 1):
        if ev[i]["t"] <= DEADLINE < ev[i + 1]["t"]:
            # post-impact normal speed of bounce i, ballistic over the gap
            vn_up = 0.9 * ev[i]["vn"]
            dt_local = DEADLINE - ev[i]["t"]
            vn_now = vn_up - G * dt_local
            vt_now = 0.9 ** i * (V0 * math.sin(math.radians(bad_angle)))
            t_ratio = math.hypot(vn_now, vt_now) / V0
            break
    out["bad_case"] = dict(e=0.9, angle=bad_angle,
                           bounces_until_sleep=bad["bounces"],
                           settle_time=bad["settle_time"],
                           zeno=zeno_time(v1n, 0.9),
                           ratio_at_deadline=t_ratio)

    pathlib.Path(__file__).parent.joinpath("bounce_report.json").write_text(
        json.dumps(out, indent=2, ensure_ascii=False))

    # ---------------- console summary ----------------
    p = out["params"]
    print("== parameters ==")
    print(f"V0={p['v0']} g={p['g']} mu={p['mu']} v_sleep={p['v_sleep']:.2f}"
          f" ({p['ratio']:.0%} of V0) deadline={p['deadline']}s")
    print("\n== infeasibility ==")
    b = out["bounds"]
    print(f"Zeno sufficient e bound (15 deg, v1n={v1n_steep:.3f}): "
          f"e <= {b['sufficient_e_steep']:.4f}")
    print(f"Zeno time at e=0.9: {b['zeno_e_0.9_steep']:.2f} s (diverges as e->1)")
    floors = "[" + ", ".join(f"{x:.3f}" for x in b["tangent_floor_mu1"]) + "]"
    print(f"tangent floor with mu=1: {floors} all exceed v_sleep="
          f"{V_SLEEP:.2f} -> never sleeps")
    print("\n== feasible e (scan) ==")
    for k, v in feasible.items():
        print(f"theta={k:>3} deg: e_max={v:.4f}")
    print(f"hard ceiling e_max = {e_ceiling:.4f}; engineering pick e* = {e_pick:.2f}")
    print("\n== three-angle verification at e* ==")
    for v in verifs:
        print(f"theta={v['angle']:>3} deg: N={v['bounces']:>2} bounces, "
              f"T_settle={v['settle_time']:.4f}s, final ratio="
              f"{v['final_ratio']:.5f}, deadline={v['meets_deadline']}, "
              f"ratio={v['meets_ratio']}, sim N={v['sim_impacts']}, "
              f"sim T={v['sim_settle']:.4f}s, "
              f"err(dt={DT}) dT={v['time_err']:.2e} dW={v['speed_err']:.2e}")
    print("\n== bad case e=0.9 ==")
    bc = out["bad_case"]
    print(f"sleeps only after bounce #{bc['bounces_until_sleep']}, "
          f"T={bc['settle_time']:.2f}s (Zeno {bc['zeno']:.2f}s), "
          f"ratio at t=1s: {bc['ratio_at_deadline']:.4f} -> fails 1% rule")


if __name__ == "__main__":
    main()
