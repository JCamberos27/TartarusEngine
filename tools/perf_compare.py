#!/usr/bin/env python3
"""Compares two --perf-bench --perf-csv runs (docs/JOB_SYSTEM.md, Phase 0).

    python tools/perf_compare.py <before.csv> <after.csv> [--scopes N] [--min-ms X]

For every scene and phase (edit, play, play-max, combat) in both files: the frame time (average, p50, p95, p99), then
the CPU and GPU scopes with the largest change, before and after, in milliseconds and percent. A file that holds
several runs of the same scene and phase (the CSV is appended to) is averaged per row.

Typical use, on the machine being measured:

    TartarusEngine.exe --perf-bench --perf-csv inline.csv --jobs 0
    TartarusEngine.exe --perf-bench --perf-csv jobs.csv
    python tools/perf_compare.py inline.csv jobs.csv
"""

import argparse
import csv
import sys
from collections import defaultdict


def load(path):
    """{(scene, phase): {(kind, name): avg_ms}} averaged over repeated runs, plus the job settings seen."""
    sums = defaultdict(lambda: defaultdict(lambda: [0.0, 0]))
    jobs = set()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            cell = sums[(row["scene"], row["phase"])][(row["kind"], row["name"])]
            cell[0] += float(row["avg_ms"])
            cell[1] += 1
            jobs.add(row.get("jobs", "?"))
    out = {k: {n: s / c for n, (s, c) in v.items()} for k, v in sums.items()}
    return out, ", ".join(sorted(jobs))


def pct(before, after):
    return (after - before) / before * 100.0 if before > 0 else float("inf") if after > 0 else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--scopes", type=int, default=12, help="CPU / GPU scopes to list per phase (default 12)")
    ap.add_argument("--min-ms", type=float, default=0.05, help="ignore scopes under this in both runs (default 0.05)")
    args = ap.parse_args()

    before, before_jobs = load(args.before)
    after, after_jobs = load(args.after)
    print(f"before: {args.before} (jobs {before_jobs})")
    print(f"after:  {args.after} (jobs {after_jobs})")
    common = sorted(set(before) & set(after))
    if not common:
        print("perf_compare: the two files share no scene and phase")
        return 1
    for key in sorted(set(before) ^ set(after)):
        print(f"(only in {'before' if key in before else 'after'}: {key[0]} {key[1]})")

    for scene, phase in common:
        b, a = before[(scene, phase)], after[(scene, phase)]
        print(f"\n== {scene} ({phase})")
        for stat in ("avg", "p50", "p95", "p99"):
            k = ("frame", stat)
            if k in b and k in a:
                print(f"  frame {stat:<4} {b[k]:9.3f} -> {a[k]:9.3f} ms  ({pct(b[k], a[k]):+6.1f}%)")
        for kind in ("cpu", "gpu"):
            names = {n for (kd, n) in set(b) | set(a) if kd == kind}
            rows = []
            for n in names:
                bv, av = b.get((kind, n), 0.0), a.get((kind, n), 0.0)
                if max(bv, av) >= args.min_ms:
                    rows.append((abs(av - bv), n, bv, av))
            rows.sort(reverse=True)
            if rows:
                print(f"  {kind.upper()} scopes, largest change first:")
            for _, n, bv, av in rows[:args.scopes]:
                print(f"    {n:<36} {bv:9.3f} -> {av:9.3f} ms  ({av - bv:+8.3f} ms, {pct(bv, av):+6.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
