#!/usr/bin/env python3
"""Aggregate per-flow rows into per-cell mean/stddev with NaN-safe f.

f = (per_flow_share_A - share_A) / (per_flow_share_A - 0.50)
where per_flow_share_A = N / (N + M). For N == M, f is undefined
(0/0); we emit NaN and exclude from any cross-cell max|f| reduction.
"""

import os
from pathlib import Path

import numpy as np
import pandas as pd

REPO_ROOT = Path(os.environ.get(
    "REPO_ROOT", Path(__file__).resolve().parent.parent))
OUT_DIR = Path(os.environ.get(
    "OUT_DIR", REPO_ROOT / "output" / "ns3" / "cake-host-fairness"))


def _minmax_ratio(group: pd.DataFrame, host: str) -> float:
    sub = group[group["host"] == host]["goodput_mbps"]
    if len(sub) < 2 or sub.min() <= 0:
        return float("nan")
    return float(sub.max() / sub.min())


def main() -> int:
    raw = pd.read_csv(OUT_DIR / "sweep-perflow.csv")
    # Linux rows emit strategy="n/a" verbatim; pandas's default NA
    # interpretation maps "n/a" to NaN, then to_csv writes NaN as empty
    # string, breaking groupby alignment in this aggregator. Normalize
    # NaN strategy to the literal "none" string for group-keying.
    raw["strategy"] = raw["strategy"].fillna("none")

    cell_replica_keys = [
        "implementation", "strategy", "mode", "tcp_variant",
        "N", "M", "bandwidth_mbps", "duration_s", "rng_run",
    ]

    # Per-host sum of goodputs within each (cell, replica).
    per_host = (
        raw.groupby(cell_replica_keys + ["host"], dropna=False)
        ["goodput_mbps"].sum().reset_index()
    )

    # Pivot host into columns to compute share per replica.
    pivot = per_host.pivot_table(
        index=cell_replica_keys, columns="host", values="goodput_mbps",
        aggfunc="sum",
    ).reset_index()
    if "A" not in pivot.columns:
        pivot["A"] = 0.0
    if "B" not in pivot.columns:
        pivot["B"] = 0.0
    pivot["goodput_total_mbps"] = pivot["A"].fillna(0) + pivot["B"].fillna(0)
    pivot["share_A"] = pivot["A"].fillna(0) / pivot["goodput_total_mbps"]
    pivot["share_B"] = 1.0 - pivot["share_A"]

    # Jain's index + per-host minmax ratios per (cell, replica).
    metrics_per_replica = []
    for keys, group in raw.groupby(cell_replica_keys, dropna=False):
        x = group["goodput_mbps"].values
        n = len(x)
        sum_x = x.sum()
        sum_x_sq = (x ** 2).sum()
        jains = (sum_x ** 2) / (n * sum_x_sq) if sum_x_sq > 0 else np.nan
        metrics_per_replica.append({
            **dict(zip(cell_replica_keys, keys)),
            "jains_index": jains,
            "per_host_a_minmax_ratio": _minmax_ratio(group, "A"),
            "per_host_b_minmax_ratio": _minmax_ratio(group, "B"),
        })
    jdf = pd.DataFrame(metrics_per_replica)
    pivot = pivot.merge(jdf, on=cell_replica_keys, how="left")

    # f normalised metric (NaN for N == M).
    pivot["per_flow_share_A"] = pivot["N"] / (pivot["N"] + pivot["M"])
    with np.errstate(divide="ignore", invalid="ignore"):
        pivot["f"] = np.where(
            pivot["N"] == pivot["M"],
            np.nan,
            (pivot["per_flow_share_A"] - pivot["share_A"])
                / (pivot["per_flow_share_A"] - 0.50),
        )

    # Aggregate over replicas -> cell.
    cell_keys = [k for k in cell_replica_keys if k != "rng_run"]
    grp = pivot.groupby(cell_keys, dropna=False)
    cells = grp.agg(
        k_replicas=("rng_run", "nunique"),
        share_A_mean=("share_A", "mean"),
        share_A_stddev=("share_A", "std"),
        share_B_mean=("share_B", "mean"),
        share_B_stddev=("share_B", "std"),
        goodput_total_mean=("goodput_total_mbps", "mean"),
        jains_index_mean=("jains_index", "mean"),
        per_host_a_minmax_ratio_mean=("per_host_a_minmax_ratio", "mean"),
        per_host_b_minmax_ratio_mean=("per_host_b_minmax_ratio", "mean"),
        f_mean=("f", "mean"),
        f_stddev=("f", "std"),
        per_flow_share_A=("per_flow_share_A", "first"),
    ).reset_index()

    # Provenance carried through.
    prov_cols = [c for c in raw.columns
                 if c in ("git_sha", "kernel_release",
                          "iproute2_version", "image_sha")]
    prov = raw.groupby(cell_keys, dropna=False)[prov_cols].first().reset_index()
    cells = cells.merge(prov, on=cell_keys, how="left")

    # Derived labels for plot recipe.
    cells["implementation_strategy"] = cells.apply(
        lambda r: f"{r['implementation']}/{r['strategy']}", axis=1
    )

    out_path = OUT_DIR / "sweep-cells.csv"
    cells.to_csv(out_path, index=False)
    print(f"Wrote {out_path} ({len(cells)} cells).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
