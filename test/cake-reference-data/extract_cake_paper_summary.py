#!/usr/bin/env python3
# Copyright (C) 2001-2026  Sergio Andreozzi
# SPDX-License-Identifier: GPL-2.0-only
#
# Q-15.6 calibration: parse the Linux tc-cake reference dataset
# (Zenodo 10.5281/zenodo.1226887, Hoiland-Jorgensen et al. 2018,
# CC-BY-SA-4.0) and emit a small `cake-paper-summary.json` that the
# C++ Q-15.6 test loads at runtime.
#
# Usage:
#   python3 extract_cake_paper_summary.py \
#       --root /tmp/cake-reference-data \
#       --out  cake-paper-summary.json
#
# The 471 MB tarball is *not* tracked in git; the summary JSON is.

import argparse
import gzip
import json
import os
import statistics
import sys
from glob import glob


# Paper Fig. 4 (RRUL) and Fig. 5 (per-tin shares) are both derived from
# the rrul-diffserv batch. cake_diff4 maps the four marked TCP flows
# (BK/BE/CS5/EF) onto three of four diffserv4 tins (BK->Bulk, BE->BE,
# CS5+EF->Voice; Video tin is idle), so the per-tin shares observed
# here are the 3-active-tin equilibrium, not the 4-tin Fig. 5 reading.
RRUL_DIFFSERV_RELDIR = (
    "caketest-2018-04-13T203801-Full_test_run/rrul-diffserv"
)
TCP_32UP_VOIP_RELDIR = (
    "caketest-2018-04-17T163552-VoIP_diffserv_test_32_flows_"
    "diffserv-enabled_cake_2Mbps_VoIP_flow/tcp-32up-voip-diffserv"
)

# Steady-state window. Flent runs 150 s with 10 s warm-up; the paper
# figures ignore the first 30 s and teardown samples after 140 s.
T_STEADY_START = 30.0
T_STEADY_END = 140.0


def steady_mean(samples, x_values, t_start, t_end):
    """Mean of (sample, time) pairs whose time falls in [t_start, t_end]."""
    vals = [s for s, x in zip(samples, x_values)
            if s is not None and t_start <= x <= t_end]
    return statistics.fmean(vals) if vals else None


def steady_percentile(samples, x_values, t_start, t_end, q):
    vals = sorted(s for s, x in zip(samples, x_values)
                  if s is not None and t_start <= x <= t_end)
    if not vals:
        return None
    if q <= 0:
        return vals[0]
    if q >= 1:
        return vals[-1]
    idx = int(q * (len(vals) - 1))
    return vals[idx]


def parse_flent(path):
    """Return the per-series steady-state mean for a single flent.gz."""
    with gzip.open(path, "rt") as f:
        d = json.load(f)
    xv = d["x_values"]
    out = {}
    for series, samples in d["results"].items():
        if "::" in series:  # skip per-flow tcp_cwnd / tcp_rtt sub-series
            continue
        m = steady_mean(samples, xv, T_STEADY_START, T_STEADY_END)
        if m is not None:
            out.setdefault(series, {})["mean"] = m
        if series.startswith("Ping"):
            for q, label in [(0.50, "p50"), (0.95, "p95"), (0.99, "p99")]:
                v = steady_percentile(samples, xv, T_STEADY_START, T_STEADY_END, q)
                if v is not None:
                    out.setdefault(series, {})[label] = v
    return out


def aggregate(per_rep_dicts):
    """Median + IQR across N reps. Each per-rep dict is {series: {stat: v}}."""
    series_keys = set()
    for d in per_rep_dicts:
        series_keys.update(d.keys())
    out = {}
    for s in sorted(series_keys):
        out[s] = {}
        stat_keys = set()
        for d in per_rep_dicts:
            stat_keys.update(d.get(s, {}).keys())
        for k in sorted(stat_keys):
            vals = [d[s][k] for d in per_rep_dicts
                    if s in d and k in d[s]]
            if not vals:
                continue
            out[s][k] = {
                "median": statistics.median(vals),
                "q1": statistics.quantiles(vals, n=4)[0]
                if len(vals) > 1 else vals[0],
                "q3": statistics.quantiles(vals, n=4)[2]
                if len(vals) > 1 else vals[0],
                "n_reps": len(vals),
            }
    return out


