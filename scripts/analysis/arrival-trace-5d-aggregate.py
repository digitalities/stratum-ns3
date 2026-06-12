#!/usr/bin/env python3
"""Aggregate per-RngRun verdict CSVs into a single mean ± std table."""
import argparse
import csv
from collections import defaultdict

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    accum = defaultdict(lambda: defaultdict(list))
    for path in args.inp:
        with open(path) as fh:
            for row in csv.DictReader(fh):
                accum[row["dimension"]]["wasserstein"].append(float(row["wasserstein"]))
                accum[row["dimension"]]["ks_stat"].append(float(row["ks_stat"]))
                accum[row["dimension"]]["ks_p"].append(float(row["ks_p"]))
                accum[row["dimension"]]["n_stratum"].append(int(row["n_stratum"]))
                accum[row["dimension"]]["n_linux"].append(int(row["n_linux"]))

    rows = []
    for dim, kvs in accum.items():
        wd_mean = float(np.mean(kvs["wasserstein"]))
        wd_std = float(np.std(kvs["wasserstein"]))
        rel_std = float(wd_std / max(1e-12, abs(wd_mean)))
        rows.append({
            "dimension": dim,
            "wasserstein_mean": wd_mean,
            "wasserstein_std": wd_std,
            "wasserstein_relstd": rel_std,
            "determinism": "OK" if rel_std < 0.20 else "NOISY",
            "ks_stat_mean": float(np.mean(kvs["ks_stat"])),
            "ks_p_max": float(np.max(kvs["ks_p"])),
            "n_stratum_mean": float(np.mean(kvs["n_stratum"])),
            "n_linux_mean": float(np.mean(kvs["n_linux"])),
        })
    rows.sort(key=lambda r: -r["wasserstein_mean"])

    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        for r in rows:
            w.writerow(r)

    print(f"Wrote {args.out}\n")
    print(f"{'dimension':40s}  {'W mean':>8s}  {'rel-std':>8s}  {'det':>6s}  {'KS':>8s}")
    print(f"{'-'*40}  {'-'*8}  {'-'*8}  {'-'*6}  {'-'*8}")
    for r in rows:
        print(f"  {r['dimension']:38s}  {r['wasserstein_mean']:8.4f}  {r['wasserstein_relstd']:8.2%}  "
              f"{r['determinism']:>6s}  {r['ks_stat_mean']:8.4f}")


if __name__ == "__main__":
    main()
