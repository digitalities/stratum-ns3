#!/usr/bin/env python3
"""Closed-loop bridge MVP verdict.

Reads pre-registered bands from bands.yaml, aggregates iperf3 JSON across
N replicas, classifies the result, writes verdict.json. The bands.yaml git
SHA is recorded in the verdict to prove that bands were not adjusted
post-hoc.
"""

from __future__ import annotations

import argparse
import datetime
import glob
import json
import statistics
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("verdict.py requires PyYAML. Install with: pip3 install pyyaml")


def parse_iperf3_bytes_sent(json_path: Path) -> int:
    """Sum bytes_sent across all parallel streams (sender side)."""
    with json_path.open() as f:
        data = json.load(f)
    if "end" not in data or "streams" not in data["end"]:
        raise ValueError(f"{json_path}: malformed iperf3 JSON")
    return sum(int(s["sender"]["bytes"]) for s in data["end"]["streams"])


def git_sha_of(path: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "log", "-1", "--format=%H", "--", path.name],
            cwd=path.parent, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"


def classify(mean_share: float, bands: dict) -> tuple[str, str]:
    s = bands["strength_qualifier"]
    midpoint = bands["midpoint"]
    if mean_share < s["OUT_OF_RANGE_LOW"][1]:
        return ("OUT_OF_RANGE", "OUT_OF_RANGE_LOW")
    if mean_share > s["OUT_OF_RANGE_HIGH"][0]:
        return ("OUT_OF_RANGE", "OUT_OF_RANGE_HIGH")
    primary = "VALIDATED" if mean_share <= midpoint else "INVALIDATED"
    for label, bounds in s.items():
        if label.startswith("OUT_OF_RANGE"):
            continue
        lo, hi = bounds
        if lo < mean_share <= hi:
            return (primary, label)
    return (primary, "BOUNDARY")


def ci95(values: list) -> tuple:
    mean = statistics.mean(values)
    if len(values) < 2:
        return (mean, mean, mean)
    stdev = statistics.stdev(values)
    sem = stdev / (len(values) ** 0.5)
    # Student-t 95% CI: df = n - 1
    t_table = {1: 12.7062, 2: 4.3027, 3: 3.1824, 4: 2.7764, 5: 2.5706}
    t = t_table.get(len(values) - 1, 1.96)
    return (mean, mean - t * sem, mean + t * sem)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bands", required=True, type=Path)
    p.add_argument("--output-dir", required=True, type=Path)
    p.add_argument("--replica-glob", required=True,
                   help="Glob for per-replica directories (e.g. output/r*/)")
    args = p.parse_args()

    bands = yaml.safe_load(args.bands.read_text())

    rdirs = sorted(Path(d) for d in glob.glob(args.replica_glob))
    if not rdirs:
        sys.exit(f"No replicas found at glob: {args.replica_glob}")

    replicas = []
    for rdir in rdirs:
        ja = rdir / "iperf3-src-a.json"
        jb = rdir / "iperf3-src-b.json"
        if not ja.exists() or not jb.exists():
            sys.exit(f"{rdir}: missing iperf3 JSONs")
        ba = parse_iperf3_bytes_sent(ja)
        bb = parse_iperf3_bytes_sent(jb)
        share = ba / (ba + bb) if (ba + bb) > 0 else 0.0
        replicas.append({
            "replica_dir": str(rdir),
            "bytes_a": ba,
            "bytes_b": bb,
            "share_a": share,
        })

    shares = [r["share_a"] for r in replicas]
    mean, lo, hi = ci95(shares)
    primary, strength = classify(mean, bands)

    cv = {
        "ci95_width": hi - lo,
        "ci95_width_limit": bands["cross_validation"]["ci95_max_width"],
        "ci95_within_limit": (hi - lo) <= bands["cross_validation"]["ci95_max_width"],
    }
    n_warnings = 0
    for rdir in rdirs:
        stderr_path = rdir / "ns3.stderr"
        if stderr_path.exists():
            n_warnings += stderr_path.read_text().count("hard limit exceeded")
    cv["real_time_hard_limit_warnings"] = n_warnings
    cv["real_time_hard_limit_within_limit"] = n_warnings <= \
        bands["cross_validation"]["real_time_hard_limit_warnings"]

    verdict = {
        "verdict": primary,
        "strength": strength,
        "mean_share_a": round(mean, 6),
        "ci95": [round(lo, 6), round(hi, 6)],
        "replicas": replicas,
        "bands_path": str(args.bands),
        "bands_git_sha": git_sha_of(args.bands),
        "computed_at": datetime.datetime.utcnow().isoformat() + "Z",
        "cross_validation": cv,
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    out_json = args.output_dir / "verdict.json"
    out_json.write_text(json.dumps(verdict, indent=2) + "\n")

    print()
    print("=" * 64)
    print("  STRATUM-BRIDGE CLOSED-LOOP VERDICT")
    print("=" * 64)
    print(f"  Reference points (from bands.yaml):")
    print(f"    Linux  ground truth:   {bands['reference_runs']['linux_native']}")
    print(f"    Stratum ground truth:  {bands['reference_runs']['ns3_stratum']}")
    print(f"    Midpoint (bright line): {bands['midpoint']}")
    print()
    print(f"  Observed: mean share_a = {mean:.4f}  CI95 [{lo:.4f}, {hi:.4f}]")
    print(f"  Replicas: {[round(s, 4) for s in shares]}")
    print()
    print(f"  PRIMARY VERDICT: {primary}")
    print(f"  STRENGTH:        {strength}")
    print()
    print(f"  Cross-validation:")
    print(f"    CI95 width {cv['ci95_width']:.4f} <= {cv['ci95_width_limit']}: " +
          ("PASS" if cv['ci95_within_limit'] else "FAIL"))
    print(f"    ns-3 hard-limit warnings: {n_warnings} <= 0: " +
          ("PASS" if cv['real_time_hard_limit_within_limit'] else "FAIL"))
    print()
    print(f"  Bands git SHA: {verdict['bands_git_sha']}")
    print(f"  Verdict JSON:  {out_json}")
    print("=" * 64)

    if primary == "OUT_OF_RANGE" or not cv["ci95_within_limit"]:
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
