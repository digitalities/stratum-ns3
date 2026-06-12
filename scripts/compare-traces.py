#!/usr/bin/env python3
"""
Compare ns-2 and ns-3 DiffServ trace CSVs against Q-tier tolerances.

Reads two CSVs produced by parse-traces.py and evaluates the Q-1.x metrics
defined in specs/03-quality.md.

Usage:
    python3 compare-traces.py --ns2 /tmp/ns2-parsed.csv --ns3 /tmp/ns3-parsed.csv

Exit code 0 if all metrics pass, 1 if any fail.
"""

import argparse
import csv
import json
import math
import sys
from collections import defaultdict


def load_csv(path):
    """Load parsed CSV into {metric: {queue: [(time, value)]}}."""
    data = defaultdict(lambda: defaultdict(list))
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            metric = row["metric"]
            queue = row["queue"]
            t = float(row["time"])
            v = float(row["value"])
            data[metric][queue].append((t, v))
    # Sort by time
    for metric in data:
        for queue in data[metric]:
            data[metric][queue].sort(key=lambda x: x[0])
    return data


def values_only(series):
    """Extract just the values from a [(time, value)] series."""
    return [v for _, v in series]


def mean(vals):
    return sum(vals) / len(vals) if vals else 0.0


def stdev(vals):
    if len(vals) < 2:
        return 0.0
    m = mean(vals)
    return math.sqrt(sum((x - m) ** 2 for x in vals) / (len(vals) - 1))


def percentile(vals, pct):
    if not vals:
        return 0.0
    s = sorted(vals)
    k = (len(s) - 1) * pct / 100.0
    f = int(k)
    c = f + 1
    if c >= len(s):
        return s[f]
    return s[f] + (k - f) * (s[c] - s[f])


def steady_state(series, warmup=10.0):
    """Filter to steady-state values (after warmup period)."""
    return [v for t, v in series if t >= warmup]


def pct_diff(ns2_val, ns3_val):
    """Percentage difference: (ns3 - ns2) / ns2 * 100."""
    if ns2_val == 0:
        return float("inf") if ns3_val != 0 else 0.0
    return (ns3_val - ns2_val) / ns2_val * 100.0


class Result:
    def __init__(self, spec, description, ns2_val, ns3_val, diff_pct, tolerance_pct, passed):
        self.spec = spec
        self.description = description
        self.ns2_val = ns2_val
        self.ns3_val = ns3_val
        self.diff_pct = diff_pct
        self.tolerance_pct = tolerance_pct
        self.passed = passed

    def __str__(self):
        status = "PASS" if self.passed else "FAIL"
        return (f"{self.spec:20s} {self.description:40s} "
                f"ns2={self.ns2_val:12.4f}  ns3={self.ns3_val:12.4f}  "
                f"diff={self.diff_pct:+7.2f}%  tol={self.tolerance_pct}%  [{status}]")

    def to_dict(self):
        return {
            "spec": self.spec,
            "description": self.description,
            "ns2": self.ns2_val,
            "ns3": self.ns3_val,
            "diff_pct": self.diff_pct,
            "tolerance_pct": self.tolerance_pct,
            "passed": self.passed,
        }