def gather_batch(root, reldir, qdisc, bandwidth_dir):
    """Find and parse all rep files under reldir/bandwidth_dir matching qdisc."""
    pattern = os.path.join(
        root, reldir, bandwidth_dir, f"*-{qdisc}-cubic-noecn-*.flent.gz"
    )
    files = sorted(glob(pattern))
    return [parse_flent(p) for p in files]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True,
                    help="Path to extracted Zenodo tarball "
                         "(parent of caketest-2018-04-* dirs)")
    ap.add_argument("--out", required=True,
                    help="Output JSON path")
    args = ap.parse_args()

    summary = {
        "_meta": {
            "source_doi": "10.5281/zenodo.1226887",
            "source_title": "Piece of CAKE: A Comprehensive Queue "
                            "Management Solution for Home Gateways",
            "source_publication_date": "2018-04-23",
            "source_filename": "cake-paper-20180420-flent-data.tar.gz",
            "source_md5": "613b2d1526491eb110af3a46e9fd4144",
            "source_license": "CC-BY-SA-4.0",
            "source_authors": [
                "Hoiland-Jorgensen, Toke",
                "Taht, Dave",
                "Morton, Jonathan",
            ],
            "linux_kernel": "4.11 endpoints / 4.14 routers",
            "cake_commit": "16d7fed7ea0ef528d138cb7295aa51f55680ceef",
            "tcp_variant": "cubic",
            "ecn": "noecn",
            "rtt_added_ms": 50,
            "test_duration_s": 150,
            "steady_state_window_s": [T_STEADY_START, T_STEADY_END],
            "calibration_tolerance_fraction": 0.15,
        },
        "rrul_diffserv": {},
        "tcp_32up_voip_diffserv": {},
    }

    # rrul-diffserv: bandwidths actually present in the dataset.
    for bandwidth in ("1Mbit-10Mbit", "10Mbit-10Mbit", "100Mbit-100Mbit"):
        bandwidth_block = {}
        for qdisc in ("cake", "cake_diff3", "cake_diff4", "fq_codel"):
            bandwidth_dirs = sorted(glob(os.path.join(
                args.root, RRUL_DIFFSERV_RELDIR, bandwidth + "-*")))
            if not bandwidth_dirs:
                continue
            per_rep = []
            for bd in bandwidth_dirs:
                rep_files = sorted(glob(os.path.join(
                    bd, f"*-{qdisc}-cubic-noecn-*.flent.gz")))
                for f in rep_files:
                    per_rep.append(parse_flent(f))
            if per_rep:
                bandwidth_block[qdisc] = aggregate(per_rep)
        if bandwidth_block:
            summary["rrul_diffserv"][bandwidth] = bandwidth_block

    # tcp-32up-voip-diffserv lives in a separate caketest-* tree and only
    # has one bandwidth directory.
    for qdisc in ("cake", "cake_diff4", "fq_codel"):
        bandwidth_dirs = sorted(glob(os.path.join(
            args.root, TCP_32UP_VOIP_RELDIR, "*Mbit-*Mbit-*")))
        per_rep = {}
        for bd in bandwidth_dirs:
            band = os.path.basename(bd).rsplit("-", 1)[0]
            rep_files = sorted(glob(os.path.join(
                bd, f"*-{qdisc}-cubic-noecn-*.flent.gz")))
            if not rep_files:
                continue
            per_rep.setdefault(band, []).extend(parse_flent(p) for p in rep_files)
        for band, reps in per_rep.items():
            summary["tcp_32up_voip_diffserv"].setdefault(band, {})[qdisc] = (
                aggregate(reps)
            )

    with open(args.out, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
    print(f"Wrote {args.out}", file=sys.stderr)
    print(f"  rrul_diffserv bands: "
          f"{sorted(summary['rrul_diffserv'].keys())}", file=sys.stderr)
    print(f"  tcp_32up_voip_diffserv bands: "
          f"{sorted(summary['tcp_32up_voip_diffserv'].keys())}", file=sys.stderr)


if __name__ == "__main__":
    main()
