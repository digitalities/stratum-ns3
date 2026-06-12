#!/usr/bin/env python3
"""Phase D S1 plot generator: L4S-on vs L4S-off latency comparison.

Reads per-packet OWD / IPDV CSVs and queue-state CSVs produced by
`diffserv-l4s-s1-latency`, emits time-series + CDF plots, a boxplot of
percentiles, and a markdown summary table.

Usage:
    python3 scripts/l4s-s1-plots.py \\
        [--data-dir output/comparison/l4s-vs-classic/scenario-1] \\
        [--fig-dir handbook/figures/N-l4s]
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def read_owd(path: Path):
    if not path.exists():
        return np.empty((0, 3))
    data = np.genfromtxt(path, delimiter=",", skip_header=1)
    if data.ndim == 1:
        data = data.reshape(1, -1)
    return data


def read_queue_state(path: Path):
    if not path.exists():
        return np.empty((0, 6))
    data = np.genfromtxt(path, delimiter=",", skip_header=1)
    if data.ndim == 1:
        data = data.reshape(1, -1)
    return data


def percentiles(arr, ps=(50, 95, 99)):
    if arr.size == 0:
        return {p: float("nan") for p in ps}
    return {p: float(np.percentile(arr, p)) for p in ps}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--data-dir", default="output/comparison/l4s-vs-classic/scenario-1"
    )
    ap.add_argument("--fig-dir", default="handbook/figures/N-l4s")
    args = ap.parse_args()

    data_dir = Path(args.data_dir)
    fig_dir = Path(args.fig_dir)
    fig_dir.mkdir(parents=True, exist_ok=True)

    # Load both modes.
    owd_on_ef = read_owd(data_dir / "l4s-on__owd-ef.csv")
    owd_off_ef = read_owd(data_dir / "l4s-off__owd-ef.csv")
    owd_on_af = read_owd(data_dir / "l4s-on__owd-af.csv")
    owd_off_af = read_owd(data_dir / "l4s-off__owd-af.csv")
    qs_on = read_queue_state(data_dir / "l4s-on__queue-state.csv")
    qs_off = read_queue_state(data_dir / "l4s-off__queue-state.csv")

    # --- Plot 1: OWD time-series EF (overlay L4S-on vs L4S-off) -----------
    fig, ax = plt.subplots(figsize=(9, 4))
    if owd_on_ef.size:
        ax.plot(owd_on_ef[:, 0], owd_on_ef[:, 1], label="L4S-on EF", linewidth=0.6,
                color="tab:blue")
    if owd_off_ef.size:
        ax.plot(owd_off_ef[:, 0], owd_off_ef[:, 1], label="L4S-off EF",
                linewidth=0.6, color="tab:orange", alpha=0.8)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("One-way delay (ms)")
    ax.set_title("S1 — EF flow OWD: L4S-on vs L4S-off (UDP CBR 500 kbps probe)")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-1-owd-ef-timeseries.png", dpi=150)
    plt.close(fig)

    # --- Plot 2: OWD CDF EF -----------------------------------------------
    fig, ax = plt.subplots(figsize=(7, 5))
    for arr, lab, color in (
        (owd_on_ef, "L4S-on EF", "tab:blue"),
        (owd_off_ef, "L4S-off EF", "tab:orange"),
    ):
        if arr.size:
            sorted_owd = np.sort(arr[:, 1])
            y = np.arange(1, len(sorted_owd) + 1) / len(sorted_owd)
            ax.plot(sorted_owd, y, label=lab, linewidth=1.2, color=color)
    ax.set_xlabel("One-way delay (ms)")
    ax.set_ylabel("CDF")
    ax.set_title("S1 — EF OWD distribution")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-1-owd-ef-cdf.png", dpi=150)
    plt.close(fig)

    # --- Plot 3: Boxplot of OWD across EF and AF --------------------------
    labels = ["EF\nL4S-on", "EF\nL4S-off", "AF\nL4S-on", "AF\nL4S-off"]
    arrays = [
        owd_on_ef[:, 1] if owd_on_ef.size else np.array([]),
        owd_off_ef[:, 1] if owd_off_ef.size else np.array([]),
        owd_on_af[:, 1] if owd_on_af.size else np.array([]),
        owd_off_af[:, 1] if owd_off_af.size else np.array([]),
    ]
    fig, ax = plt.subplots(figsize=(8, 5))
    ax.boxplot(arrays, labels=labels, showfliers=False, whis=(5, 95))
    ax.set_ylabel("One-way delay (ms)")
    ax.set_title("S1 — OWD boxplot (5–95 whiskers, fliers hidden)")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-1-owd-boxplot.png", dpi=150)
    plt.close(fig)

    # --- Plot 4: Queue state (L4S-on only): q0, q1, p' --------------------
    fig, ax = plt.subplots(figsize=(9, 4))
    if qs_on.size:
        ax.plot(qs_on[:, 0], qs_on[:, 1], label="q0 (L4S) pkts",
                color="tab:blue", linewidth=0.8)
        ax.plot(qs_on[:, 0], qs_on[:, 2], label="q1 (classic) pkts",
                color="tab:orange", linewidth=0.8)
        ax2 = ax.twinx()
        # p' at column 3 when present
        if qs_on.shape[1] >= 4:
            ax2.plot(qs_on[:, 0], qs_on[:, 3], label="p' (base prob)",
                     color="tab:red", linewidth=0.6, alpha=0.7)
            ax2.set_ylabel("p'", color="tab:red")
            ax2.tick_params(axis="y", labelcolor="tab:red")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Packets in queue")
    ax.set_title("S1 (L4S-on) — queue occupancy + controller base prob")
    ax.legend(loc="upper left")
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-1-queue-state-on.png", dpi=150)
    plt.close(fig)

    # --- Summary markdown -------------------------------------------------
    summary = data_dir / "comparison-summary.md"
    with open(summary, "w") as f:
        f.write("# Scenario 1 (Phase D) — L4S-on vs L4S-off latency\n\n")
        f.write("Topology: sender — 100 Mbps/1 ms — router — 10 Mbps/5 ms — receiver.\n")
        f.write("Flows: EF 500 kbps UDP CBR (ECT(1) in l4s-on, NotECT in l4s-off); "
                "AF 9.5 Mbps UDP CBR (NotECT both modes).\n\n")

        f.write("## OWD percentiles (EF flow)\n\n")
        f.write("| Metric | L4S-on | L4S-off | Δ (on − off) |\n")
        f.write("|--------|-------:|--------:|-------------:|\n")
        for p in (50, 95, 99):
            a = percentiles(owd_on_ef[:, 1] if owd_on_ef.size else np.array([]))[p]
            b = percentiles(owd_off_ef[:, 1] if owd_off_ef.size else np.array([]))[p]
            delta = a - b
            f.write(f"| P{p} OWD (ms) | {a:.3f} | {b:.3f} | {delta:+.3f} |\n")
        f.write("\n")

        f.write("## OWD percentiles (AF flow)\n\n")
        f.write("| Metric | L4S-on | L4S-off | Δ (on − off) |\n")
        f.write("|--------|-------:|--------:|-------------:|\n")
        for p in (50, 95, 99):
            a = percentiles(owd_on_af[:, 1] if owd_on_af.size else np.array([]))[p]
            b = percentiles(owd_off_af[:, 1] if owd_off_af.size else np.array([]))[p]
            delta = a - b
            f.write(f"| P{p} OWD (ms) | {a:.3f} | {b:.3f} | {delta:+.3f} |\n")
        f.write("\n")

        f.write("## IPDV summary (EF)\n\n")
        f.write("| Metric | L4S-on | L4S-off |\n|--------|-------:|--------:|\n")
        for p in (50, 95, 99):
            a_arr = owd_on_ef[:, 2] if owd_on_ef.size else np.array([])
            b_arr = owd_off_ef[:, 2] if owd_off_ef.size else np.array([])
            pa = percentiles(a_arr)[p] if a_arr.size else float("nan")
            pb = percentiles(b_arr)[p] if b_arr.size else float("nan")
            f.write(f"| P{p} IPDV (ms) | {pa:.3f} | {pb:.3f} |\n")
        f.write("\n")

        f.write("## Plots\n\n")
        for name in (
            "scenario-1-owd-ef-timeseries.png",
            "scenario-1-owd-ef-cdf.png",
            "scenario-1-owd-boxplot.png",
            "scenario-1-queue-state-on.png",
        ):
            f.write(f"- `{fig_dir}/{name}`\n")

    print(f"Summary + 4 plots written to {fig_dir}/ and {summary}")


if __name__ == "__main__":
    main()
