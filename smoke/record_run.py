#!/usr/bin/env python3
"""按 params JSON 调用 C 录制器 record。

用法: python3 record_run.py params.json out.csv
"""
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    params_path = Path(sys.argv[1])
    if not params_path.is_absolute():
        params_path = HERE / params_path
    out_csv = Path(sys.argv[2])
    if not out_csv.is_absolute():
        out_csv = HERE / out_csv
    out_csv.parent.mkdir(parents=True, exist_ok=True)

    cfg = json.loads(params_path.read_text(encoding="utf-8"))
    f = cfg["forces"]
    r = cfg["record"]
    e = cfg["emitter"]
    dt = 1.0 / float(r["fps"])

    argv = [
        str(HERE / "record"),
        str(out_csv),
        str(float(r["duration_s"])),
        repr(dt),
        str(int(r["seed"])),
        str(float(e["rate_per_s"])),
        str(float(e["life_s"])),
        str(float(e["spawn_radius"])),
        str(float(e["spawn_vx_sigma"])),
        str(float(e["spawn_vy0"])),
        str(float(e["spawn_vy_jitter"])),
        str(float(f["gravity_x"])),
        str(float(f["gravity_y"])),
        str(float(f["drag"])),
        str(float(f["swirl"])),
    ]
    print("+ " + " ".join(argv), file=sys.stderr)
    return subprocess.call(argv)


if __name__ == "__main__":
    raise SystemExit(main())
