#!/usr/bin/env python3
"""Independent measurement over the CSV trajectories recorded by ./pause.

Outputs:
  * jump measured across the pause/resume boundary for every policy variant
  * additive ablation split of the jump excess:
        total = time_compensation + emission_reissue + interaction
  * exact vector split of one recorded centroid jump (normal / time / emission)
  * spatial backlog deficit of the one-frame emission flush vs the schedule
  * acceptance checks (continuity + no emission flood + count conservation)
  * metrics.json next to the CSVs
Stdlib only. Exit code 1 if any acceptance check fails.
"""
import csv
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REC = ROOT / "recordings"

H = 1.0 / 60.0
PAUSE = 1.0
SPEED = 2.0
PRE_FRAME = 30
RESUME_FRAME = 91
PAY_CAP = 1
RATE = 60.0

MODES = ["buggy_full", "time_only", "emit_only", "fixed", "scheduled"]


def load_traj(mode):
    """id -> list of (frame, x, y) sorted by frame."""
    tracks = {}
    with open(REC / f"traj_{mode}.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            tracks.setdefault(int(row["id"]), []).append(
                (int(row["frame"]), float(row["x"]), float(row["y"]))
            )
    for seq in tracks.values():
        seq.sort()
    return tracks


def load_frames(mode):
    float_cols = ("wall_t", "motion_dt", "emit_dt", "debt_span", "step_max_move")
    rows = {}
    with open(REC / f"frames_{mode}.csv", newline="") as fh:
        for row in csv.DictReader(fh):
            rows[int(row["frame"])] = {
                k: (float(v) if k in float_cols else int(v))
                for k, v in row.items()
                if k not in ("mode",)
            }
    return rows


N_TAGGED = 12  # cohort recorded on every frame; snapshots add sparse extras


def jump_profile(tracks):
    """Return normal-max, boundary-jump, all-max for the TAGGED tracks only.
    (Sparse full-population snapshots must not be paired as frame steps.)"""
    normal_max = 0.0
    all_max = 0.0
    boundary = 0.0
    boundary_pairs = []
    for pid, seq in tracks.items():
        if pid >= N_TAGGED:
            continue
        for (f0, x0, y0), (f1, x1, y1) in zip(seq, seq[1:]):
            d = math.hypot(x1 - x0, y1 - y0)
            all_max = max(all_max, d)
            if f1 <= PRE_FRAME:
                normal_max = max(normal_max, d)
            if f0 < RESUME_FRAME <= f1:
                boundary = max(boundary, d)
                boundary_pairs.append((f0, f1, d, x0, y0, x1, y1))
    return normal_max, boundary, all_max, boundary_pairs


def positions_at(tracks, frame):
    out = {}
    for pid, seq in tracks.items():
        for f, x, y in seq:
            if f == frame:
                out[pid] = (x, y)
    return out


def centroid(pts):
    n = len(pts)
    return (sum(x for x, _ in pts) / n, sum(y for _, y in pts) / n)


def vadd(a, b):
    return (a[0] + b[0], a[1] + b[1])


def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1])


def vmul(k, a):
    return (k * a[0], k * a[1])


def vlen(a):
    return math.hypot(a[0], a[1])


checks = {}
metrics = {"modes": {}}


def check(name, ok, detail=""):
    checks[name] = bool(ok)
    print(f"{name}={'1' if ok else '0'} {detail}".rstrip())


print("== 1. jump measured from recorded trajectories (CSV) ==")
profiles = {}
for m in MODES:
    tr = load_traj(m)
    nmax, bj, amax, pairs = jump_profile(tr)
    profiles[m] = (nmax, bj, amax, pairs)
    fr = load_frames(m)
    max_spawn = max(r["spawned"] for r in fr.values())
    resume_spawn = fr[RESUME_FRAME]["spawned"]
    born_end = max(r["born"] for r in fr.values())
    debt_end = sorted(fr.values(), key=lambda r: r["frame"])[-1]["debt"]
    metrics["modes"][m] = {
        "normal_max_jump": nmax,
        "boundary_jump": bj,
        "all_max_jump": amax,
        "resume_spawned": resume_spawn,
        "max_frame_spawned": max_spawn,
        "born_end": born_end,
        "debt_end": debt_end,
    }
    print(f"{m:12s} normal={nmax:.6f} boundary={bj:.6f} all_max={amax:.6f} "
          f"resume_spawn={resume_spawn} max_spawn/frame={max_spawn} born={born_end}")

