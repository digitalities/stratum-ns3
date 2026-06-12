#!/usr/bin/env python3
"""AQM characterisation ellipse plot — RFC 7928 signature figure.

Reads the per-cell summary.txt files written by `aqm-eval-runner`,
groups them by AQM, and produces two-panel ellipse plots:

  Panel A — Goodput vs Jain fairness envelope (1 sigma)
            One ellipse per AQM over the 9 scenarios.  Standard
            RFC 7928 trade-off visualisation.

  Panel B — Goodput vs TCP retransmission rate (1 sigma)
            One ellipse per AQM over the 3 TCP scenarios.
            Shows tag-aware retx accounting in operation
            (MR !2830 demonstration).

By default consumes `output/aqm-eval/day2-matrix/`.

If only the legacy alpha-lite single-point smoke is available
(`output/ns3/aqm-comparison-smoke.txt`), falls back to the
3-AQM-only scatter version.

Backward-compat note: the schema changed between alpha-lite and
beta-prime.  Old summaries had `disc=` keys; new ones use `aqm=`.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

import aqm_manifest

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INPUT_DIR = REPO_ROOT / "output" / "aqm-eval" / "day2-matrix"
DEFAULT_LEGACY_INPUT = REPO_ROOT / "output" / "ns3" / "aqm-comparison-smoke.txt"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "output" / "aqm-eval"

# Family + marker maps come from the C++ registry via the JSON manifest
# emitted by `aqm-eval-runner --manifest=PATH`.  Editorial choices
# (palette, legend order, short labels) stay below.
AQM_FAMILIES = aqm_manifest.families()
AQM_MARKERS = aqm_manifest.markers_by_family()

# 1-sigma ellipse for a 2D bivariate normal: covers ~39% of probability mass.
CHI2_1SIGMA_2DOF = 2.30

# Colour-blind-safe palette derived from Tol's "muted" + Okabe-Ito.
# Family is also encoded via marker shape (AQM_MARKERS), so colour
# differentiation supplements rather than carries the family signal.
# B/W-print sanity: each pair of AQMs differs in shape OR luminance.
AQM_COLORS = {
    "PfifoFast":        "#000000",  # black
    "Red":              "#cc6677",  # rose
    "AdaptiveRed":      "#882255",  # wine
    "CoDel":            "#ddcc77",  # sand
    "FqCoDel":          "#999933",  # olive
    "Pie":              "#117733",  # green
    "FqPie":            "#44aa99",  # teal
    "Cobalt":           "#332288",  # indigo
    "FqCobalt":         "#88ccee",  # light cyan
    "StratumRed":            "#aa4499",  # purple
    "StratumL4sWred":        "#dd77aa",  # mauve
    "StratumL4sCoupledOnly": "#cc8844",  # ochre
    "StratumCake":           "#0077bb",  # bright blue
}

# Family band colour (used for matching y-tick label colour in heatmap;
# also exported for any future per-family aggregation overlays).
FAMILY_COLOURS = {
    "single":  "#444444",  # dark grey
    "fq":      "#117733",  # green
    "stratum": "#882255",  # wine
    "ds4":     "#882255",  # legacy family token in pre-rename recorded manifests
}
# Order in legend (mainline AQMs first, Stratum last).
AQM_ORDER = [
    "PfifoFast", "Red", "AdaptiveRed",
    "CoDel", "FqCoDel",
    "Pie", "FqPie",
    "Cobalt", "FqCobalt",
    "StratumRed", "StratumL4sWred", "StratumL4sCoupledOnly", "StratumCake",
]

# Short row labels for Panel C (avoid clipping in the y-tick column).
AQM_SHORT_LABELS = {
    "StratumL4sCoupledOnly": "StratumL4s/Coupled",
    "StratumL4sWred":        "StratumL4s/Wred",
}


def parse_summary(path: Path) -> dict:
    """Parse one `<scenario>-<aqm>-summary.txt` written by aqm-eval-runner."""
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


def aqm_short_name(full: str) -> str:
    """Map runner's AQM key to the short label used in colour table."""
    if full.startswith("ns3::"):
        full = full[len("ns3::"):]
    if full.endswith("QueueDisc"):
        full = full[: -len("QueueDisc")]
    return full