def compare(ns2_data, ns3_data):
    """Run all Q-1.x comparisons, return list of Result objects."""
    results = []

    # --- Q-1.1: EF queue length ---
    ns2_ef_ql = steady_state(ns2_data["queue_length"]["EF"])
    ns3_ef_ql = steady_state(ns3_data["queue_length"]["EF"])
    if ns2_ef_ql and ns3_ef_ql:
        m2, m3 = mean(ns2_ef_ql), mean(ns3_ef_ql)
        d = pct_diff(m2, m3)
        results.append(Result("Q-1.1", "EF queue length mean", m2, m3, d, 10, abs(d) <= 10))

        s2, s3 = stdev(ns2_ef_ql), stdev(ns3_ef_ql)
        d = pct_diff(s2, s3) if s2 > 0 else (0.0 if s3 == 0 else float("inf"))
        results.append(Result("Q-1.1", "EF queue length stdev", s2, s3, d, 20, abs(d) <= 20))

    # --- Q-1.2: BE queue length ---
    ns2_be_ql = steady_state(ns2_data["queue_length"]["BE"])
    ns3_be_ql = steady_state(ns3_data["queue_length"]["BE"])
    if ns2_be_ql and ns3_be_ql:
        m2, m3 = mean(ns2_be_ql), mean(ns3_be_ql)
        d = pct_diff(m2, m3)
        results.append(Result("Q-1.2", "BE queue length mean", m2, m3, d, 10, abs(d) <= 10))

        s2, s3 = stdev(ns2_be_ql), stdev(ns3_be_ql)
        d = pct_diff(s2, s3) if s2 > 0 else (0.0 if s3 == 0 else float("inf"))
        results.append(Result("Q-1.2", "BE queue length stdev", s2, s3, d, 20, abs(d) <= 20))

    # --- Q-1.3: OWD for EF traffic ---
    ns2_owd = steady_state(ns2_data["owd"]["EF"])
    ns3_owd = steady_state(ns3_data["owd"]["EF"])
    if ns2_owd and ns3_owd:
        m2, m3 = mean(ns2_owd), mean(ns3_owd)
        d = pct_diff(m2, m3)
        results.append(Result("Q-1.3", "Mean OWD (EF) ms", m2, m3, d, 5, abs(d) <= 5))

        p2, p3 = percentile(ns2_owd, 99), percentile(ns3_owd, 99)
        d = pct_diff(p2, p3)
        results.append(Result("Q-1.3", "99th pctile OWD (EF) ms", p2, p3, d, 15, abs(d) <= 15))

    # --- Q-1.4: IPDV for EF traffic ---
    ns2_ipdv = steady_state(ns2_data["ipdv"]["EF"])
    ns3_ipdv = steady_state(ns3_data["ipdv"]["EF"])
    if ns2_ipdv and ns3_ipdv:
        m2, m3 = mean(ns2_ipdv), mean(ns3_ipdv)
        d = pct_diff(m2, m3)
        results.append(Result("Q-1.4", "Mean IPDV (EF) ms", m2, m3, d, 10, abs(d) <= 10))

    # --- Q-1.5: Per-queue departure rate ---
    ns2_ef_dr = steady_state(ns2_data["departure_rate"]["EF"])
    ns3_ef_dr = steady_state(ns3_data["departure_rate"]["EF"])
    if ns2_ef_dr and ns3_ef_dr:
        m2, m3 = mean(ns2_ef_dr), mean(ns3_ef_dr)
        d = pct_diff(m2, m3)
        results.append(Result("Q-1.5", "EF departure rate (kbps)", m2, m3, d, 5, abs(d) <= 5))

    ns2_be_dr = steady_state(ns2_data["departure_rate"]["BE"])
    ns3_be_dr = steady_state(ns3_data["departure_rate"]["BE"])
    if ns2_be_dr and ns3_be_dr:
        m2, m3 = mean(ns2_be_dr), mean(ns3_be_dr)
        d = pct_diff(m2, m3)
        results.append(Result("Q-1.5", "BE departure rate (kbps)", m2, m3, d, 5, abs(d) <= 5))

    # --- Q-1.6: srTCM colour distribution ---
    # Check per-CP stats from ns2-stdout.log if available
    ns2_cp46 = ns2_data.get("total_pkts", {}).get("CP46", [])
    ns2_cp48 = ns2_data.get("total_pkts", {}).get("CP48", [])
    ns3_cp46 = ns3_data.get("total_pkts", {}).get("CP46", [])
    ns3_cp48 = ns3_data.get("total_pkts", {}).get("CP48", [])
    if ns2_cp46 and ns3_cp46:
        # Total EF = CP46 + CP48
        ns2_46 = ns2_cp46[0][1] if ns2_cp46 else 0
        ns2_48 = ns2_cp48[0][1] if ns2_cp48 else 0
        ns3_46 = ns3_cp46[0][1] if ns3_cp46 else 0
        ns3_48 = ns3_cp48[0][1] if ns3_cp48 else 0
        ns2_total = ns2_46 + ns2_48
        ns3_total = ns3_46 + ns3_48
        if ns2_total > 0 and ns3_total > 0:
            ns2_green_pct = ns2_46 / ns2_total * 100
            ns3_green_pct = ns3_46 / ns3_total * 100
            d = ns3_green_pct - ns2_green_pct
            results.append(Result("Q-1.6", "GREEN fraction (ppts)",
                                  ns2_green_pct, ns3_green_pct, d, 2, abs(d) <= 2))

    return results


def print_trajectory(ns2_data, ns3_data, metric, queue, label):
    """Print per-second trajectory comparison for a metric/queue."""
    ns2_series = dict(ns2_data[metric][queue])
    ns3_series = dict(ns3_data[metric][queue])
    all_times = sorted(set(list(ns2_series.keys()) + list(ns3_series.keys())))

    print(f"\n--- {label} trajectory (per-second) ---")
    print(f"{'time':>6s}  {'ns2':>12s}  {'ns3':>12s}  {'diff%':>8s}")
    for t in all_times:
        if t < 5 or t > 195:
            continue
        v2 = ns2_series.get(t, float("nan"))
        v3 = ns3_series.get(t, float("nan"))
        if math.isnan(v2) or math.isnan(v3):
            d = "N/A"
        elif v2 == 0:
            d = "inf" if v3 != 0 else "0.00"
        else:
            d = f"{pct_diff(v2, v3):+.2f}"
        print(f"{t:6.0f}  {v2:12.2f}  {v3:12.2f}  {d:>8s}")


def main():
    parser = argparse.ArgumentParser(
        description="Compare DiffServ trace CSVs against Q-tier tolerances")
    parser.add_argument("--ns2", required=True, help="ns-2 parsed CSV")
    parser.add_argument("--ns3", required=True, help="ns-3 parsed CSV")
    parser.add_argument("--json", default=None, help="Output JSON report")
    parser.add_argument("--trajectory", action="store_true",
                        help="Print per-second departure rate trajectories")
    args = parser.parse_args()

    ns2_data = load_csv(args.ns2)
    ns3_data = load_csv(args.ns3)

    results = compare(ns2_data, ns3_data)

    # Print results
    print("=" * 120)
    print("Q-tier Tolerance Comparison Report")
    print("=" * 120)
    passed = 0
    failed = 0
    for r in results:
        print(r)
        if r.passed:
            passed += 1
        else:
            failed += 1

    print("-" * 120)
    print(f"OVERALL: {passed}/{passed + failed} PASS, {failed} FAIL")
    print("=" * 120)

    # Optional trajectory output
    if args.trajectory:
        print_trajectory(ns2_data, ns3_data, "departure_rate", "EF",
                         "EF departure rate (kbps)")
        print_trajectory(ns2_data, ns3_data, "departure_rate", "BE",
                         "BE departure rate (kbps)")

    # Optional JSON output
    if args.json:
        report = {
            "passed": passed,
            "failed": failed,
            "total": passed + failed,
            "results": [r.to_dict() for r in results],
        }
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nJSON report written to {args.json}", file=sys.stderr)

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
