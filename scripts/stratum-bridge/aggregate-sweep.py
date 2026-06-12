#!/usr/bin/env python3
"""Aggregate stratum-bridge sweep results against bundled ground truth.

Walks a sweep output directory (one subdir per cell, each containing
summary.txt produced by the emitted netns script), pulls the
ground-truth `share_A` measurement from a reference CSV bundled
alongside this script, and prints a markdown table comparing the
emitted result to the reference plus a PASS/FAIL summary line.

The ground-truth CSV ships at scripts/stratum-bridge/reference/
ground-truth-cells.csv; override via --ground-truth.

Usage:
    python3 scripts/stratum-bridge/aggregate-sweep.py /tmp/sb-output-sweep
    python3 scripts/stratum-bridge/aggregate-sweep.py --ground-truth my-cells.csv /tmp/out
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

DEFAULT_GROUND_TRUTH_CSV = (
    Path(__file__).resolve().parent / "reference" / "ground-truth-cells.csv"
)
TOLERANCE = 0.01

# Cell name pattern: cake-{N}-{M}-{protocol}
CELL_RE = re.compile(r"^cake-(\d+)-(\d+)-(cubic|newreno|bbr|udp)$")


def load_ground_truth(csv_path: Path) -> dict[tuple[int, int, str], tuple[float, float]]:
    gt = {}
    with csv_path.open() as fh:
        for row in csv.DictReader(fh):
            if row["implementation"] != "linux":
                continue
            key = (int(row["N"]), int(row["M"]), row["protocol"])
            gt[key] = (float(row["share_A_mean"]), float(row["share_A_std"]))
    return gt


def parse_summary(path: Path) -> tuple[float, float] | None:
    mean = std = None
    for line in path.read_text().splitlines():
        if line.startswith("share_A_mean"):
            mean = float(line.split("=")[1])
        elif line.startswith("share_A_pstd"):
            std = float(line.split("=")[1])
    if mean is None or std is None:
        return None
    return (mean, std)


def main() -> None:
    parser = argparse.ArgumentParser(description="Aggregate stratum-bridge sweep results.")
    parser.add_argument("sweep_dir", type=Path, help="Path to sweep output directory")
    parser.add_argument(
        "--ground-truth",
        type=Path,
        default=DEFAULT_GROUND_TRUTH_CSV,
        help="Path to ground-truth CSV (default: bundled reference/ground-truth-cells.csv)",
    )
    args = parser.parse_args()

    if not args.sweep_dir.is_dir():
        print(f"FAIL: sweep dir not found: {args.sweep_dir}", file=sys.stderr)
        sys.exit(1)
    if not args.ground_truth.is_file():
        print(f"FAIL: ground-truth CSV not found: {args.ground_truth}", file=sys.stderr)
        sys.exit(1)

    gt = load_ground_truth(args.ground_truth)
    rows = []
    for cell_dir in sorted(args.sweep_dir.iterdir()):
        if not cell_dir.is_dir():
            continue
        m = CELL_RE.match(cell_dir.name)
        if not m:
            continue
        N, M, protocol = int(m.group(1)), int(m.group(2)), m.group(3)
        summary_path = cell_dir / "summary.txt"
        if not summary_path.exists():
            print(f"WARN: missing summary for {cell_dir.name}", file=sys.stderr)
            continue
        parsed = parse_summary(summary_path)
        if parsed is None:
            print(f"WARN: could not parse summary for {cell_dir.name}", file=sys.stderr)
            continue
        emitted_mean, emitted_std = parsed
        gt_mean, gt_std = gt.get((N, M, protocol), (None, None))
        if gt_mean is None:
            print(f"WARN: no ground truth for {cell_dir.name}", file=sys.stderr)
            continue
        delta = abs(emitted_mean - gt_mean)
        verdict = "PASS" if delta <= TOLERANCE else "FAIL"
        rows.append({
            "name": cell_dir.name,
            "N": N,
            "M": M,
            "protocol": protocol,
            "emitted_mean": emitted_mean,
            "emitted_std": emitted_std,
            "gt_mean": gt_mean,
            "gt_std": gt_std,
            "delta": delta,
            "verdict": verdict,
        })

    if not rows:
        print("FAIL: no cells aggregated", file=sys.stderr)
        sys.exit(1)

    # Sort: (16,1) first, then (16,16); within each, cubic, newreno, bbr, udp
    protocol_order = {"cubic": 0, "newreno": 1, "bbr": 2, "udp": 3}
    rows.sort(key=lambda r: (r["M"], protocol_order.get(r["protocol"], 99)))

    print("| Scenario | Emitted (3-replica mean ± pstdev) | Reference ground truth | Δ | Verdict |")
    print("|---|---|---|---|---|")
    for r in rows:
        print(
            f"| `{r['name']}` "
            f"| {r['emitted_mean']:.4f} ± {r['emitted_std']:.4f} "
            f"| {r['gt_mean']:.4f} ± {r['gt_std']:.4f} "
            f"| {r['delta']:.4f} "
            f"| {r['verdict']} |"
        )

    n_pass = sum(1 for r in rows if r["verdict"] == "PASS")
    n_total = len(rows)
    max_delta = max(r["delta"] for r in rows)
    print(f"\n{n_pass}/{n_total} cells PASS within ±{TOLERANCE} tolerance; max Δ = {max_delta:.4f}")
    if n_pass < n_total:
        sys.exit(1)


if __name__ == "__main__":
    main()
