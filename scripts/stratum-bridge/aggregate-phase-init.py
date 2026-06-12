#!/usr/bin/env python3
"""Aggregate cake-host-fairness-sweep CSVs into per-stagger share_A.

Reads a directory of CSVs named stagger-{X}-rng-{N}.csv and prints a
markdown summary table comparing share_A across stagger values.

Usage: python3 aggregate-phase-init.py /tmp/probe-phase-init
"""

from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from pathlib import Path
from collections import defaultdict

FNAME_RE = re.compile(r"^stagger-(?P<stagger>[\d.]+)-rng-(?P<rng>\d+)\.csv$")


def share_a_for(rows: list[dict]) -> float | None:
    a = sum(float(r["goodput_mbps"]) for r in rows if r["host"] == "A")
    b = sum(float(r["goodput_mbps"]) for r in rows if r["host"] == "B")
    if a + b == 0:
        return None
    return a / (a + b)


COLUMNS = [
    "implementation",
    "strategy",
    "mode",
    "tcp_variant",
    "nFlowsA",
    "nFlowsB",
    "bandwidth_mbps",
    "duration_s",
    "rng_run",
    "host",
    "flow_idx",
    "goodput_mbps",
    "_d1",
    "_d2",
    "_d3",
    "_d4",
    "_d5",
    "_d6",
    "ts",
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dir", type=Path)
    args = parser.parse_args()

    cells = defaultdict(list)
    for csv_path in sorted(args.dir.glob("stagger-*-rng-*.csv")):
        m = FNAME_RE.match(csv_path.name)
        if not m:
            continue
        stagger = float(m["stagger"])
        rng = int(m["rng"])
        with csv_path.open() as fh:
            rows = list(csv.DictReader(fh, fieldnames=COLUMNS))
        s = share_a_for(rows)
        if s is None:
            continue
        cells[stagger].append((rng, s))

    if not cells:
        print("FAIL: no matched CSVs", file=sys.stderr)
        sys.exit(1)

    print("| Stagger (ms) | rng=1 | rng=2 | rng=3 | mean | pstdev |")
    print("|---|---|---|---|---|---|")
    for stagger in sorted(cells):
        runs = sorted(cells[stagger])
        per_rng = {r: s for r, s in runs}
        shares = [s for _, s in runs]
        mean = statistics.mean(shares)
        pstd = statistics.pstdev(shares) if len(shares) > 1 else 0.0
        row = f"| {stagger} "
        for r in (1, 2, 3):
            v = per_rng.get(r)
            row += f"| {v:.4f} " if v is not None else "| - "
        row += f"| {mean:.4f} | {pstd:.4f} |"
        print(row)

    print()
    baseline = cells.get(0.0)
    adversarial = [(s, cells[s]) for s in sorted(cells) if s != 0.0]
    if baseline:
        b_mean = statistics.mean(s for _, s in baseline)
        for s, runs in adversarial:
            a_mean = statistics.mean(s for _, s in runs)
            delta = a_mean - b_mean
            print(f"Δ vs baseline at stagger={s}ms: {delta:+.4f}")


if __name__ == "__main__":
    main()
