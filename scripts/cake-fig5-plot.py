#!/usr/bin/env python3
"""CAKE Fig-5 figure: a 2 Mbit/s EF fixed-rate flow vs 32 bulk flows under three
bottleneck qdiscs (cake-diffserv / cake-besteffort / fq-codel).

Two panels:
  (a) latency over time (ns-3) — reproduces the paper's Fig. 5 shape (induced
      one-way delay vs time, bulk flows start at t=5 s).
  (b) was the flow served? — EF goodput (of 2 Mbit/s offered) with packet-loss
      annotated, ns-3 vs real Linux sch_cake side by side. This is the
      load-bearing addition: per-flow FQ alone (besteffort, fq-codel) holds
      delivered-packet latency low only by DROPPING most of the unresponsive
      flow; only DiffServ delivers the full rate at zero loss. The Linux arm
      cross-validates that ns-3's drop-not-queue failure matches the live kernel.

Inputs:
  output/ns3/cake-fig5/ns3-latency.csv        (arm,rng_run,t_s,induced_owd_ms)
  output/ns3/cake-fig5/ns3-goodput-loss.csv   (arm,rng_run,ef_goodput_mbps,
                                               bulk_goodput_mbps,ef_loss_pct)
  output/ns3/cake-fig5/linux-goodput-loss.csv (arm,ef_goodput_mbps,ef_loss_pct,
                                               bulk_goodput_mbps,...) [optional]
Output: cake-fig5.{svg,pdf} in output/ns3/cake-fig5/, paper/figures/,
        handbook/figures/cake-fig5/.
"""
import os
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

REPO_ROOT = Path(os.environ.get("REPO_ROOT", Path(__file__).resolve().parent.parent))
OUT_DIR = Path(os.environ.get("OUT_DIR", REPO_ROOT / "output" / "ns3" / "cake-fig5"))
PAPER_FIG_DIR = REPO_ROOT / "paper" / "figures"
HANDBOOK_FIG_DIR = REPO_ROOT / "handbook" / "figures" / "cake-fig5"

ARMS = [
    ("cake-diffserv", "CAKE DiffServ (EF tin)", "#4a9a5a"),
    ("cake-besteffort", "CAKE best-effort (per-flow FQ)", "#e08a3c"),
    ("fq-codel", "FQ-CoDel", "#4878a8"),
]
BULK_START_S = 5.0
OFFERED_MBPS = 2.0
COLOR_NS3 = "#e08a3c"
COLOR_LINUX = "#4a9a5a"


def main() -> int:
    lat = pd.read_csv(OUT_DIR / "ns3-latency.csv")
    gl_path = OUT_DIR / "ns3-goodput-loss.csv"
    lin_path = OUT_DIR / "linux-goodput-loss.csv"
    gl = pd.read_csv(gl_path) if gl_path.exists() else pd.DataFrame()
    lin = pd.read_csv(lin_path) if lin_path.exists() else pd.DataFrame()
    if lat.empty:
        print("cake-fig5-plot: ns3-latency.csv is empty", file=sys.stderr)
        return 1

    fig, (ax0, ax1) = plt.subplots(1, 2, figsize=(13, 5),
                                   gridspec_kw={"width_ratios": [1.7, 1.3]})

    # Panel (a): ns-3 latency over time, median across seeds per time bucket.
    plotted_any = False
    for arm, label, color in ARMS:
        adf = lat[lat["arm"] == arm]
        if adf.empty:
            continue
        g = adf.groupby("t_s")["induced_owd_ms"].median().sort_index()
        if g.empty:
            continue
        ax0.plot(g.index.to_numpy(), g.to_numpy(), label=label, color=color, linewidth=1.4)
        plotted_any = True
    if not plotted_any:
        print("cake-fig5-plot: no per-arm latency series (empty trace?)", file=sys.stderr)
        return 1
    ax0.axvline(BULK_START_S, color="grey", linestyle="--", linewidth=0.8,
                label="bulk flows start")
    ax0.set_xlabel("Time (s)")
    ax0.set_ylabel("Induced one-way delay (ms)")
    ax0.set_title("(a) Latency over time (ns-3)")
    ax0.grid(True, alpha=0.3)
    ax0.legend(loc="upper right", fontsize=8)

    # Panel (b): EF goodput (of 2 Mbit/s offered), ns-3 vs Linux, loss annotated.
    arm_keys = [a[0] for a in ARMS]
    arm_short = [a[1].split(" (")[0] for a in ARMS]
    have_linux = not lin.empty
    x = np.arange(len(ARMS))
    width = 0.36 if have_linux else 0.6

    ns3_gp = []
    ns3_loss = []
    for arm, _, _ in ARMS:
        a = gl[gl["arm"] == arm] if not gl.empty else pd.DataFrame()
        ns3_gp.append(float(a["ef_goodput_mbps"].median()) if not a.empty else np.nan)
        ns3_loss.append(float(a["ef_loss_pct"].median()) if not a.empty else np.nan)

    if have_linux:
        lin_gp = []
        lin_loss = []
        for arm, _, _ in ARMS:
            a = lin[lin["arm"] == arm]
            lin_gp.append(float(a["ef_goodput_mbps"].iloc[0]) if not a.empty else np.nan)
            lin_loss.append(float(a["ef_loss_pct"].iloc[0]) if not a.empty else np.nan)
        b1 = ax1.bar(x - width / 2, ns3_gp, width, label="ns-3", color=COLOR_NS3)
        b2 = ax1.bar(x + width / 2, lin_gp, width, label="Linux sch_cake", color=COLOR_LINUX)
        for bar, loss in zip(b1, ns3_loss):
            ax1.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.03,
                     f"{loss:.0f}%", ha="center", va="bottom", fontsize=8)
        for bar, loss in zip(b2, lin_loss):
            ax1.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.03,
                     f"{loss:.0f}%", ha="center", va="bottom", fontsize=8)
    else:
        bars = ax1.bar(x, ns3_gp, width, color=[a[2] for a in ARMS])
        for bar, loss in zip(bars, ns3_loss):
            ax1.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 0.03,
                     f"{loss:.0f}% loss", ha="center", va="bottom", fontsize=9)

    ax1.axhline(OFFERED_MBPS, color="black", linestyle=":", linewidth=1.0,
                label=f"offered ({OFFERED_MBPS:.0f} Mbit/s)")
    ax1.set_xticks(x)
    ax1.set_xticklabels(arm_short, rotation=20, ha="right", fontsize=8)
    ax1.set_ylabel("EF flow goodput (Mbit/s)")
    ax1.set_ylim(0, OFFERED_MBPS * 1.25)
    ax1.set_title("(b) Was the flow served?  (loss % labelled)")
    ax1.grid(True, axis="y", alpha=0.3)
    ax1.legend(loc="upper right", fontsize=8)

    suptitle = ("CAKE Fig-5: a 2 Mbit/s EF fixed-rate flow vs 32 bulk flows on 10 Mbit/s"
                + ("" if have_linux else "  [Linux: pending]"))
    fig.suptitle(suptitle, fontsize=12)
    fig.tight_layout(rect=[0, 0, 1, 0.96])

    for d in (OUT_DIR, PAPER_FIG_DIR, HANDBOOK_FIG_DIR):
        d.mkdir(parents=True, exist_ok=True)
        for ext in ("svg", "pdf"):
            fig.savefig(d / f"cake-fig5.{ext}", bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote cake-fig5.{{svg,pdf}} to {OUT_DIR}, {PAPER_FIG_DIR}, {HANDBOOK_FIG_DIR} "
          f"(linux={'yes' if have_linux else 'pending'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
