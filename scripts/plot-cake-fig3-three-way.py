#!/usr/bin/env python3
"""Three-way CAKE Fig-3 host-isolation figure: paper-ideal / ns-3 / Linux.

Layout A: 4 panels (no-iso / source / dest / triple), x-axis = 6 flows,
three grouped bars per flow (ideal / ns-3 / Linux), error bars from seed
or replica spread. Renders to output/ns3/cake-fig3/ and paper/figures/.

Inputs:
  output/ns3/cake-fig3/ns3-perflow.csv    (mode,rng_run,flow_idx,src,dst,share)
  output/ns3/cake-fig3/linux-perflow.csv  (mode,rng_run,flow_idx,src,dst,goodput_mbps)
                                          [optional; if absent -> ideal+ns-3 only]
The paper-ideal series is hardcoded from specs/03-quality.md Q-15.12.
"""
import os
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

REPO_ROOT = Path(os.environ.get("REPO_ROOT", Path(__file__).resolve().parent.parent))
OUT_DIR = Path(os.environ.get("OUT_DIR", REPO_ROOT / "output" / "ns3" / "cake-fig3"))
PAPER_FIG_DIR = REPO_ROOT / "paper" / "figures"

MODES = ["no-iso", "source", "dest", "triple"]
FLOW_LABELS = ["A→destA", "A→destB", "A→destC", "A→destC",
               "B→destC", "B→destD"]

# Paper-ideal per-flow shares (Q-15.12 table), indexed [mode][flow_idx].
IDEAL = {
    "no-iso": [1/6, 1/6, 1/6, 1/6, 1/6, 1/6],
    "source": [0.125, 0.125, 0.125, 0.125, 0.25, 0.25],
    "dest":   [0.25, 0.25, 1/12, 1/12, 1/12, 0.25],
    "triple": [0.1364, 0.1364, 0.1364, 0.1364, 0.1818, 0.2727],
}

COLOR_IDEAL = "#4878a8"
COLOR_NS3 = "#e08a3c"
COLOR_LINUX = "#4a9a5a"


def _agg_shares(df: pd.DataFrame, value_to_share: bool) -> dict:
    """Return {mode: {flow_idx: (mean_share, std_share)}}.

    If value_to_share, the value column is goodput_mbps and is normalized to a
    per-replica share before aggregating; otherwise it is already a share."""
    out = {}
    for mode, mdf in df.groupby("mode"):
        per_flow = {f: [] for f in range(6)}
        for _rng, rdf in mdf.groupby("rng_run"):
            if value_to_share:
                tot = rdf["goodput_mbps"].sum()
                shares = {int(r.flow_idx): (r.goodput_mbps / tot if tot > 0 else 0.0)
                          for r in rdf.itertuples()}
            else:
                shares = {int(r.flow_idx): float(r.share) for r in rdf.itertuples()}
            for f in range(6):
                if f in shares:
                    per_flow[f].append(shares[f])
        out[mode] = {f: (float(np.mean(v)) if v else np.nan,
                         float(np.std(v)) if len(v) > 1 else 0.0)
                     for f, v in per_flow.items()}
    return out


def main() -> int:
    ns3_csv = OUT_DIR / "ns3-perflow.csv"
    linux_csv = OUT_DIR / "linux-perflow.csv"
    ns3 = _agg_shares(pd.read_csv(ns3_csv), value_to_share=False)
    have_linux = linux_csv.exists()
    linux = _agg_shares(pd.read_csv(linux_csv), value_to_share=True) if have_linux else {}

    x = np.arange(6)
    width = 0.27
    fig, axes = plt.subplots(1, 4, figsize=(20, 5), sharey=True)
    for ax, mode in zip(axes, MODES):
        ideal_v = IDEAL[mode]
        ns3_v = [ns3.get(mode, {}).get(f, (np.nan, 0.0))[0] for f in range(6)]
        ns3_e = [ns3.get(mode, {}).get(f, (np.nan, 0.0))[1] for f in range(6)]
        ax.bar(x - width, ideal_v, width, label="paper ideal", color=COLOR_IDEAL)
        ax.bar(x, ns3_v, width, yerr=ns3_e, capsize=3, label="pure ns-3",
               color=COLOR_NS3)
        if have_linux:
            lin_v = [linux.get(mode, {}).get(f, (np.nan, 0.0))[0] for f in range(6)]
            lin_e = [linux.get(mode, {}).get(f, (np.nan, 0.0))[1] for f in range(6)]
            ax.bar(x + width, lin_v, width, yerr=lin_e, capsize=3, label="Linux",
                   color=COLOR_LINUX)
        ax.axhline(1/6, color="grey", linestyle="--", linewidth=0.7,
                   label="per-flow-equal (1/6)")
        ax.set_xticks(x)
        ax.set_xticklabels(FLOW_LABELS, rotation=30, ha="right", fontsize=8)
        ax.set_title(mode)
        ax.grid(True, axis="y", alpha=0.3)
    axes[0].set_ylabel("per-flow goodput share")
    axes[0].legend(loc="upper left", fontsize=8)
    title = ("CAKE paper Fig-3 host isolation: paper-ideal vs pure ns-3"
             + (" vs Linux" if have_linux else "  [Linux: pending]"))
    fig.suptitle(title, fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.96])

    PAPER_FIG_DIR.mkdir(parents=True, exist_ok=True)
    for d in (OUT_DIR, PAPER_FIG_DIR):
        for ext in ("svg", "pdf"):
            fig.savefig(d / f"cake-fig3-three-way.{ext}", bbox_inches="tight")
    plt.close(fig)
    print(f"Wrote cake-fig3-three-way.{{svg,pdf}} to {OUT_DIR} and {PAPER_FIG_DIR} "
          f"(linux={'yes' if have_linux else 'pending'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
