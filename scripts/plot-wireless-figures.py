#!/usr/bin/env python3
"""
Generate the two figures consumed by the wireless-extension handbook
chapter:

  1. magrin-validation.{png,pdf} — aggregate UL+DL throughput vs number of
     STAs at MCS 5, 20 MHz, 5 GHz, BE saturation. Compares our measured
     curve to the published Magrin / Avallone / Roy / Zorzi WNS3 2021
     Figure 3 CWmin=15 curve (digitized from the published figure).

  2. scheduler-comparison.{png,pdf} — eight DiffServ4NS schedulers (PQ,
     RR, WRR, WIRR, SCFQ, WFQ, WF2Q+, LLQ) over the same 802.11a 6 Mb/s
     topology. Two stacked panels: per-class throughput (kb/s) and per-
     class p99 OWD (ms).

Inputs:
  handbook/figures/12-wireless/magrin-sweep.csv
  handbook/figures/12-wireless/scheduler-comparison.csv

Outputs:
  handbook/figures/12-wireless/magrin-validation.{png,pdf}
  handbook/figures/12-wireless/scheduler-comparison.{png,pdf}
"""

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

REPO = Path(__file__).resolve().parents[1]
FIG_DIR = REPO / "handbook" / "figures" / "12-wireless"

# Magrin Figure 3, CWmin=15 curve, digitized by visual inspection of the
# published figure (paper page 7 + slide 7 of the WNS3 2021 deck).
# Values are approximate to ±0.5 Mb/s; the figure itself is the precision
# floor.
MAGRIN_CWMIN15 = {
    1: 58.0,
    3: 54.0,
    5: 52.0,
    7: 50.0,
    9: 48.0,
    11: 47.0,
    13: 46.0,
    15: 45.0,
    17: 44.5,
    19: 44.0,
}


def plot_magrin_validation():
    csv_path = FIG_DIR / "magrin-sweep.csv"
    n_vals, agg = [], []
    with csv_path.open() as f:
        for row in csv.DictReader(f):
            n_vals.append(int(row["numStas"]))
            agg.append(float(row["aggregate_mbps"]))

    magrin_n = sorted(MAGRIN_CWMIN15.keys())
    magrin_y = [MAGRIN_CWMIN15[n] for n in magrin_n]

    fig, ax = plt.subplots(figsize=(6.5, 4.0))
    ax.plot(magrin_n, magrin_y, "o-", color="#1f77b4",
            label="Magrin et al. WNS3 2021, CWmin=15 (digitized)",
            markersize=6, linewidth=1.5)
    ax.plot(n_vals, agg, "x--", color="#d62728",
            label="Stratum example (this work)",
            markersize=8, linewidth=1.5)
    ax.set_xlabel("Number of STAs")
    ax.set_ylabel("Aggregate UL+DL throughput [Mbit/s]")
    ax.set_title("802.11ax SU saturation — MCS 5, 20 MHz, 5 GHz, BE")
    ax.set_xticks([1, 3, 5, 7, 9, 11, 13, 15, 17, 19])
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right", fontsize=9)
    ax.set_ylim(40, 62)

    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(FIG_DIR / f"magrin-validation.{ext}", dpi=150)
    plt.close(fig)
    print(f"Wrote {FIG_DIR}/magrin-validation.{{png,pdf}}")


def plot_scheduler_comparison():
    csv_path = FIG_DIR / "scheduler-comparison.csv"
    schedulers = []
    rows = []
    with csv_path.open() as f:
        for row in csv.DictReader(f):
            schedulers.append(row["scheduler"].upper().replace("WF2QP", "WF2Q+"))
            rows.append({
                "EF": (float(row["ef_kbps"]), float(row["ef_p99_ms"])),
                "AF41": (float(row["af_kbps"]), float(row["af_p99_ms"])),
                "BE": (float(row["be_kbps"]), float(row["be_p99_ms"])),
                "BK": (float(row["bk_kbps"]), float(row["bk_p99_ms"])),
            })

    classes = ["EF", "AF41", "BE", "BK"]
    class_colors = {"EF": "#d62728", "AF41": "#ff7f0e",
                    "BE": "#1f77b4", "BK": "#7f7f7f"}

    fig, (ax_top, ax_bot) = plt.subplots(2, 1, figsize=(8.5, 6.5),
                                          sharex=True)

    x = np.arange(len(schedulers))
    bar_w = 0.2

    for j, c in enumerate(classes):
        ys = [r[c][0] / 1000.0 for r in rows]  # kbps -> Mbps
        ax_top.bar(x + (j - 1.5) * bar_w, ys, bar_w,
                   label=c, color=class_colors[c])
    ax_top.set_ylabel("Per-class throughput [Mbit/s]")
    ax_top.set_title("DiffServ4NS schedulers over 802.11a 6 Mb/s — "
                     "saturating offered load, QoS off")
    ax_top.legend(loc="upper right", fontsize=9, ncol=4)
    ax_top.grid(True, axis="y", alpha=0.3)

    for j, c in enumerate(classes):
        ys = [r[c][1] for r in rows]
        ax_bot.bar(x + (j - 1.5) * bar_w, ys, bar_w,
                   label=c, color=class_colors[c])
    ax_bot.set_ylabel("Per-class p99 OWD [ms]")
    ax_bot.set_xticks(x)
    ax_bot.set_xticklabels(schedulers)
    ax_bot.grid(True, axis="y", alpha=0.3)

    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(FIG_DIR / f"scheduler-comparison.{ext}", dpi=150)
    plt.close(fig)
    print(f"Wrote {FIG_DIR}/scheduler-comparison.{{png,pdf}}")