j0 = profiles["buggy_full"][0]
j_bug = profiles["buggy_full"][1]
j_time = profiles["time_only"][1]
j_emit = profiles["emit_only"][1]
j_fix = profiles["fixed"][1]

print("\n== 2. ablation split of the boundary jump excess over one normal frame ==")
total = j_bug - j0
t_part = j_time - j0
e_part = j_emit - j0
if abs(e_part) < 5e-6:  # CSV 6-decimal rounding noise around zero
    e_part = 0.0
inter = total - t_part - e_part
print(f"normal one-frame displacement J0 = {j0:.6f}")
print(f"buggy boundary jump Jbug         = {j_bug:.6f}")
print(f"excess total X = Jbug-J0         = {total:.6f}")
print(f"  time compensation Xt            = {t_part:.6f}  ({100*t_part/total:.2f}% of X)")
print(f"  emission reissue   Xe           = {e_part:.6f}  ({100*e_part/total:.2f}% of X)")
print(f"  interaction residual            = {inter:.9f}")
metrics["ablation"] = {
    "J0": j0, "excess_total": total, "time_part": t_part,
    "emit_part_same_id": e_part, "interaction": inter,
    "time_share_pct": 100 * t_part / total,
    "emit_share_pct": 100 * e_part / total,
}

print("\n== 3. exact vector split of the recorded centroid jump (buggy_full) ==")
trb = load_traj("buggy_full")
p0 = positions_at(trb, PRE_FRAME)
p1 = positions_at(trb, RESUME_FRAME)
old_ids = [i for i in p0 if i in p1]
N = len(old_ids)
K = len(p1) - N
C0 = centroid([p0[i] for i in old_ids])
C1_meas = centroid(list(p1.values()))
E = (0.0, 0.0)
dH = (SPEED * H, 0.0)
dP = (SPEED * PAUSE, 0.0)
Kn = 1
Kd = int(round(RATE * PAUSE))
# telescoping path: normal frame -> time overshoot of old set -> debt births
Cn = vmul(1.0 / (N + Kn), vadd(vmul(N, vadd(C0, dH)), E))
part_normal = vsub(Cn, C0)
Ct = vadd(Cn, vmul(N / (N + Kn), dP))
part_time = vsub(Ct, Cn)
Cf = vmul(1.0 / (N + Kn + Kd),
          vadd(vmul(N + Kn, Ct), vmul(Kd, E)))
part_emit = vsub(Cf, Ct)
part_total = vsub(Cf, C0)
meas_total = vsub(C1_meas, C0)
gross = vlen(part_time) + vlen(part_emit)
print(f"N_old={N} K_resume={K} (normal {Kn} + pause debt {Kd})")
print(f"C_pre={C0}  C_resume_measured={C1_meas}")
print(f"normal-frame vector   = ({part_normal[0]:+.6f},{part_normal[1]:+.6f}) |.|={vlen(part_normal):.6f}")
print(f"time-comp vector      = ({part_time[0]:+.6f},{part_time[1]:+.6f}) |.|={vlen(part_time):.6f}")
print(f"emission-debt vector  = ({part_emit[0]:+.6f},{part_emit[1]:+.6f}) |.|={vlen(part_emit):.6f}")
print(f"predicted total       = ({part_total[0]:+.6f},{part_total[1]:+.6f})")
print(f"measured total        = ({meas_total[0]:+.6f},{meas_total[1]:+.6f})")
print(f"gross pause budget    = {gross:.6f}; "
      f"time share {100*vlen(part_time)/gross:.1f}% / emit share {100*vlen(part_emit)/gross:.1f}% "
      f"(vectors oppose, so net centroid jump is {vlen(meas_total):.6f})")
metrics["centroid_split"] = {
    "N_old": N, "K_resume": K, "K_debt": Kd,
    "part_normal": part_normal, "part_time": part_time, "part_emit": part_emit,
    "predicted_total": part_total, "measured_total": meas_total,
    "gross_pause_budget": gross,
}