def load_matrix(in_dir: Path) -> dict[str, list[dict]]:
    """Group summary records by AQM (short-name)."""
    grouped: dict[str, list[dict]] = defaultdict(list)
    for f in sorted(in_dir.glob("*-summary.txt")):
        rec = parse_summary(f)
        aqm_full = rec.get("aqm", "")
        if not aqm_full:
            continue
        rec["aqm_short"] = aqm_short_name(str(aqm_full))
        # Convenience numeric: retx rate as fraction.
        fm_orig = rec.get("fm_orig_bytes", 0)
        fm_retx = rec.get("fm_retx_bytes", 0)
        rec["retx_rate"] = (fm_retx / (fm_orig + fm_retx)) if (fm_orig + fm_retx) > 0 else 0.0
        grouped[rec["aqm_short"]].append(rec)
    return grouped


def confidence_ellipse(ax, xs, ys, *, color: str, alpha: float = 0.10, edge_alpha: float = 0.6) -> None:
    """1-sigma ellipse for the (xs, ys) sample drawn on `ax`."""
    if len(xs) < 2:
        return
    xs = np.asarray(xs, dtype=float)
    ys = np.asarray(ys, dtype=float)
    if np.allclose(xs, xs[0]) and np.allclose(ys, ys[0]):
        return
    mean = np.array([xs.mean(), ys.mean()])
    cov = np.cov(np.stack([xs, ys]))
    if not np.all(np.isfinite(cov)):
        return
    eigvals, eigvecs = np.linalg.eigh(cov)
    order = eigvals.argsort()[::-1]
    eigvals = eigvals[order]
    eigvecs = eigvecs[:, order]
    if eigvals[0] <= 0:
        return
    theta = np.linspace(0.0, 2.0 * np.pi, 120)
    rx = math.sqrt(CHI2_1SIGMA_2DOF * max(eigvals[0], 0.0))
    ry = math.sqrt(CHI2_1SIGMA_2DOF * max(eigvals[1], 0.0))
    pts = np.stack([rx * np.cos(theta), ry * np.sin(theta)])
    pts = eigvecs @ pts
    pts[0] += mean[0]
    pts[1] += mean[1]
    ax.fill(pts[0], pts[1], color=color, alpha=alpha, zorder=1)
    ax.plot(pts[0], pts[1], color=color, alpha=edge_alpha, linewidth=1.2, zorder=2)


# Inline-label semantics: a label denotes a CURRENT, OPEN anomaly
# worth the reader's attention.  AQMs that have been investigated
# and classified as characterisation findings (F-C RNG-bistable PI,
# F-D Cobalt sojourn-vs-DRR, A3-StratumCake hash placement) are NOT
# labelled — they live in the legend like every other AQM, and
# the Panel C bar chart surfaces them by bar length.  After the
# 2026-04-28 F-C reframe no open implementation defect remains;
# both dictionaries are empty.
PANEL_A_LABEL_OFFSETS: dict = {}
PANEL_B_LABEL_OFFSETS: dict = {}

# Marker edge style options (CLI-selectable for A/B comparison).
EDGE_STYLES = {
    "thin-black": {"edgecolors": "black", "linewidths": 0.4},
    "soft-gray":  {"edgecolors": "dimgray", "linewidths": 0.3},
    "none":       {"edgecolors": "none", "linewidths": 0.0},
}
EDGE_STYLE = EDGE_STYLES["soft-gray"]  # default; overridable via --edges


def plot_jain_vs_goodput(ax, grouped: dict[str, list[dict]]) -> None:
    """Panel A: aggregate goodput vs Jain fairness, one ellipse per AQM.

    13 ellipses are unavoidably busy in the central cluster — family
    marker shapes (●/▲/■) plus the Tol-muted colour-blind-safe palette
    keep them disambiguable.  Panel C carries the actual fairness
    findings; Panel A documents the envelope.
    """
    for aqm in AQM_ORDER:
        if aqm not in grouped:
            continue
        recs = grouped[aqm]
        xs = [r["jain_fairness"] for r in recs if "jain_fairness" in r]
        ys = [r["aggregate_Mbps"] for r in recs if "aggregate_Mbps" in r]
        if not xs:
            continue
        c = AQM_COLORS.get(aqm, "black")
        m = AQM_MARKERS.get(aqm, "o")
        confidence_ellipse(ax, xs, ys, color=c)
        ax.scatter(xs, ys, s=42, c=c, marker=m, **EDGE_STYLE, zorder=3, label=aqm)
        if aqm in PANEL_A_LABEL_OFFSETS:
            ax.annotate(
                aqm,
                (float(np.mean(xs)), float(np.mean(ys))),
                textcoords="offset points",
                xytext=PANEL_A_LABEL_OFFSETS[aqm],
                fontsize=8.5,
                color=c,
                fontweight="bold",
                zorder=5,
                bbox=dict(boxstyle="round,pad=0.18", fc="white", ec=c, lw=0.6, alpha=0.92),
                arrowprops=dict(arrowstyle="-", color=c, alpha=0.5, lw=0.6),
            )
    ax.set_xlabel("Jain fairness index")
    ax.set_ylabel("Aggregate goodput (Mbps)")
    ax.set_title("Panel A — Fairness vs goodput envelope (1σ)\n9 scenarios × 13 AQMs (StratumL4s in 2 modes)")
    ax.set_xlim(0.55, 1.05)
    ax.set_ylim(6.5, 10.0)
    ax.grid(True, alpha=0.3)


