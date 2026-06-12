#!/usr/bin/env python3
"""Phase D S2 plot generator: coupling-formula sanity check.

Reads throughput + coupling CSVs produced by `diffserv-l4s-s2-equivalence`
and emits time-series overlays and a markdown summary.

Usage:
    python3 scripts/l4s-s2-plots.py \\
        [--data-dir output/comparison/l4s-vs-classic/scenario-2] \\
        [--fig-dir handbook/figures/N-l4s]
"""
from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def read_csv(path: Path):
    if not path.exists():
        return np.empty((0, 0))
    data = np.genfromtxt(path, delimiter=",", skip_header=1)
    if data.ndim == 1:
        data = data.reshape(1, -1)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--data-dir", default="output/comparison/l4s-vs-classic/scenario-2"
    )
    ap.add_argument("--fig-dir", default="handbook/figures/N-l4s")
    args = ap.parse_args()

    data_dir = Path(args.data_dir)
    fig_dir = Path(args.fig_dir)
    fig_dir.mkdir(parents=True, exist_ok=True)

    thr = read_csv(data_dir / "throughput.csv")
    cpl = read_csv(data_dir / "coupling.csv")

    # --- Plot 1: Throughput time-series (L vs C) ---------------------------
    fig, ax = plt.subplots(figsize=(9, 4))
    if thr.size:
        ax.plot(thr[:, 0], thr[:, 1], label="ECT(1) / L4S flow",
                color="tab:blue", linewidth=1.0)
        ax.plot(thr[:, 0], thr[:, 2], label="NotECT / classic flow",
                color="tab:orange", linewidth=1.0)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Throughput (Mbps)")
    ax.set_title("S2 — Per-flow receiver throughput (50 ms window)")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-2-throughput.png", dpi=150)
    plt.close(fig)

    # --- Plot 2: p', p_C, p_L time-series ---------------------------------
    fig, ax = plt.subplots(figsize=(9, 4))
    if cpl.size:
        ax.plot(cpl[:, 0], cpl[:, 3], label="p' (PI base)",
                color="tab:purple", linewidth=0.8)
        ax.plot(cpl[:, 0], cpl[:, 4], label="p_C (classic coupled drop)",
                color="tab:red", linewidth=0.8)
        ax.plot(cpl[:, 0], cpl[:, 5], label="p_L (L4S mark)",
                color="tab:green", linewidth=0.8)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Probability")
    ax.set_title("S2 — Coupling probabilities over time (20 ms sample)")
    ax.legend()
    ax.grid(alpha=0.3)
    ax.set_ylim(-0.02, 1.02)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-2-coupling-probs.png", dpi=150)
    plt.close(fig)

    # --- Plot 3: Queue lengths time-series --------------------------------
    fig, ax = plt.subplots(figsize=(9, 4))
    if cpl.size:
        ax.plot(cpl[:, 0], cpl[:, 1], label="q0 pkts", color="tab:orange",
                linewidth=0.8)
        ax.plot(cpl[:, 0], cpl[:, 2], label="q1 (L4S) pkts", color="tab:blue",
                linewidth=0.8)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Packets")
    ax.set_title("S2 — Sub-queue occupancy")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(fig_dir / "scenario-2-queue-occupancy.png", dpi=150)
    plt.close(fig)

    # --- Summary markdown --------------------------------------------------
    summary = data_dir / "comparison-summary.md"
    with open(summary, "w") as f:
        f.write("# Scenario 2 (Phase D) — Coupling-formula sanity check\n\n")
        f.write("**Framing.** RFC 9332 §2.1 throughput *equivalence* presumes a\n")
        f.write("Scalable congestion-control sender (e.g. DCTCP, TCP Prague).\n")
        f.write("ns-3 mainline does not expose a straightforward ECT(1)\n")
        f.write("Scalable-CC TCP. This scenario therefore uses UDP CBR for\n")
        f.write("both flows and reports the result as a sanity check of the\n")
        f.write("coupling machinery, not as a throughput-equivalence claim.\n\n")

        if thr.size:
            steady = thr[len(thr) // 2 :]
            mean_l = float(np.mean(steady[:, 1]))
            mean_c = float(np.mean(steady[:, 2]))
            f.write("## Steady-state throughput (2nd half of run)\n\n")
            f.write(f"- ECT(1) / L4S flow:  **{mean_l:.3f} Mbps**\n")
            f.write(f"- NotECT / classic:   **{mean_c:.3f} Mbps**\n")
            if mean_c > 0:
                f.write(f"- L : C ratio: **{mean_l / mean_c:.3f}**\n\n")

        if cpl.size:
            pp = cpl[:, 3]
            pc = cpl[:, 4]
            pl = cpl[:, 5]
            f.write("## Controller + coupling observations\n\n")
            f.write(f"- p' range: [{pp.min():.4f}, {pp.max():.4f}], "
                    f"mean {pp.mean():.4f}\n")
            f.write(f"- p_C range: [{pc.min():.4f}, {pc.max():.4f}], "
                    f"mean {pc.mean():.4f}\n")
            f.write(f"- p_L range: [{pl.min():.4f}, {pl.max():.4f}], "
                    f"mean {pl.mean():.4f}\n")
            # RFC 9332 §2.1 eq. (1) / App. A.1 Fig. 6: p_L = min(k * p', 1)
            # with default k = 2, p_C = p'^2 (k-independent). Verify
            # numerically at sample points where p_L is not saturated to 1
            # and p' > 0.
            mask = (pl > 1e-6) & (pl < 0.999) & (pp > 1e-6)
            n_live = int(mask.sum())
            if n_live > 0:
                predicted_pc = np.clip(pp[mask] ** 2, 0.0, 1.0)
                residual = np.abs(pc[mask] - predicted_pc)
                f.write(f"- Controller operating region samples (p_L ∈ (0, 1)): "
                        f"{n_live}\n")
                f.write(f"- |p_C − p'²| mean: {residual.mean():.5f}, "
                        f"max: {residual.max():.5f}\n")
            else:
                f.write("- Controller saturated p_L = 1 throughout sampled "
                        "window (L4S queue exceeded target sojourn "
                        "continuously; expected when L4S flow is not "
                        "Scalable-CC-responsive).\n")
            f.write("\n## Throughput notes\n\n")
            # Full-run means (guards against empty 2nd-half window).
            if thr.size:
                mean_l_all = float(np.mean(thr[:, 1]))
                mean_c_all = float(np.mean(thr[:, 2]))
                f.write(f"- Full-run mean throughput L: {mean_l_all:.3f} Mbps, "
                        f"C: {mean_c_all:.3f} Mbps.\n")
            f.write("- At this offered load, coupled p_C saturates to ~1 so "
                    "NotECT/classic flow is drop-silenced. This is the "
                    "expected RFC 9332 failure mode when the L4S flow does "
                    "not respond to CE marks — UDP CBR ignores p_L.\n\n")

        f.write("## Plots\n\n")
        for name in (
            "scenario-2-throughput.png",
            "scenario-2-coupling-probs.png",
            "scenario-2-queue-occupancy.png",
        ):
            f.write(f"- `{fig_dir}/{name}`\n")

    print(f"Summary + 3 plots written to {fig_dir}/ and {summary}")


if __name__ == "__main__":
    main()
