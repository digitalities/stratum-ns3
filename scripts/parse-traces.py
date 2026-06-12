#!/usr/bin/env python3
"""
Parse ns-2 and ns-3 DiffServ trace files into a common CSV format.

Both simulators produce space-delimited trace files with identical column
layouts (by design — the ns-3 example matches the ns-2 output format).

Input:  a directory containing ServiceRate.tr, EFQueueLen.tr, BEQueueLen.tr,
        OWD.tr, IPDV.tr (and optionally *_FD.tr frequency distributions).
Output: a single CSV to stdout (or --output file) with schema:
        metric,time,queue,value

Usage:
    python3 parse-traces.py --input-dir baseline/ns2/example-1/PQ-0512
    python3 parse-traces.py --input-dir output/ns3/example-1/PQ-0512
"""

import argparse
import csv
import os
import sys


def parse_service_rate(path):
    """Parse ServiceRate.tr: 'time EFrate BErate' -> departure_rate rows."""
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 3:
                t, ef, be = float(parts[0]), float(parts[1]), float(parts[2])
                rows.append(("departure_rate", t, "EF", ef))
                rows.append(("departure_rate", t, "BE", be))
    return rows


def parse_queue_len(path, queue_name):
    """Parse EFQueueLen.tr or BEQueueLen.tr: 'time length' -> queue_length rows."""
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                t, length = float(parts[0]), float(parts[1])
                rows.append(("queue_length", t, queue_name, length))
    return rows


def parse_delay(path, metric_name):
    """Parse OWD.tr or IPDV.tr: 'time value' -> owd/ipdv rows."""
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                t, val = float(parts[0]), float(parts[1])
                rows.append((metric_name, t, "EF", val))
    return rows


def parse_frequency_distribution(path, metric_name):
    """Parse *_FD.tr: 'bin_center percentage' -> fd rows."""
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                bin_center, pct = float(parts[0]), float(parts[1])
                rows.append((metric_name, bin_center, "EF", pct))
    return rows


def parse_stdout_stats(path):
    """Parse ns2-stdout.log for per-CP packet statistics.

    Extracts from the LAST 'Packets Statistics' block (end-of-run stats).
    Returns list of (metric, 0, cp_name, value) rows.
    """
    rows = []
    if not os.path.exists(path):
        return rows

    with open(path) as f:
        content = f.read()

    # Find last stats block
    blocks = content.split("Packets Statistics")
    if len(blocks) < 2:
        return rows

    last_block = blocks[-1]
    for line in last_block.split("\n"):
        line = line.strip()
        if not line or line.startswith("===") or line.startswith("--") or line.startswith("CP"):
            continue
        if line.startswith("All"):
            parts = line.split()
            if len(parts) >= 5:
                rows.append(("total_pkts", 0, "ALL", float(parts[1])))
                rows.append(("tx_pct", 0, "ALL", float(parts[2].rstrip("%"))))
                rows.append(("ldrop_pct", 0, "ALL", float(parts[3].rstrip("%"))))
                rows.append(("edrop_pct", 0, "ALL", float(parts[4].rstrip("%"))))
            break
        parts = line.split()
        if len(parts) >= 5:
            try:
                cp = int(parts[0])
                cp_name = f"CP{cp}"
                rows.append(("total_pkts", 0, cp_name, float(parts[1])))
                rows.append(("tx_pct", 0, cp_name, float(parts[2].rstrip("%"))))
                rows.append(("ldrop_pct", 0, cp_name, float(parts[3].rstrip("%"))))
                rows.append(("edrop_pct", 0, cp_name, float(parts[4].rstrip("%"))))
            except ValueError:
                continue

    return rows


def parse_directory(input_dir):
    """Parse all trace files in a directory, return list of (metric, time, queue, value)."""
    all_rows = []

    sr = os.path.join(input_dir, "ServiceRate.tr")
    if os.path.exists(sr):
        all_rows.extend(parse_service_rate(sr))

    ef_ql = os.path.join(input_dir, "EFQueueLen.tr")
    if os.path.exists(ef_ql):
        all_rows.extend(parse_queue_len(ef_ql, "EF"))

    be_ql = os.path.join(input_dir, "BEQueueLen.tr")
    if os.path.exists(be_ql):
        all_rows.extend(parse_queue_len(be_ql, "BE"))

    owd = os.path.join(input_dir, "OWD.tr")
    if os.path.exists(owd):
        all_rows.extend(parse_delay(owd, "owd"))

    ipdv = os.path.join(input_dir, "IPDV.tr")
    if os.path.exists(ipdv):
        all_rows.extend(parse_delay(ipdv, "ipdv"))

    # Frequency distributions
    for fn in os.listdir(input_dir):
        if fn.endswith("_FD.tr"):
            if fn.startswith("owd"):
                all_rows.extend(parse_frequency_distribution(
                    os.path.join(input_dir, fn), "owd_fd"))
            elif fn.startswith("ipdv"):
                all_rows.extend(parse_frequency_distribution(
                    os.path.join(input_dir, fn), "ipdv_fd"))

    # Stdout stats (ns-2 only, contains per-CP packet counts)
    stdout_log = os.path.join(input_dir, "ns2-stdout.log")
    if os.path.exists(stdout_log):
        all_rows.extend(parse_stdout_stats(stdout_log))

    return all_rows


def main():
    parser = argparse.ArgumentParser(
        description="Parse DiffServ trace files into common CSV format")
    parser.add_argument("--input-dir", required=True,
                        help="Directory containing trace files")
    parser.add_argument("--output", default="-",
                        help="Output CSV file (default: stdout)")
    args = parser.parse_args()

    if not os.path.isdir(args.input_dir):
        print(f"ERROR: {args.input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    rows = parse_directory(args.input_dir)

    if args.output == "-":
        out = sys.stdout
    else:
        out = open(args.output, "w", newline="")

    writer = csv.writer(out)
    writer.writerow(["metric", "time", "queue", "value"])
    for row in rows:
        writer.writerow(row)

    if args.output != "-":
        out.close()
        print(f"Wrote {len(rows)} rows to {args.output}", file=sys.stderr)
    else:
        print(f"Parsed {len(rows)} rows", file=sys.stderr)


if __name__ == "__main__":
    main()