def plot_retx_vs_goodput(ax, grouped: dict[str, list[dict]]) -> None:
    """Panel B: tag-aware retx rate vs aggregate goodput, ellipse over TCP scenarios."""
    for aqm in AQM_ORDER:
        if aqm not in grouped:
            continue
        recs = [
            r
            for r in grouped[aqm]
            if isinstance(r.get("scenario"), str) and r["scenario"].startswith("tcp-")
        ]
        if not recs:
            continue
        xs = [r["retx_rate"] * 100.0 for r in recs]
        ys = [r["aggregate_Mbps"] for r in recs]
        c = AQM_COLORS.get(aqm, "black")
        m = AQM_MARKERS.get(aqm, "o")
        confidence_ellipse(ax, xs, ys, color=c)
        ax.scatter(xs, ys, s=42, c=c, marker=m, **EDGE_STYLE, zorder=3)
        if aqm in PANEL_B_LABEL_OFFSETS:
            ax.annotate(
                aqm,
                (float(np.mean(xs)), float(np.mean(ys))),
                textcoords="offset points",
                xytext=PANEL_B_LABEL_OFFSETS[aqm],
                fontsize=8.5,
                color=c,
                fontweight="bold",
                zorder=5,
                bbox=dict(boxstyle="round,pad=0.18", fc="white", ec=c, lw=0.6, alpha=0.92),
                arrowprops=dict(arrowstyle="-", color=c, alpha=0.5, lw=0.6),
            )
    ax.set_xlabel("TCP retransmission rate (%)")
    ax.set_ylabel("Aggregate goodput (Mbps)")
    ax.set_title("Panel B — Retx vs goodput envelope (1σ)\n3 TCP scenarios × 13 rows (retx-aware via TcpRetransmitTag)")
    ax.grid(True, alpha=0.3)
    ax.set_xlim(-0.05, None)


def plot_jain_range(ax, grouped: dict[str, list[dict]]) -> None:
    """Panel C: per-AQM Jain min/max range over TCP scenarios.

    Restricted to the 3 TCP scenarios (tcp-friendly, tcp-aggressive,
    tcp-unresponsive) where flows submit equal greedy demand — so
    Jain on goodput isolates AQM-induced fairness rather than the
    deliberate input-rate imbalance of the UDP scenarios.

    Refuses to average across scenarios — exposes per-cell anomalies
    that the Panel A ellipse fitting smooths out.  F-C (FqPie
    tcp-friendly Jain 0.74) towers visibly below the FqCoDel/FqCobalt
    near-1.0 dots.
    """
    rows: list[tuple[str, float, float, float]] = []
    for aqm in AQM_ORDER:
        if aqm not in grouped:
            continue
        js = [
            r["jain_fairness"]
            for r in grouped[aqm]
            if "jain_fairness" in r
            and isinstance(r.get("scenario"), str)
            and r["scenario"].startswith("tcp-")
        ]
        if not js:
            continue
        rows.append((aqm, float(min(js)), float(max(js)), float(np.mean(js))))
    rows.sort(key=lambda t: t[1])  # worst min first → top of plot
    y = list(range(len(rows)))
    for i, (aqm, jmin, jmax, jmean) in enumerate(rows):
        c = AQM_COLORS.get(aqm, "black")
        ax.hlines(i, jmin, jmax, colors=c, linewidth=4.5, alpha=0.75, zorder=2)
        ax.plot([jmin, jmax], [i, i], "o", color=c, markersize=5,
                markeredgecolor="dimgray", markeredgewidth=0.3, zorder=3)
        # White halo under the black tick so the mean is visible against
        # saturated bar fills (item 13 in the audit).
        ax.plot(jmean, i, "|", color="white", markersize=11,
                markeredgewidth=3.2, zorder=4)
        ax.plot(jmean, i, "|", color="black", markersize=10,
                markeredgewidth=1.8, zorder=5)
    ax.set_yticks(y)
    ax.set_yticklabels(
        [AQM_SHORT_LABELS.get(r[0], r[0]) for r in rows],
        fontsize=8.5,
    )
    ax.invert_yaxis()
    ax.axvline(1.0, color="lightgray", linestyle=":", linewidth=0.8, zorder=1)
    ax.set_xlabel("Jain fairness — TCP scenarios (min … max)")
    ax.set_title("Panel C — Fairness range over 3 TCP scenarios\nbar = min..max; tick = mean (equal-demand isolates AQM)")
    ax.set_xlim(0.55, 1.05)
    ax.grid(True, axis="x", alpha=0.3)