def _read_sched_csv(path):
    """Load one CSV row per scheduler into the bar-plot row dict format."""
    schedulers = []
    rows = []
    with path.open() as f:
        for row in csv.DictReader(f):
            schedulers.append(row["scheduler"].upper().replace("WF2QP", "WF2Q+"))
            rows.append({
                "EF":   (float(row["ef_kbps"]), float(row["ef_p99_ms"])),
                "AF41": (float(row["af_kbps"]), float(row["af_p99_ms"])),
                "BE":   (float(row["be_kbps"]), float(row["be_p99_ms"])),
                "BK":   (float(row["bk_kbps"]), float(row["bk_p99_ms"])),
            })
    return schedulers, rows


def plot_scheduler_matrix():
    """2x2 throughput-bar matrix: rows = high/low load, cols = mode (off, hybrid).

    Each panel shows 8 schedulers x 4 classes. Visualises the (near-)equality
    of off vs hybrid at both loads, the headline finding of handbook section
    12.6.1.
    """
    panels = [
        ("off, high load",   FIG_DIR / "scheduler-comparison.csv"),
        ("hybrid, high load", FIG_DIR / "scheduler-comparison-wmm.csv"),
        ("off, low load",    FIG_DIR / "scheduler-comparison-low.csv"),
        ("hybrid, low load",  FIG_DIR / "scheduler-comparison-wmm-low.csv"),
    ]
    classes = ["EF", "AF41", "BE", "BK"]
    class_colors = {"EF": "#d62728", "AF41": "#ff7f0e",
                    "BE": "#1f77b4", "BK": "#7f7f7f"}

    fig, axes = plt.subplots(2, 2, figsize=(11.5, 7.0), sharey="row")

    for idx, (title, csv_path) in enumerate(panels):
        ax = axes[idx // 2, idx % 2]
        schedulers, rows = _read_sched_csv(csv_path)
        x = np.arange(len(schedulers))
        bar_w = 0.2
        for j, c in enumerate(classes):
            ys = [r[c][0] / 1000.0 for r in rows]  # kb/s -> Mb/s
            ax.bar(x + (j - 1.5) * bar_w, ys, bar_w,
                   label=c, color=class_colors[c])
        ax.set_title(title, fontsize=10)
        ax.set_xticks(x)
        ax.set_xticklabels(schedulers, fontsize=8, rotation=30, ha="right")
        ax.grid(True, axis="y", alpha=0.3)
        if idx % 2 == 0:
            ax.set_ylabel("Per-class throughput [Mbit/s]")
        if idx == 0:
            ax.legend(loc="upper right", fontsize=8, ncol=4)

    fig.suptitle("Stratum schedulers over 802.11a 6 Mb/s — "
                 "mode=off (qdisc only) vs mode=hybrid (qdisc + WMM), "
                 "high vs low load",
                 fontsize=11)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(FIG_DIR / f"scheduler-comparison-matrix.{ext}", dpi=150)
    plt.close(fig)
    print(f"Wrote {FIG_DIR}/scheduler-comparison-matrix.{{png,pdf}}")


def plot_edca_only_degeneracy():
    """Single-figure illustration of the edca-only mode: at high load the
    single-FIFO qdisc tail-drops EF proportionally to offered rate; at low
    load EDCA in a single-AP DL-only scenario does nothing (matches hybrid).
    """
    csv_path = FIG_DIR / "scheduler-comparison-edca-only.csv"
    loads = []
    rates = {"EF": [], "AF41": [], "BE": [], "BK": []}
    with csv_path.open() as f:
        for row in csv.DictReader(f):
            loads.append(row["scheduler"])  # "high-load" / "low-load"
            rates["EF"].append(float(row["ef_kbps"]) / 1000.0)
            rates["AF41"].append(float(row["af_kbps"]) / 1000.0)
            rates["BE"].append(float(row["be_kbps"]) / 1000.0)
            rates["BK"].append(float(row["bk_kbps"]) / 1000.0)

    classes = ["EF", "AF41", "BE", "BK"]
    class_colors = {"EF": "#d62728", "AF41": "#ff7f0e",
                    "BE": "#1f77b4", "BK": "#7f7f7f"}

    fig, ax = plt.subplots(figsize=(6.0, 3.5))
    x = np.arange(len(loads))
    bar_w = 0.2
    for j, c in enumerate(classes):
        ax.bar(x + (j - 1.5) * bar_w, rates[c], bar_w,
               label=c, color=class_colors[c])
    ax.set_xticks(x)
    ax.set_xticklabels(loads)
    ax.set_ylabel("Per-class throughput [Mbit/s]")
    ax.set_title("WMM mode = edca-only (single-queue qdisc, EDCA at L2)")
    ax.legend(loc="upper left", fontsize=9, ncol=4)
    ax.grid(True, axis="y", alpha=0.3)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(FIG_DIR / f"scheduler-comparison-edca-only.{ext}", dpi=150)
    plt.close(fig)
    print(f"Wrote {FIG_DIR}/scheduler-comparison-edca-only.{{png,pdf}}")


if __name__ == "__main__":
    plot_magrin_validation()
    plot_scheduler_comparison()
    plot_scheduler_matrix()
    plot_edca_only_degeneracy()
