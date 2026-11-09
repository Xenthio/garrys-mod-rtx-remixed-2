#!/usr/bin/env python3
"""Per-phase acceleration structure cost of an openremix_bench run.

Usage: accel_phases.py <REMIX_ACCEL_PROFILE csv> <openremix_bench_phases.txt>

Lines the CSV's rows (wall-clock milliseconds) up with the bench phases and
prints, per phase: frames, CPU geometry conversion and BLAS/TLAS recording
times, BLAS builds and refits with their triangles, and the GPU build time
(the last read back: a few frames behind the rest).
"""
import csv
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rows = []
    with open(sys.argv[1], newline="") as f:
        for row in csv.DictReader(f):
            rows.append({k: float(v) for k, v in row.items()})
    phases = []
    with open(sys.argv[2]) as f:
        for line in f:
            parts = line.split(None, 3)
            if len(parts) == 4:
                phases.append((int(parts[0]), float(parts[1]), float(parts[2]), parts[3].strip()))
    if not phases:
        sys.exit("no phases (was the bench run with the wall-clock recorder?)")
    spans = phases + [(0, phases[0][1], phases[-1][2], "whole session")]

    def stat(values):
        if not values:
            return 0.0, 0.0, 0.0
        ordered = sorted(values)
        return sum(values) / len(values), ordered[min(len(ordered) - 1, int(0.95 * len(ordered)))], ordered[-1]

    print(f"{'phase':14} {'frames':>6} | {'geometry ms':>19} | {'BLAS record ms':>19} | {'TLAS ms':>13} | "
          f"{'builds':>6} {'tris':>9} | {'refits':>6} {'tris':>10} | {'GPU build ms':>19}")
    print(f"{'':14} {'':>6} | {'mean   p95    max':>19} | {'mean   p95    max':>19} | {'mean    max':>13} | "
          f"{'':>6} {'built':>9} | {'':>6} {'refitted':>10} | {'mean   p95    max':>19}")
    for _, start, end, name in spans:
        inside = [r for r in rows if start <= r["time_ms"] < end]
        if not inside:
            print(f"{name:14} {0:>6} | no profile rows in this span")
            continue
        g = stat([r["geometry_ms"] for r in inside])
        b = stat([r["bottom_ms"] for r in inside])
        t = stat([r["top_ms"] for r in inside])
        u = stat([r["gpu_build_ms"] for r in inside])
        builds = sum(r["blas_builds"] for r in inside)
        refits = sum(r["blas_refits"] for r in inside)
        built = sum(r["built_triangles"] for r in inside)
        refitted = sum(r["refit_triangles"] for r in inside)
        print(f"{name:14} {len(inside):>6} | {g[0]:5.2f} {g[1]:6.2f} {g[2]:6.2f} | {b[0]:5.2f} {b[1]:6.2f} {b[2]:6.2f} | "
              f"{t[0]:5.2f} {t[2]:7.2f} | {builds:>6.0f} {built:>9.0f} | {refits:>6.0f} {refitted:>10.0f} | "
              f"{u[0]:5.2f} {u[1]:6.2f} {u[2]:6.2f}")


if __name__ == "__main__":
    main()