def build_figure_legend(fig) -> None:
    """Single-row legend at figure bottom (13 entries), encoding family via marker shape."""
    handles = []
    edge_kwargs = {}
    if EDGE_STYLE.get("edgecolors") and EDGE_STYLE["edgecolors"] != "none":
        edge_kwargs = {"markeredgecolor": EDGE_STYLE["edgecolors"]}
    for aqm in AQM_ORDER:
        c = AQM_COLORS.get(aqm, "black")
        m = AQM_MARKERS.get(aqm, "o")
        handles.append(
            plt.Line2D(
                [0], [0], marker=m, color="w",
                markerfacecolor=c, markersize=8,
                label=AQM_SHORT_LABELS.get(aqm, aqm), **edge_kwargs,
            )
        )
    fig.legend(
        handles=handles,
        loc="lower center",
        ncol=13,           # all 13 on one row; matches AQM_ORDER count
        fontsize=7.5,
        framealpha=0.0,
        bbox_to_anchor=(0.5, 0.0),
        columnspacing=0.9,
        handletextpad=0.3,
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=DEFAULT_INPUT_DIR,
        help=f"Matrix directory written by aqm-eval-runner (default: {DEFAULT_INPUT_DIR.relative_to(REPO_ROOT)})",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
    )
    parser.add_argument("--no-pdf", action="store_true")
    parser.add_argument(
        "--edges",
        choices=list(EDGE_STYLES.keys()),
        default="soft-gray",
        help="Marker-edge style: thin-black, soft-gray (default), or none",
    )
    parser.add_argument(
        "--suffix",
        default="",
        help="Suffix appended to output filename (qdel-goodput<suffix>.png)",
    )
    args = parser.parse_args(argv)
    global EDGE_STYLE
    EDGE_STYLE = EDGE_STYLES[args.edges]

    if not args.input_dir.exists() or not any(args.input_dir.glob("*-summary.txt")):
        print(f"ERROR: no summary files in {args.input_dir}", file=sys.stderr)
        return 2

    grouped = load_matrix(args.input_dir)
    if not grouped:
        print(f"ERROR: parsed no AQMs from {args.input_dir}", file=sys.stderr)
        return 3

    args.output_dir.mkdir(parents=True, exist_ok=True)

    fig, (ax_a, ax_b, ax_c) = plt.subplots(1, 3, figsize=(20, 5.0))
    plot_jain_vs_goodput(ax_a, grouped)
    plot_retx_vs_goodput(ax_b, grouped)
    plot_jain_range(ax_c, grouped)
    build_figure_legend(fig)
    n_rows = len(grouped)
    n_cells = sum(len(v) for v in grouped.values())
    n_scen = n_cells // n_rows if n_rows else 0
    # n_rows=13 in the standard run; 12 distinct AQMs because StratumL4s
    # is evaluated in two ClassicAqm modes (Wred + CoupledOnly).
    distinct_aqms = max(n_rows - 1, 1) if any(
        a.startswith("StratumL4s") for a in grouped
    ) else n_rows
    fig.suptitle(
        f"AQM characterisation — {n_rows} rows × {n_scen} scenarios "
        f"= {n_cells} cells ({distinct_aqms} AQMs; StratumL4s in 2 modes)",
        y=0.99,
        fontsize=12,
    )
    fig.tight_layout(rect=(0, 0.08, 1, 0.94))

    def _show(p: Path) -> str:
        try:
            return str(p.relative_to(REPO_ROOT))
        except ValueError:
            return str(p)

    png_path = args.output_dir / f"qdel-goodput{args.suffix}.png"
    fig.savefig(png_path, dpi=150)
    print(f"wrote {_show(png_path)}")
    if not args.no_pdf:
        pdf_path = args.output_dir / f"qdel-goodput{args.suffix}.pdf"
        fig.savefig(pdf_path)
        print(f"wrote {_show(pdf_path)}")
    plt.close(fig)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
