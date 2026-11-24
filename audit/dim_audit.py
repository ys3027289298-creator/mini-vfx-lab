#!/usr/bin/env python3
"""Dimensional-consistency audit for the parameter -> displacement chain.

Method
------
1. Every link of the chain (frame clock, emitter, field integrator, trail)
   declares a time-unit contract in CHAIN and exposes a `probe-*` command in
   the sim binary that measures the link in isolation against its analytic
   expectation.
2. The end-to-end criterion is the free-fall contract: at 60 fps, 60 frames,
   g=9.8 m/s^2, zero initial velocity, the displacement must satisfy
       |d - 4.9 m| / 4.9 m <= TOL_REL   (TOL_REL = 0.0102, ~5 cm)
   i.e. d in [4.85, 4.95] m. The same criterion gates every link.
3. Error attribution:
   a. each link's isolated probe contributes its own relative error;
   b. single-link perturbations (SIM_DT_SCALE_* = 0.1 and 10) measure the
      end-to-end sensitivity to that link's time unit;
   c. the clock constant SIM_MS_TO_SEC is swept over replacement candidates
      (1, 1e-2, 1e-3, 1e-4, 1e-6) -- a constant whose correct value flips the
      verdict is ranked by the error it removes.
Suspects are printed sorted by error contribution, largest first.

Run:  python3 -I dim_audit.py /path/to/sim [--fps 60] [--frames 60]
Exit code 0 only when the end-to-end contract passes.
"""
import argparse
import os
import re
import subprocess
import sys

TOL_REL = 0.0102          # 4.85..4.95 m around 4.9 m
ANALYTIC_1S_M = 4.9
CLOCK_CANDIDATES = ("1", "0.01", "0.001", "0.0001", "0.000001")

# (step, kind, input time unit, output time unit, probe argv, measured key,
#  analytic expectation, source location of the time-unit boundary)
# kind: param = declared physical parameter; link = dt-carrying code step.
CHAIN = [
    ("gravity param g",  "param", "m/s^2 (declared)", "m/s^2",
     None, None, 9.8, "caller: fall <g>; consumed as g*dt in field"),
    ("drag param",       "param", "1/s (declared)",   "1/s",
     None, None, 0.0, "field/src/field.c: expf(-drag*dt)"),
    ("frame clock",      "link",  "ms tick",          "s (dt)",
     ["probe-clock", "60", "60"], "total_sim_time_s", 1.0,
     "sim/src/sim.c: SIM_MS_TO_SEC"),
    ("emitter update",   "link",  "s (dt)",           "s (age); m (x=v*dt)",
     ["probe-emitter"], "age_s", 1.0,
     "emitter/src/emitter.c: age += dt; x += vx*dt"),
    ("field integrator", "link",  "s (dt)",           "m (x=v*dt+0.5*a*dt^2)",
     ["probe-field", "verlet"], "displacement_m", 4.9,
     "field/src/field.c: step_verlet"),
    ("trail aging",      "link",  "s (dt)",           "s (point age)",
     ["probe-trail"], "age_s", 0.5,
     "trail/src/trail.c: age += dt"),
]
STAGE_ENV = {"emitter update":   "SIM_DT_SCALE_EMITTER",
             "field integrator": "SIM_DT_SCALE_FIELD",
             "trail aging":      "SIM_DT_SCALE_TRAIL"}


def run(sim, args, env_extra=None):
    env = dict(os.environ)
    # strip sim-internal knobs so probes are reproducible
    for v in set(STAGE_ENV.values()) | {"SIM_MS_TO_SEC"}:
        env.pop(v, None)
    if env_extra:
        env.update(env_extra)
    out = subprocess.run([sim, *args], capture_output=True, text=True,
                         env=env, check=True).stdout
    return dict(re.findall(r"^([a-z_]+)=(.*)$", out, re.M))


