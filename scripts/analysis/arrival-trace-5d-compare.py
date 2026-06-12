#!/usr/bin/env python3
"""
Compare Stratum-vs-Linux pre-cake arrival pcaps across five statistical
dimensions selected for input-pattern phase-effects analysis:

    1. Per-flow idle-period tail (gaps > 1 ms), in milliseconds
    2. Burst-count per 1s window (cross-flow burst rate)
    3. Multi-flow interleaving: cross-host transition rate over sub-ms slots
    4. Host-B single-flow silence-fraction over T-windows  [LOAD-BEARING]
    5. Loss-recovery rebackoff gap (gaps > 50 ms), in milliseconds

Per dimension, compute the scipy.stats Wasserstein distance and KS test
between Stratum's and Linux's distributions. Emit a ranked CSV table.

Input CSV format (from arrival-trace-5d-extract.py):
    ts_ns, src_ip, src_port, dst_ip, dst_port, length, is_ack

The Linux side typically has two CSVs (va1 + vb1) passed as comma-separated
paths via --linux.
"""
import argparse
import csv
import ipaddress
from collections import defaultdict

import numpy as np
from scipy import stats


def load_csv(paths):
    """Load + concatenate one or more CSVs. Returns list of dicts sorted by ts_ns."""
    rows = []
    for p in paths:
        with open(p) as fh:
            for row in csv.DictReader(fh):
                rows.append({
                    "ts_ns": int(row["ts_ns"]),
                    "src_ip": row["src_ip"],
                    "src_port": int(row["src_port"]),
                    "length": int(row["length"]),
                    "is_ack": int(row["is_ack"]),
                })
    rows.sort(key=lambda r: r["ts_ns"])
    return rows


def _in_net(ip_str, net):
    try:
        return ipaddress.ip_address(ip_str) in net
    except ValueError:
        return False


