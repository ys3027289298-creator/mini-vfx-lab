#!/usr/bin/env python3
"""逐帧比对两份回放帧日志，报告首个分叉帧及该帧的具体差异。

用法: python3 -I compare_frames.py a.log b.log
帧日志由 replay 程序产出：每个 frame 一行状态头，后跟若干 slot 行。
"""
import sys


def parse(path):
    frames = {}
    cur = None
    with open(path, "r", encoding="utf-8") as fp:
        for raw in fp:
            line = raw.rstrip("\n")
            tok = line.split()
            if not tok:
                continue
            if tok[0] == "frame":
                cur = int(tok[1])
                frames[cur] = {"header": line, "slots": {}}
            elif tok[0] == "slot":
                idx = int(tok[1])
                frames[cur]["slots"][idx] = line
    return frames


def main(argv):
    if len(argv) != 3:
        print("usage: compare_frames.py <a.log> <b.log>", file=sys.stderr)
        return 2
    a, b = parse(argv[1]), parse(argv[2])
    keys = sorted(set(a) & set(b))
    only_a = sorted(set(a) - set(b))
    only_b = sorted(set(b) - set(a))
    if only_a or only_b:
        print(f"note: frame set differs (ignored for compare); "
              f"only_in_a={only_a[:8]}{'...' if len(only_a)>8 else ''} "
              f"only_in_b={only_b[:8]}{'...' if len(only_b)>8 else ''}")
    if not keys:
        print("no overlapping frames to compare", file=sys.stderr)
        return 2

    first = None
    diffs_total = 0
    for f in keys:
        if f not in a or f not in b:
            print(f"frame {f}: missing in one log")
            first = first if first is not None else f
            continue
        fa, fb = a[f], b[f]
        local = []
        if fa["header"] != fb["header"]:
            local.append(("header", fa["header"], fb["header"]))
        sa, sb = fa["slots"], fb["slots"]
        for i in sorted(set(sa) | set(sb)):
            if sa.get(i) != sb.get(i):
                local.append((f"slot {i}", sa.get(i, "<absent>"),
                              sb.get(i, "<absent>")))
        if local:
            diffs_total += 1
            if first is None:
                first = f
                print(f"FIRST DIVERGENCE at frame {f}: {len(local)} differing item(s)")
                for kind, x, y in local[:12]:
                    print(f"  [{kind}]\n    A: {x}\n    B: {y}")
                if len(local) > 12:
                    print(f"  ... and {len(local)-12} more")
    if first is None:
        print(f"IDENTICAL across {len(keys)} frames")
        return 0
    print(f"summary: first divergence frame {first}, "
          f"{diffs_total}/{len(keys)} frames differ")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
