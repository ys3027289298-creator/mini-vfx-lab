#!/usr/bin/env python3
"""Verify the boundary fix against the full 500-frame logs.

Assertions (the task's acceptance criteria, checked every frame not just
at the end):
  NONE    : the bug — out-of-box-alive count grows and ends at 80.4%
  KILL    : out-of-box-alive == 0 in every frame; particles only die,
            never exceed the box coordinate
  REFLECT : out-of-box-alive == 0 in every frame; max coordinate <= box
  FADE    : out-of-box-alive == 0 in every frame; alpha changes smoothly
            between consecutive frames (no flicker), particles die only
            at the hard edge
All four runs must share the same emission schedule (same seed/params).
Writes artifacts/boundary/verification.txt; exits non-zero on any failure.
"""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.path.join(ROOT, "artifacts", "boundary", "data")
OUT = os.path.join(ROOT, "artifacts", "boundary", "verification.txt")

MODES = ("none", "kill", "reflect", "fade")
BOX = 4.0
# Largest per-frame alpha change possible from pure motion in the fade band:
# max speed (~4.0) * dt (0.02) / margin (1.0) = 0.08, plus float slack.
ALPHA_JUMP_LIMIT = 0.11

results = []
def check(name, ok, detail=""):
    results.append((name, ok, detail))

def load_log(mode):
    with open(os.path.join(DATA, f"{mode}_log.csv")) as f:
        return list(csv.DictReader(f))

logs = {m: load_log(m) for m in MODES}

# --- shared determinism: identical alive count where the boundary has not
#     removed anything yet, and identical frame counts ----------------------
frame_counts = {m: len(r) for m, r in logs.items()}
check("all_runs_500_frames", set(frame_counts.values()) == {500},
      str(frame_counts))

# NONE and REFLECT never kill particles, so their alive counts must match
# frame by frame exactly (same spawns, same lifetimes, same RNG stream).
alive_none = [int(r["alive"]) for r in logs["none"]]
alive_refl = [int(r["alive"]) for r in logs["reflect"]]
check("none_vs_reflect_same_population", alive_none == alive_refl,
      f"final alive {alive_none[-1]} vs {alive_refl[-1]}")

# KILL and FADE apply the same hard edge, so their alive counts match too.
alive_kill = [int(r["alive"]) for r in logs["kill"]]
alive_fade = [int(r["alive"]) for r in logs["fade"]]
check("kill_vs_fade_same_population", alive_kill == alive_fade,
      f"final alive {alive_kill[-1]} vs {alive_fade[-1]}")

# --- NONE: reproduce the reported bug ---------------------------------------
none = logs["none"]
oob_none = [int(r["out_of_box"]) for r in none]
check("none_bug_present_final_share",
      oob_none[-1] == 201 and int(none[-1]["alive"]) == 250
      and abs(float(none[-1]["out_share"]) - 0.804) < 0.001,
      f"oob={oob_none[-1]}, share={none[-1]['out_share']}")
check("none_oob_monotonic_growth",
      all(b >= a for a, b in zip(oob_none, oob_none[1:])),
      "escaped particles never return")
check("none_drifts_far_away",
      max(max(float(r["max_x"]), float(r["max_y"])) for r in none) > 80.0,
      f"max |coord| = {max(max(float(r['max_x']), float(r['max_y'])) for r in none):.1f}")

# --- the three real strategies: zero out-of-box-alive in EVERY frame -------
for m in ("kill", "reflect", "fade"):
    rows = logs[m]
    worst_oob = max(int(r["out_of_box"]) for r in rows)
    final_oob = int(rows[-1]["out_of_box"])
    check(f"{m}_zero_oob_every_frame", worst_oob == 0 and final_oob == 0,
          f"max over all frames = {worst_oob}")
    worst_coord = max(max(float(r["max_x"]), float(r["max_y"])) for r in rows)
    check(f"{m}_never_past_box", worst_coord <= BOX + 1e-6,
          f"max |coord| = {worst_coord:.4f}")

# KILL and FADE actually destroy particles at the edge (201 each).
check("kill_destroys_201", int(logs["kill"][-1]["killed_cum"]) == 201)
check("fade_destroys_201", int(logs["fade"][-1]["killed_cum"]) == 201)
# Reflect destroys nothing and keeps the full population alive.
check("reflect_destroys_nothing",
      int(logs["reflect"][-1]["killed_cum"]) == 0
      and int(logs["reflect"][-1]["alive"]) == 250)
check("reflect_bounces_recorded", int(logs["reflect"][-1]["bounces_cum"]) > 0,
      logs["reflect"][-1]["bounces_cum"])

# --- flicker checks ----------------------------------------------------------
# KILL / REFLECT keep alpha constant at 1.0; FADE must stay smooth.
for m in ("kill", "reflect"):
    worst = max(float(r["max_abs_alpha_jump"]) for r in logs[m])
    check(f"{m}_alpha_constant", worst == 0.0, f"worst jump = {worst}")
worst_fade = max(float(r["max_abs_alpha_jump"]) for r in logs["fade"])
check("fade_alpha_smooth_no_flicker", worst_fade <= ALPHA_JUMP_LIMIT,
      f"worst jump = {worst_fade:.4f}, limit = {ALPHA_JUMP_LIMIT}")

# Final-frame FADE snapshot: every surviving particle inside the box, and
# alpha equals the position-derived ramp (no arbitrary per-frame values).
with open(os.path.join(DATA, "fade_final.csv")) as f:
    snap = list(csv.DictReader(f))
ramp_ok, inside_ok = True, True
for r in snap:
    x, y, a = float(r["x"]), float(r["y"]), float(r["alpha"])
    inside_ok &= (-BOX <= x <= BOX and -BOX <= y <= BOX)
    d = min(x + BOX, BOX - x, y + BOX, BOX - y)
    expect = max(0.0, min(1.0, d / 1.0))
    ramp_ok &= abs(a - expect) < 1e-4
check("fade_final_all_inside", inside_ok, f"{len(snap)} particles")
check("fade_alpha_position_derived", ramp_ok,
      "alpha = clamp(distance_to_nearest_wall / margin)")

# FADE particles seen inside the margin band at the final frame prove the
# fade-out is actually visible (not all-or-nothing).
in_band = [r for r in snap if float(r["alpha"]) < 1.0]
check("fade_band_occupied", len(in_band) > 0, f"{len(in_band)} fading particles")

# --- report ------------------------------------------------------------------
lines = []
w = max(len(n) for n, _, _ in results)
all_ok = True
for name, ok, detail in results:
    all_ok &= ok
    lines.append(f"[{'PASS' if ok else 'FAIL'}] {name.ljust(w)}  {detail}")
report = "\n".join(lines) + f"\n\nALL_CHECKS_PASSED={int(all_ok)}\n"
print(report)
with open(OUT, "w") as f:
    f.write(report)
sys.exit(0 if all_ok else 1)