print("\n== 4. spatial deficit of the one-frame flush vs the no-pause schedule ==")
trs = load_traj("scheduled")
ps = positions_at(trs, RESUME_FRAME)
# All flush births first appear at the resume frame; spawn order makes their
# ids map 1:1 onto the scheduled birth slots. The first Kd new ids are the
# particles owed by the pause interval.
new_ids = sorted(pid for pid in p1 if pid not in p0)
debt_ids = new_ids[:Kd]
debt_dist = [math.dist(p1[i], ps[i]) for i in debt_ids if i in ps]
dmax = max(debt_dist)
dmean = sum(debt_dist) / len(debt_dist)
print(f"pause-debt cohort n={len(debt_dist)} "
      f"flush-vs-scheduled position gap: max={dmax:.6f} mean={dmean:.6f}")
print(f"(oldest missed emission should be ~speed*pause={SPEED*PAUSE:.3f}; "
      f"it is erased to age 0 by the flush)")
metrics["emission_deficit"] = {"n": len(debt_dist), "max": dmax, "mean": dmean}

print("\n== 5. trajectory excerpt, tagged particle id 0 ==")
for m in ("buggy_full", "fixed"):
    seq = dict((f, (x, y)) for f, x, y in load_traj(m)[0])
    cols = [29, 30, 91, 92, 93]
    print(m, " ".join(f"f{f}:({seq[f][0]:.4f},{seq[f][1]:.4f})" for f in cols if f in seq))

print("\n== 6. acceptance checks (independent of the C assertions) ==")
check("csv_baseline_teleport_gt_1u", j_bug > 1.0, f"({j_bug:.4f})")
check("csv_decomp_additive", abs(inter) < 1e-4, f"(resid {inter:.2e})")
check("csv_time_owns_position_jump", abs(t_part - total) < 1e-4)
check("csv_emit_sameid_jump_zero", j_emit <= j0 * 1.001 + 1e-6, f"({j_emit:.6f})")
check("csv_fixed_boundary_le_normal", j_fix <= j0 * 1.001 + 1e-6,
      f"(fixed {j_fix:.6f} vs normal {j0:.6f})")
check("csv_fixed_allmax_le_normal",
      profiles["fixed"][2] <= j0 * 1.001 + 1e-6,
      f"({profiles['fixed'][2]:.6f})")
# independent ALL-particle check from the per-frame max-move column
fstep_max = max(r["step_max_move"] for r in load_frames("fixed").values())
bstep_max = max(r["step_max_move"] for r in load_frames("buggy_full").values())
check("csv_fixed_every_particle_step_le_normal",
      fstep_max <= j0 * 1.001 + 1e-6, f"({fstep_max:.6f})")
check("csv_buggy_every_particle_step_was_huge", bstep_max > 1.0,
      f"({bstep_max:.4f})")
ffix = load_frames("fixed")
check("csv_fixed_resume_spawn_is_2", ffix[RESUME_FRAME]["spawned"] == 1 + PAY_CAP)
check("csv_no_frame_flood_over_2",
      max(r["spawned"] for r in ffix.values()) <= 1 + PAY_CAP)
check("csv_buggy_did_flood_61",
      load_frames("buggy_full")[RESUME_FRAME]["spawned"] == 61)
check("csv_emission_count_conserved",
      metrics["modes"]["fixed"]["born_end"] == metrics["modes"]["scheduled"]["born_end"],
      f"({metrics['modes']['fixed']['born_end']} vs "
      f"{metrics['modes']['scheduled']['born_end']})")
check("csv_debt_repaid", metrics["modes"]["fixed"]["debt_end"] == 0)
check("csv_centroid_split_matches_recording",
      math.dist(part_total, meas_total) < 1e-4)
check("csv_emit_deficit_max_about_G", abs(dmax - SPEED * PAUSE) < 0.04,
      f"(dmax {dmax:.4f})")

all_ok = all(checks.values())
metrics["checks"] = checks
metrics["py_all_ok"] = all_ok
with open(REC / "metrics.json", "w") as fh:
    json.dump(metrics, fh, indent=2)
print(f"\npy_all_ok={1 if all_ok else 0}")
print(f"wrote {REC / 'metrics.json'}")
sys.exit(0 if all_ok else 1)
