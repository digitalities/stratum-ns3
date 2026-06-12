#!/usr/bin/env python3
"""
Multi-scheduler comparison: extract summary metrics from ns-2 and ns-3 trace
directories for all scheduler/packet-size combinations, then produce a summary
table and optional LaTeX output.

Usage:
    python3 scripts/multi-scheduler-comparison.py
    python3 scripts/multi-scheduler-comparison.py --latex paper/tables/comparison.tex
    python3 scripts/multi-scheduler-comparison.py --json output/comparison.json

Reads from:
    baseline/ns2/example-1/{SCHED}-{PKTSIZE}/  (ns-2 baselines)
    output/ns3/example-1/{SCHED}-{PKTSIZE}/    (ns-3 outputs)
"""

import argparse
import csv
import json
import math
import os
import sys

SCHEDULERS = ["PQ", "WFQ", "SCFQ", "SFQ", "WF2Qp"]  # LLQ excluded (no ns-2 baseline)
PACKET_SIZES = [64, 128, 256, 512, 1024, 1280, 1518]
WARMUP = 10.0  # seconds to skip for steady-state


def mean(vals):
    return sum(vals) / len(vals) if vals else 0.0


def parse_owd(path):
    """Parse OWD.tr -> list of (time, owd_ms)."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                rows.append((float(parts[0]), float(parts[1])))
    return rows


def parse_ipdv(path):
    """Parse IPDV.tr -> list of (time, ipdv_ms)."""
    return parse_owd(path)  # same format


def parse_service_rate(path):
    """Parse ServiceRate.tr -> ([(time, ef_kbps)], [(time, be_kbps)])."""
    ef, be = [], []
    if not os.path.exists(path):
        return ef, be
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 3:
                t = float(parts[0])
                ef.append((t, float(parts[1])))
                be.append((t, float(parts[2])))
    return ef, be


def parse_queue_len(path):
    """Parse EFQueueLen.tr or BEQueueLen.tr -> list of (time, length)."""
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                rows.append((float(parts[0]), float(parts[1])))
    return rows


def steady_values(series, warmup=WARMUP):
    """Filter to steady-state values and return just the values."""
    return [v for t, v in series if t >= warmup]


def pct_diff(ns2_val, ns3_val):
    if ns2_val == 0:
        return float("inf") if ns3_val != 0 else 0.0
    return (ns3_val - ns2_val) / ns2_val * 100.0


def extract_metrics(trace_dir):
    """Extract summary metrics from a single trace directory."""
    metrics = {}

    # OWD
    owd = parse_owd(os.path.join(trace_dir, "OWD.tr"))
    ss_owd = steady_values(owd)
    metrics["mean_owd_ms"] = mean(ss_owd) if ss_owd else None

    # IPDV
    ipdv = parse_ipdv(os.path.join(trace_dir, "IPDV.tr"))
    ss_ipdv = steady_values(ipdv)
    metrics["mean_ipdv_ms"] = mean(ss_ipdv) if ss_ipdv else None

    # Departure rates
    ef_dr, be_dr = parse_service_rate(os.path.join(trace_dir, "ServiceRate.tr"))
    ss_ef = steady_values(ef_dr)
    ss_be = steady_values(be_dr)
    metrics["ef_rate_kbps"] = mean(ss_ef) if ss_ef else None
    metrics["be_rate_kbps"] = mean(ss_be) if ss_be else None

    # Queue lengths
    ef_ql = parse_queue_len(os.path.join(trace_dir, "EFQueueLen.tr"))
    be_ql = parse_queue_len(os.path.join(trace_dir, "BEQueueLen.tr"))
    ss_efql = steady_values(ef_ql)
    ss_beql = steady_values(be_ql)
    metrics["ef_qlen_mean"] = mean(ss_efql) if ss_efql else None
    metrics["be_qlen_mean"] = mean(ss_beql) if ss_beql else None

    return metrics


def compare_run(ns2_dir, ns3_dir):
    """Compare metrics from one (scheduler, pktsize) pair."""
    ns2 = extract_metrics(ns2_dir)
    ns3 = extract_metrics(ns3_dir)

    row = {}
    for key in ["mean_owd_ms", "mean_ipdv_ms", "ef_rate_kbps", "be_rate_kbps",
                "ef_qlen_mean", "be_qlen_mean"]:
        v2 = ns2.get(key)
        v3 = ns3.get(key)
        row[f"ns2_{key}"] = v2
        row[f"ns3_{key}"] = v3
        if v2 is not None and v3 is not None:
            row[f"diff_{key}"] = pct_diff(v2, v3)
        else:
            row[f"diff_{key}"] = None
    return row


def main():
    parser = argparse.ArgumentParser(description="Multi-scheduler comparison")
    parser.add_argument("--ns2-base", default="baseline/ns2/example-1",
                        help="Base directory for ns-2 traces")
    parser.add_argument("--ns3-base", default="output/ns3/example-1",
                        help="Base directory for ns-3 traces")
    parser.add_argument("--latex", default=None, help="Output LaTeX table")
    parser.add_argument("--json", default=None, help="Output JSON report")
    parser.add_argument("--pktsize", type=int, default=512,
                        help="Default packet size for summary table (default: 512)")
    args = parser.parse_args()

    all_results = []
    summary_512 = []

    for sched in SCHEDULERS:
        for ps in PACKET_SIZES:
            tag = f"{sched}-{ps:04d}"
            ns2_dir = os.path.join(args.ns2_base, tag)
            ns3_dir = os.path.join(args.ns3_base, tag)

            if not os.path.isdir(ns2_dir):
                print(f"  SKIP {tag}: no ns-2 baseline", file=sys.stderr)
                continue
            if not os.path.isdir(ns3_dir):
                print(f"  SKIP {tag}: no ns-3 output", file=sys.stderr)
                continue

            row = compare_run(ns2_dir, ns3_dir)
            row["scheduler"] = sched
            row["pktsize"] = ps
            all_results.append(row)

            if ps == args.pktsize:
                summary_512.append(row)

    # Print summary table (default pktsize)
    print(f"\n{'='*110}")
    print(f"Multi-Scheduler Comparison — Packet Size {args.pktsize} bytes")
    print(f"{'='*110}")
    print(f"{'Sched':>8s} {'EF Rate':>10s} {'':>8s} {'BE Rate':>10s} {'':>8s} "
          f"{'OWD':>10s} {'':>8s} {'IPDV':>10s} {'':>8s}")
    print(f"{'':>8s} {'ns2':>5s} {'ns3':>5s} {'diff':>8s} {'ns2':>5s} {'ns3':>5s} {'diff':>8s} "
          f"{'ns2':>5s} {'ns3':>5s} {'diff':>8s} {'ns2':>5s} {'ns3':>5s} {'diff':>8s}")
    print("-" * 110)

    for r in summary_512:
        def fmt(key):
            v2 = r.get(f"ns2_{key}")
            v3 = r.get(f"ns3_{key}")
            d = r.get(f"diff_{key}")
            s2 = f"{v2:.1f}" if v2 is not None else "N/A"
            s3 = f"{v3:.1f}" if v3 is not None else "N/A"
            sd = f"{d:+.1f}%" if d is not None and not math.isinf(d) else "N/A"
            return s2, s3, sd

        ef2, ef3, efd = fmt("ef_rate_kbps")
        be2, be3, bed = fmt("be_rate_kbps")
        o2, o3, od = fmt("mean_owd_ms")
        i2, i3, id_ = fmt("mean_ipdv_ms")
        print(f"{r['scheduler']:>8s} {ef2:>5s} {ef3:>5s} {efd:>8s} "
              f"{be2:>5s} {be3:>5s} {bed:>8s} "
              f"{o2:>5s} {o3:>5s} {od:>8s} "
              f"{i2:>5s} {i3:>5s} {id_:>8s}")

    print(f"{'='*110}")
    print(f"\nTotal: {len(all_results)} runs compared "
          f"({len(SCHEDULERS)} schedulers x {len(PACKET_SIZES)} pkt sizes)")

    # Print per-scheduler OWD vs packet size table (thesis Test A data)
    print(f"\n{'='*80}")
    print("Mean OWD (ms) by Scheduler and Packet Size")
    print(f"{'='*80}")
    header = f"{'Sched':>8s}"
    for ps in PACKET_SIZES:
        header += f" {ps:>7d}"
    print(header)
    print("-" * 80)

    for sched in SCHEDULERS:
        line = f"{sched:>8s}"
        for ps in PACKET_SIZES:
            match = [r for r in all_results
                     if r["scheduler"] == sched and r["pktsize"] == ps]
            if match:
                v = match[0].get("ns3_mean_owd_ms")
                line += f" {v:7.2f}" if v is not None else "     N/A"
            else:
                line += "     N/A"
        print(line)

    # ns-2 row for comparison
    print()
    for sched in SCHEDULERS:
        line = f"{sched:>5s}(2)"
        for ps in PACKET_SIZES:
            match = [r for r in all_results
                     if r["scheduler"] == sched and r["pktsize"] == ps]
            if match:
                v = match[0].get("ns2_mean_owd_ms")
                line += f" {v:7.2f}" if v is not None else "     N/A"
            else:
                line += "     N/A"
        print(line)

    print(f"{'='*80}")

    # JSON output
    if args.json:
        with open(args.json, "w") as f:
            json.dump(all_results, f, indent=2)
        print(f"\nJSON report: {args.json}", file=sys.stderr)

    # LaTeX output
    if args.latex:
        os.makedirs(os.path.dirname(args.latex) or ".", exist_ok=True)
        with open(args.latex, "w") as f:
            f.write("% Auto-generated by multi-scheduler-comparison.py\n")
            f.write("\\begin{table}[t]\n")
            f.write("\\caption{Multi-scheduler comparison at 512-byte EF packets}\n")
            f.write("\\label{tab:multi-scheduler}\n")
            f.write("\\centering\\small\n")
            f.write("\\begin{tabular}{lrrrrrr}\n")
            f.write("\\toprule\n")
            f.write("Scheduler & \\multicolumn{2}{c}{EF Rate (kbps)} & "
                    "\\multicolumn{2}{c}{Mean OWD (ms)} & "
                    "\\multicolumn{2}{c}{Mean IPDV (ms)} \\\\\n")
            f.write("          & ns-2 & ns-3 & ns-2 & ns-3 & ns-2 & ns-3 \\\\\n")
            f.write("\\midrule\n")
            for r in summary_512:
                s = r["scheduler"]
                ef2 = f"{r['ns2_ef_rate_kbps']:.1f}" if r.get("ns2_ef_rate_kbps") else "--"
                ef3 = f"{r['ns3_ef_rate_kbps']:.1f}" if r.get("ns3_ef_rate_kbps") else "--"
                o2 = f"{r['ns2_mean_owd_ms']:.2f}" if r.get("ns2_mean_owd_ms") else "--"
                o3 = f"{r['ns3_mean_owd_ms']:.2f}" if r.get("ns3_mean_owd_ms") else "--"
                i2 = f"{r['ns2_mean_ipdv_ms']:.3f}" if r.get("ns2_mean_ipdv_ms") else "--"
                i3 = f"{r['ns3_mean_ipdv_ms']:.3f}" if r.get("ns3_mean_ipdv_ms") else "--"
                f.write(f"{s:10s} & {ef2} & {ef3} & {o2} & {o3} & {i2} & {i3} \\\\\n")
            f.write("\\bottomrule\n")
            f.write("\\end{tabular}\n")
            f.write("\\end{table}\n")
        print(f"\nLaTeX table: {args.latex}", file=sys.stderr)


if __name__ == "__main__":
    main()