def relerr(measured, expected):
    return abs(float(measured) - expected) / abs(expected)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sim")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--frames", type=int, default=60)
    args = ap.parse_args()

    print("=== dimensional audit: time-unit chain ===")
    print(f"criterion: free fall {args.frames} frames @ {args.fps}fps, "
          f"g=9.8 -> d=4.9m, relerr <= {TOL_REL} (d in [4.85, 4.95] m)\n")

    print(f"{'step':<19}{'kind':<7}{'time unit in':<19}{'time unit out':<24}"
          f"contract check")
    link_errors = {}
    for name, kind, unit_in, unit_out, pargv, key, expected, loc in CHAIN:
        if pargv is None:
            print(f"{name:<19}{kind:<7}{unit_in:<19}{unit_out:<24}"
                  f"declared parameter (no dt boundary)  [{loc}]")
            continue
        kv = run(args.sim, pargv)
        e = relerr(kv[key], expected)
        link_errors[name] = e
        verdict = "OK  " if e <= TOL_REL else "FAIL"
        print(f"{name:<19}{kind:<7}{unit_in:<19}{unit_out:<24}"
              f"{verdict} relerr={e:.3e}  [{loc}]")

    print("\n=== end-to-end free fall through the whole chain ===")
    e2e = run(args.sim, ["fall", str(args.fps), str(args.frames), "9.8", "verlet"])
    e2e_err = relerr(e2e["displacement_m"], ANALYTIC_1S_M)
    print(f"sim_time_s={e2e['sim_time_s']}  clock_dt_s={e2e['clock_dt_s']}  "
          f"d={e2e['displacement_m']} m  relerr={e2e_err:.3e}  "
          f"integrator={e2e['integrator']}")

    print("\n=== attribution experiment A: per-link dt perturbations ===")
    print("scale one link's dt x0.1 / x10; a large end-to-end shift means the")
    print("link is on the causal path of displacement, not merely adjacent.\n")
    sensitivity = {}
    for name, evar in STAGE_ENV.items():
        deltas = []
        for scale in ("0.1", "10"):
            kv = run(args.sim,
                     ["fall", str(args.fps), str(args.frames), "9.8", "verlet"],
                     {evar: scale})
            deltas.append(abs(relerr(kv["displacement_m"], ANALYTIC_1S_M) - e2e_err))
        sensitivity[name] = max(deltas)
        print(f"{name:<19} max relerr shift = {sensitivity[name]:.3e}")

    print("\n=== attribution experiment B: clock-constant sweep ===")
    print("replace SIM_MS_TO_SEC with each candidate; error removed ranks how")
    print("much of the end-to-end error that single constant explains.\n")
    clock_suspects = []
    for c in CLOCK_CANDIDATES:
        kv = run(args.sim,
                 ["fall", str(args.fps), str(args.frames), "9.8", "verlet"],
                 {"SIM_MS_TO_SEC": c})
        e = relerr(kv["displacement_m"], ANALYTIC_1S_M)
        removes = max(e2e_err - e, 0.0)
        clock_suspects.append((removes, c, e))
        print(f"candidate {c:<8} -> relerr={e:.3e}  error removed={removes:.3e}")

    # ---- ranked suspect list ------------------------------------------------
    # Two independent signals, ranked on the same [0,1] error-contribution
    # scale: (A) isolated link-contract breaches, (B) constants whose change
    # removes the end-to-end error. The largest contributor is fixed first.
    suspects = []
    for name, e in link_errors.items():
        if e > TOL_REL:
            loc = next(item[7] for item in CHAIN if item[0] == name)
            suspects.append((e, f"[A] {name} breaks its seconds contract: {loc}"))
    for removes, c, e in clock_suspects:
        if removes > 0 and e <= TOL_REL:
            suspects.append((removes,
                             f"[B] set SIM_MS_TO_SEC={c} (ms->s); restores the "
                             f"end-to-end contract, relerr {e:.3e}"))
    suspects.sort(reverse=True)

    print("\n=== suspects ranked by error contribution (fix top first) ===")
    if not suspects:
        print("none: every link and the end-to-end chain satisfy the contract")
    for rank, (contrib, msg) in enumerate(suspects, 1):
        print(f"{rank}. contribution={contrib:.3e}  {msg}")

    print("\n=== verdict ===")
    ok = e2e_err <= TOL_REL
    print(f"end-to-end relerr={e2e_err:.3e} threshold={TOL_REL} "
          f"({'PASS' if ok else 'FAIL'})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
