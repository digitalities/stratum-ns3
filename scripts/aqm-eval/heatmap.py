#!/usr/bin/env python3
"""Full-matrix companion view: AQM x scenario heatmaps.

Companion to `ellipse-plot.py`.  The ellipse plot summarises across
scenarios; this script refuses to summarise and shows every cell of
the (AQM, scenario) matrix individually so the cell-level patterns
the cluster summaries smooth out are visible at a glance.

Two panels:

  Panel A -- Jain fairness index, AQM rows x scenario columns.
             Surfaces F-C (FqPie tcp-friendly), F-D (FqCobalt
             tcp-unresponsive), A3-StratumCake (StratumCake tcp-friendly), and
             the heavy-congestion late-flow starvation that the
             ellipse panels smooth into the AQM cluster mean.

  Panel B -- Aggregate goodput (Mbps), same layout.  Confirms link-
             utilisation parity across AQMs and scenarios.

Reads the per-cell summary.txt files from `output/aqm-eval/day2-matrix/`
and writes `output/aqm-eval/full-matrix.{png,pdf}`.

Intended as a survey-style appendix figure, not part of the paper
figure budget.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import numpy as np

import aqm_manifest

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INPUT_DIR = REPO_ROOT / "output" / "aqm-eval" / "day2-matrix"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "output" / "aqm-eval"

# Family map comes from the C++ registry via the JSON manifest emitted by
# `aqm-eval-runner --manifest=PATH`.  Row order is editorial (worst-min-Jain
# at top within each family group) and stays below.
AQM_FAMILIES = aqm_manifest.families()

# Row order: vanilla single-queue, vanilla FQ-class, Stratum.
# Within each family group, AQMs are sorted by ascending min-Jain
# over the 3 equal-demand TCP scenarios -- worst-case-first within
# the group.  This keeps the categorical family structure (so the
# reader can read "rows 7-9 are FQ-class") while putting outliers
# at the top of each block where the eye lands first.
#
# StratumL4s appears as TWO rows: StratumL4sWred (WRED early-drop classic
# queue, most representative for unmarked TCP) and StratumL4sCoupledOnly
# (canonical DualPI2 with coupled p_C as sole AQM, FIFO classic
# queue under unmarked traffic per RFC 9332 §A.3).  Showing both
# demonstrates that DualPI2's observable fairness depends on its
# classic-AQM mode, not just on traffic mix.
AQM_ROWS = [
    # Single-queue mainline (6) -- worst min-TCP-Jain at top
    "Red", "AdaptiveRed", "Pie", "CoDel", "Cobalt", "PfifoFast",
    # FQ-class mainline (3) -- F-D / F-C surfaced first
    "FqCobalt", "FqPie", "FqCoDel",
    # Stratum (4) -- StratumRed at top, two StratumL4s modes adjacent, StratumCake at bottom
    "StratumRed", "StratumL4sCoupledOnly", "StratumL4sWred", "StratumCake",
]
AQM_GROUP_BREAKS = [6, 9]  # horizontal lines after row indices 5 and 8

# Short y-tick labels (parity with ellipse-plot.py Panel C) — item 21.
AQM_SHORT_LABELS = {
    "StratumL4sCoupledOnly": "StratumL4s/Coupled",
    "StratumL4sWred":        "StratumL4s/Wred",
}

# Family colour for y-tick label tinting (item 22).
FAMILY_COLOURS = {
    "single":  "#444444",
    "fq":      "#117733",
    "stratum": "#882255",
    "ds4":     "#882255",  # legacy family token in pre-rename recorded manifests
}
# Column order: scenarios within each (UDP, TCP) group are sorted
# left-to-right by AQM-stress (loosely "how much the scenario
# differentiates AQMs"), so the eye traces fairness degradation
# rightward.  UDP block ends with heavy-congestion (the strongest
# AQM-differentiator among UDP); TCP block ends with tcp-unresponsive
# (the strongest among TCP).  Adjacent boundary cells
# (heavy-congestion | tcp-friendly) reveal that "StratumL4s heavy 0.53"
# and "FqPie tcp-friendly 0.74" share surface signatures despite
# root-cause-different mechanisms (RFC 9332 §A.3 vs F-C).
SCENARIO_COLS = [
    # UDP scenarios (6) -- low-stress to high-stress
    "mild-congestion", "mixed", "steady",
    "rt-bulk", "medium-congestion", "heavy-congestion",
    # TCP scenarios (3) -- equal-demand to deliberately stressed
    "tcp-friendly", "tcp-aggressive", "tcp-unresponsive",
]
SCENARIO_GROUP_BREAKS = [6]  # vertical line after column index 5


def aqm_short_name(full: str) -> str:
    if full.startswith("ns3::"):
        full = full[len("ns3::"):]
    if full.endswith("QueueDisc"):
        full = full[: -len("QueueDisc")]
    return full


def parse_summary(path: Path) -> dict:
    rec: dict = {}
    kv_re = re.compile(r"^([A-Za-z_][\w]*)=(.*)$")
    for raw in path.read_text().splitlines():
        m = kv_re.match(raw.strip())
        if not m:
            continue
        key, val = m.group(1), m.group(2).strip()
        try:
            rec[key] = float(val) if "." in val or "e" in val.lower() else int(val)
        except ValueError:
            rec[key] = val
    return rec


def load_matrix(in_dir: Path) -> dict[tuple[str, str], dict]:
    cells: dict[tuple[str, str], dict] = {}
    for f in sorted(in_dir.glob("*-summary.txt")):
        rec = parse_summary(f)
        scenario = str(rec.get("scenario", ""))
        aqm = aqm_short_name(str(rec.get("aqm", "")))
        if not scenario or not aqm:
            continue
        cells[(scenario, aqm)] = rec
    return cells


def build_grid(cells, key: str) -> np.ndarray:
    """Return a 2D array shaped (len(AQM_ROWS), len(SCENARIO_COLS))."""
    grid = np.full((len(AQM_ROWS), len(SCENARIO_COLS)), np.nan)
    for i, aqm in enumerate(AQM_ROWS):
        for j, sc in enumerate(SCENARIO_COLS):
            rec = cells.get((sc, aqm))
            if rec and key in rec:
                grid[i, j] = float(rec[key])
    return grid


def draw_heatmap(ax, grid, *, vmin, vmax, cmap, title, cbar_label, fmt,
                 norm=None, alarm_below: float | None = None,
                 alarm_relative_only: bool = False):
    """One heatmap subplot with cell-value annotations and group dividers.

    `norm` overrides linear vmin/vmax mapping (used by Jain panel for a
    perceptually-anchored TwoSlopeNorm centred at 0.85, the
    "good fairness" threshold; item 15 in the audit).

    `alarm_below` bolds cells whose value is below the threshold (item
    17): catches StratumRed/DsL4sCoupled heavy-congestion 0.43/0.53,
    StratumL4s/Wred tcp-aggressive 0.78, FqPie tcp-friendly 0.74.

    `alarm_relative_only` (item 23, post-audit-2): when set, bold fires
    only if the cell is below `alarm_below` AND strictly below its
    column median.  This suppresses false positives in scenario-floor
    columns (mild-congestion, mixed) where every AQM is byte-identical
    because the UDP/CBR load profile, not the AQM, dominates the
    fairness number — e.g. Jain=0.708 across all 13 rows in
    mild-congestion is a load-shape artefact, not an AQM defect.
    """
    masked = np.ma.masked_invalid(grid)
    if norm is not None:
        im = ax.imshow(masked, cmap=cmap, norm=norm, aspect="auto")
    else:
        im = ax.imshow(masked, cmap=cmap, vmin=vmin, vmax=vmax, aspect="auto")

    ax.set_xticks(range(len(SCENARIO_COLS)))
    ax.set_xticklabels(SCENARIO_COLS, rotation=35, ha="right", fontsize=8)
    # Item 24: italicise scenario-floor columns where every AQM is
    # byte-identical (load profile dominates).  Audit-2 finding:
    # mild-congestion (Jain=0.708 ×13) and mixed (0.586 ×13) are
    # load-shape artefacts, not AQM-driven outcomes.
    floor_cols = {j for j, sc in enumerate(SCENARIO_COLS)
                  if sc in {"mild-congestion", "mixed"}}
    for j, lbl in enumerate(ax.get_xticklabels()):
        if j in floor_cols:
            lbl.set_fontstyle("italic")
            lbl.set_color("#666666")
    ax.set_yticks(range(len(AQM_ROWS)))
    # Short labels + family-coloured tint (items 21, 22).
    display_labels = [AQM_SHORT_LABELS.get(a, a) for a in AQM_ROWS]
    ax.set_yticklabels(display_labels, fontsize=8.5)
    for tick, aqm in zip(ax.get_yticklabels(), AQM_ROWS):
        fam = AQM_FAMILIES.get(aqm, "single")
        tick.set_color(FAMILY_COLOURS.get(fam, "black"))

    # Numeric annotation per cell; auto-contrast text colour against
    # cell shade.  Cells below `alarm_below` render bold + larger.
    norm_for_text = norm if norm is not None else mcolors.Normalize(vmin=vmin, vmax=vmax)
    col_medians = np.nanmedian(grid, axis=0)
    for i in range(grid.shape[0]):
        for j in range(grid.shape[1]):
            val = grid[i, j]
            if np.isnan(val):
                continue
            shade = float(norm_for_text(val))
            # luminance heuristic: dark cells get white text
            txt_col = "white" if shade < 0.32 else "black"
            is_alarm = alarm_below is not None and val < alarm_below
            if is_alarm and alarm_relative_only:
                # Only bold when the cell is AQM-specific underperformance
                # (strictly below column median).  Suppresses false
                # positives in scenario-floor columns.
                is_alarm = val < col_medians[j] - 1e-6
            weight = "bold" if is_alarm else "normal"
            size = 7.5 if weight == "bold" else 7
            ax.text(j, i, fmt.format(val), ha="center", va="center",
                    fontsize=size, fontweight=weight, color=txt_col)

    # Group dividers — alpha bumped 0.45 → 0.7 (item 18) for stratification.
    for b in AQM_GROUP_BREAKS:
        ax.axhline(b - 0.5, color="black", linewidth=1.0, alpha=0.7)
    for b in SCENARIO_GROUP_BREAKS:
        ax.axvline(b - 0.5, color="black", linewidth=1.0, alpha=0.7)

    cbar = ax.figure.colorbar(im, ax=ax, shrink=0.85, pad=0.04)
    cbar.set_label(cbar_label, fontsize=8.5, labelpad=8)
    cbar.ax.tick_params(labelsize=7)
    ax.set_title(title, fontsize=10.5)
    ax.set_xlabel("Scenario", fontsize=9)
    if ax.get_subplotspec().colspan.start == 0:
        ax.set_ylabel("AQM (single-queue / FQ-class / Stratum)", fontsize=9)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input-dir", type=Path, default=DEFAULT_INPUT_DIR)
    ap.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    ap.add_argument("--no-pdf", action="store_true")
    args = ap.parse_args(argv)

    if not args.input_dir.exists():
        print(f"ERROR: no input dir {args.input_dir}", file=sys.stderr)
        return 2

    cells = load_matrix(args.input_dir)
    if not cells:
        print(f"ERROR: parsed no cells from {args.input_dir}", file=sys.stderr)
        return 3

    jain_grid = build_grid(cells, "jain_fairness")
    goodput_grid = build_grid(cells, "aggregate_Mbps")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    fig, (ax_j, ax_g) = plt.subplots(1, 2, figsize=(14.5, 6.0))

    # Item 15: replace diverging RdYlGn with a sequential YlGn cmap
    # anchored by TwoSlopeNorm.  Midpoint at 0.85 = the Jain
    # "near-fair" threshold, so yellow no longer mis-signals 0.725 as
    # neutral.  Below 0.7 (alarm) → bold cell text (item 17).
    jain_norm = mcolors.TwoSlopeNorm(vmin=0.45, vcenter=0.85, vmax=1.00)
    draw_heatmap(
        ax_j, jain_grid,
        vmin=0.45, vmax=1.00, cmap=plt.cm.RdYlGn,
        norm=jain_norm,
        alarm_below=0.80,
        alarm_relative_only=True,  # item 23: suppress scenario-floor false positives
        title="Panel A — Jain fairness across the 13×9 matrix\n"
              "(centre = 0.85 'near-fair'; bold = AQM-specific < 0.80;\n"
              "italic columns = scenario-floor)",
        cbar_label="Jain index",
        fmt="{:.2f}",
    )
    draw_heatmap(
        ax_g, goodput_grid,
        # Item 16: tighten vmin from 6.0 to 6.5 so the colour gradient
        # uses the actual data range (min observed = 6.7 in mild-congestion).
        vmin=6.5, vmax=10.0, cmap=plt.cm.viridis,
        title="Panel B — Aggregate goodput (Mbps), 13×9 matrix\n"
              "(10 Mbps link; values near link rate confirm utilisation)",
        cbar_label="Mbps",
        fmt="{:.1f}",
    )

    # Item 20 (optional): subtle vertical band on the heavy-congestion
    # column where the substrate's stress test surfaces the F-A/F-D
    # signatures.  Drawn at low alpha so it doesn't overwhelm the data.
    heavy_col = SCENARIO_COLS.index("heavy-congestion")
    for ax in (ax_j, ax_g):
        ax.axvspan(heavy_col - 0.5, heavy_col + 0.5,
                   facecolor="none", edgecolor="black",
                   linewidth=0.6, linestyle="--", alpha=0.55, zorder=4)

    # Item 19: include row count in suptitle for parity with the
    # ellipse-plot figure.
    fig.suptitle(
        "Full-matrix companion view — every (scenario, AQM) cell visible "
        "(13 AQMs; StratumL4s in 2 modes)",
        y=0.995, fontsize=12,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.96))

    def _show(p: Path) -> str:
        try:
            return str(p.relative_to(REPO_ROOT))
        except ValueError:
            return str(p)

    png_path = args.output_dir / "full-matrix.png"
    fig.savefig(png_path, dpi=150)
    print(f"wrote {_show(png_path)}")
    if not args.no_pdf:
        pdf_path = args.output_dir / "full-matrix.pdf"
        fig.savefig(pdf_path)
        print(f"wrote {_show(pdf_path)}")
    plt.close(fig)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