def dim4_host_b_silence_fraction(rows, host_b_net, t_window_ns):
    """
    Per-T-window host-B activity indicator (0=silent, 1=active).
    Returns binary array — Wasserstein on this is dim 4.
    """
    net = ipaddress.ip_network(host_b_net)
    b_rows = [r for r in rows if not r["is_ack"] and _in_net(r["src_ip"], net)]
    if len(b_rows) < 2:
        return np.array([])
    t0 = b_rows[0]["ts_ns"]
    t_end = b_rows[-1]["ts_ns"]
    n_windows = max(1, (t_end - t0) // t_window_ns + 1)
    active = np.zeros(n_windows, dtype=np.int32)
    for r in b_rows:
        idx = (r["ts_ns"] - t0) // t_window_ns
        if 0 <= idx < n_windows:
            active[idx] = 1
    return 1 - active  # silence indicator


def dim1_per_flow_idle_tail(rows, host_a_net, host_b_net):
    """Per-flow inter-packet gaps > 1 ms (only data packets), in ms."""
    a_net = ipaddress.ip_network(host_a_net)
    b_net = ipaddress.ip_network(host_b_net)
    by_flow = defaultdict(list)
    for r in rows:
        if r["is_ack"]:
            continue
        if _in_net(r["src_ip"], a_net) or _in_net(r["src_ip"], b_net):
            by_flow[(r["src_ip"], r["src_port"])].append(r["ts_ns"])

    gaps_ms = []
    for ts_list in by_flow.values():
        if len(ts_list) < 2:
            continue
        gaps = np.diff(sorted(ts_list))
        gaps_ms.extend(g / 1_000_000.0 for g in gaps if g > 1_000_000)
    return np.array(gaps_ms)


def dim2_burst_count_per_sec(rows, host_a_net, host_b_net):
    """Burst-start count per 1s window across all flows. Burst gap = 5 ms."""
    a_net = ipaddress.ip_network(host_a_net)
    b_net = ipaddress.ip_network(host_b_net)
    by_flow = defaultdict(list)
    for r in rows:
        if r["is_ack"]:
            continue
        if _in_net(r["src_ip"], a_net) or _in_net(r["src_ip"], b_net):
            by_flow[(r["src_ip"], r["src_port"])].append(r["ts_ns"])

    BURST_GAP_NS = 5_000_000
    bursts_per_sec = defaultdict(int)
    for ts_list in by_flow.values():
        ts_sorted = sorted(ts_list)
        for i, t in enumerate(ts_sorted):
            if i == 0 or t - ts_sorted[i - 1] > BURST_GAP_NS:
                sec = t // 1_000_000_000
                bursts_per_sec[sec] += 1
    return np.array(list(bursts_per_sec.values()), dtype=float)


def dim3_interleaving_transitions(rows, host_a_net, host_b_net, slot_ns=100_000):
    """Per 100 µs slot, dominant host; output is transition rate sequence."""
    a_net = ipaddress.ip_network(host_a_net)
    b_net = ipaddress.ip_network(host_b_net)
    if not rows:
        return np.array([])
    t0 = rows[0]["ts_ns"]
    t_end = rows[-1]["ts_ns"]
    n_slots = max(1, (t_end - t0) // slot_ns + 1)
    a_cnt = np.zeros(n_slots, dtype=np.int32)
    b_cnt = np.zeros(n_slots, dtype=np.int32)
    for r in rows:
        if r["is_ack"]:
            continue
        addr_a = _in_net(r["src_ip"], a_net)
        addr_b = _in_net(r["src_ip"], b_net)
        if not (addr_a or addr_b):
            continue
        idx = (r["ts_ns"] - t0) // slot_ns
        if not (0 <= idx < n_slots):
            continue
        if addr_a:
            a_cnt[idx] += 1
        else:
            b_cnt[idx] += 1
    dominant = np.full(n_slots, -1, dtype=np.int32)
    dominant[a_cnt > b_cnt] = 0
    dominant[b_cnt > a_cnt] = 1
    transitions = (dominant[1:] != dominant[:-1]).astype(float)
    return transitions


def dim5_loss_rebackoff(rows, host_a_net, host_b_net):
    """Per-flow inter-packet gaps > 50 ms (loss-recovery silence), in ms."""
    a_net = ipaddress.ip_network(host_a_net)
    b_net = ipaddress.ip_network(host_b_net)
    by_flow = defaultdict(list)
    for r in rows:
        if r["is_ack"]:
            continue
        if _in_net(r["src_ip"], a_net) or _in_net(r["src_ip"], b_net):
            by_flow[(r["src_ip"], r["src_port"])].append(r["ts_ns"])
    rebackoff_ms = []
    for ts_list in by_flow.values():
        if len(ts_list) < 2:
            continue
        gaps = np.diff(sorted(ts_list))
        rebackoff_ms.extend(g / 1_000_000.0 for g in gaps if g > 50_000_000)
    return np.array(rebackoff_ms)


def safe_wasserstein(a, b):
    if a.size == 0 or b.size == 0:
        return float("nan")
    lo = float(min(a.min(), b.min()))
    hi = float(max(a.max(), b.max()))
    if hi == lo:
        return 0.0
    aa = (a - lo) / (hi - lo)
    bb = (b - lo) / (hi - lo)
    return float(stats.wasserstein_distance(aa, bb))


def safe_ks(a, b):
    if a.size == 0 or b.size == 0:
        return (float("nan"), float("nan"))
    res = stats.ks_2samp(a, b)
    return (float(res.statistic), float(res.pvalue))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stratum", required=True, help="comma-sep CSVs from Stratum side")
    ap.add_argument("--linux", required=True, help="comma-sep CSVs from Linux side")
    ap.add_argument("--host-a-net", default="10.1.1.0/24")
    ap.add_argument("--host-b-net", default="10.1.2.0/24")
    ap.add_argument("--t-window-ms", type=float, default=5.0)
    ap.add_argument("--out-table", required=True)
    args = ap.parse_args()

    s_rows = load_csv(args.stratum.split(","))
    l_rows = load_csv(args.linux.split(","))

    t_window_ns = int(args.t_window_ms * 1_000_000)

    dims = [
        ("1_idle_period_tail_ms",
         dim1_per_flow_idle_tail(s_rows, args.host_a_net, args.host_b_net),
         dim1_per_flow_idle_tail(l_rows, args.host_a_net, args.host_b_net)),
        ("2_burst_count_per_sec",
         dim2_burst_count_per_sec(s_rows, args.host_a_net, args.host_b_net),
         dim2_burst_count_per_sec(l_rows, args.host_a_net, args.host_b_net)),
        ("3_interleaving_transition_rate",
         dim3_interleaving_transitions(s_rows, args.host_a_net, args.host_b_net),
         dim3_interleaving_transitions(l_rows, args.host_a_net, args.host_b_net)),
        (f"4_host_b_silence_fraction_T{int(args.t_window_ms)}ms",
         dim4_host_b_silence_fraction(s_rows, args.host_b_net, t_window_ns),
         dim4_host_b_silence_fraction(l_rows, args.host_b_net, t_window_ns)),
        ("5_loss_rebackoff_gap_ms",
         dim5_loss_rebackoff(s_rows, args.host_a_net, args.host_b_net),
         dim5_loss_rebackoff(l_rows, args.host_a_net, args.host_b_net)),
    ]

    results = []
    for name, s, l in dims:
        wd = safe_wasserstein(s, l)
        ks_stat, ks_p = safe_ks(s, l)
        results.append({
            "dimension": name,
            "n_stratum": int(s.size),
            "n_linux": int(l.size),
            "wasserstein": wd,
            "ks_stat": ks_stat,
            "ks_p": ks_p,
            "stratum_mean": float(np.mean(s)) if s.size else float("nan"),
            "linux_mean": float(np.mean(l)) if l.size else float("nan"),
        })

    results.sort(key=lambda r: float("-inf") if np.isnan(r["wasserstein"]) else r["wasserstein"], reverse=True)

    with open(args.out_table, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(results[0].keys()))
        w.writeheader()
        for row in results:
            w.writerow(row)

    print(f"Wrote {args.out_table}")
    for r in results:
        print(f"  {r['dimension']:40s}  W={r['wasserstein']:.4f}  KS={r['ks_stat']:.4f} p={r['ks_p']:.3g}  "
              f"(n_s={r['n_stratum']} n_l={r['n_linux']})")


if __name__ == "__main__":
    main()
