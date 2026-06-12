#!/usr/bin/env python3
"""Render the RFC 7928 Section 6 RTT-fairness figure from an rtt-sweep tree.

Reads the per-cell summary files written by `aqm-eval rtt-sweep`
(`<inDir>/<aqm>/rtt<R>/r<seed>/tcp-friendly-<tag>-summary.txt`) and renders
a two-panel figure:

  Panel A — category-II/category-I goodput ratio vs category-II RTT
            (log x; the RFC 7928 Section 6.3 headline metric).  Category I
            holds RTT 100 ms; ratio 1.0 marks perfect RTT fairness.
  Panel B — category-II packet drop rate vs category-II RTT (the second
            mandatory Section 6.3 output).

Per (AQM, RTT) point the value plotted is the median across seeds.  The
script fails (exit 2) if any expected (AQM, RTT) cell has no parsed data —
a silently missing cell must never render as an empty line.

Output: <outDir>/rtt-fairness.{png,pdf}
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aqm_manifest

# Colour palette mirrors ellipse-plot.py (keyed by registry fileTag).
AQM_COLORS = {
    "PfifoFast":        "#000000",
    "Red":              "#cc6677",
    "AdaptiveRed":      "#882255",
    "CoDel":            "#ddcc77",
    "FqCoDel":          "#999933",
    "Pie":              "#117733",
    "FqPie":            "#44aa99",
    "Cobalt":           "#332288",
    "FqCobalt":         "#88ccee",
    "StratumRed":            "#aa4499",
    "StratumL4sWred":        "#dd77aa",
    "StratumL4sCoupledOnly": "#cc8844",
    "StratumCake":           "#0077bb",
}

AQM_ORDER = [
    "PfifoFast", "Red", "AdaptiveRed",
    "CoDel", "FqCoDel",
    "Pie", "FqPie",
    "Cobalt", "FqCobalt",
    "StratumRed", "StratumL4sWred", "StratumL4sCoupledOnly", "StratumCake",
]

AQM_SHORT_LABELS = {
    "StratumL4sCoupledOnly": "StratumL4s/Coupled",
    "StratumL4sWred":        "StratumL4s/Wred",
}


def parse_summary(path: Path) -> dict:
    rec: dict = {}
    kv_re = re.compile(r"^([A-Za-z_][\w]*)=(.*)$")
    for raw in path.read_text().splitlines():
        m = kv_re.match(raw.strip())
        if m:
            rec[m.group(1)] = m.group(2)
    return rec


def collect(in_dir: Path) -> dict:
    """-> {fileTag: {rtt_ms: {"ratio": [..], "drop2": [..]}}}"""
    name_to_tag = {e["name"]: e["fileTag"] for e in aqm_manifest.entries()}
    data: dict = defaultdict(lambda: defaultdict(lambda: {"ratio": [], "drop2": []}))
    for f in sorted(in_dir.glob("**/tcp-friendly-*-summary.txt")):
        rec = parse_summary(f)
        if "goodput_ratio_cat2_over_cat1" not in rec:
            continue  # not an rtt-fairness cell
        tag = name_to_tag.get(rec.get("aqm", ""), rec.get("aqm", "?"))
        rtt = float(rec["rtt_cat2_ms"])
        data[tag][rtt]["ratio"].append(float(rec["goodput_ratio_cat2_over_cat1"]))
        data[tag][rtt]["drop2"].append(float(rec.get("drop_rate_cat2", "0")))
    return data


def main(argv=None) -> int:
    repo_root = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--inDir", type=Path,
                    default=repo_root / "output" / "aqm-eval" / "rtt-sweep")
    ap.add_argument("--outDir", type=Path,
                    default=repo_root / "output" / "aqm-eval")
    args = ap.parse_args(argv)

    data = collect(args.inDir)
    if not data:
        print(f"ERROR: parsed no rtt-fairness cells from {args.inDir}", file=sys.stderr)
        return 2

    # Completeness gate: every AQM must cover the same RTT grid, with data.
    grids = {tag: sorted(pts.keys()) for tag, pts in data.items()}
    grid = sorted({r for pts in grids.values() for r in pts})
    missing = []
    for tag, pts in data.items():
        for r in grid:
            if r not in pts or not pts[r]["ratio"]:
                missing.append(f"{tag} @ rtt2={r}ms")
    if missing:
        print("ERROR: missing rtt-fairness cells (refusing to render):",
              file=sys.stderr)
        for m in missing:
            print("  -", m, file=sys.stderr)
        return 2

    tags = [t for t in AQM_ORDER if t in data] + sorted(set(data) - set(AQM_ORDER))
    fig, (ax_a, ax_b) = plt.subplots(1, 2, figsize=(11.5, 4.6))

    for tag in tags:
        xs = sorted(data[tag].keys())
        med_ratio = [statistics.median(data[tag][r]["ratio"]) for r in xs]
        med_drop2 = [statistics.median(data[tag][r]["drop2"]) for r in xs]
        label = AQM_SHORT_LABELS.get(tag, tag)
        colour = AQM_COLORS.get(tag, "#777777")
        ax_a.plot(xs, med_ratio, marker="o", ms=3, lw=1.2, color=colour, label=label)
        ax_b.plot(xs, med_drop2, marker="o", ms=3, lw=1.2, color=colour, label=label)

    for ax in (ax_a, ax_b):
        ax.set_xscale("log")
        ax.set_xlabel("Category-II RTT (ms; category I fixed at 100 ms)")
        ax.axvline(100.0, color="#bbbbbb", lw=0.8, ls=":")
        ax.grid(True, which="both", lw=0.3, alpha=0.4)

    ax_a.axhline(1.0, color="#bbbbbb", lw=0.8, ls="--")
    ax_a.set_yscale("log")
    ax_a.set_ylabel("Goodput ratio (category II / category I, log)")
    ax_a.set_title("(A) RTT fairness — goodput ratio")
    ax_b.set_ylabel("Category-II packet drop rate")
    ax_b.set_title("(B) Category-II drop rate")
    ax_b.set_yscale("symlog", linthresh=1e-4)

    ax_a.legend(fontsize=6.5, ncol=2, loc="upper right", framealpha=0.9)

    seeds = len(next(iter(data[tags[0]].values()))["ratio"])
    fig.suptitle(
        f"RTT fairness across the AQM catalogue "
        f"(RFC 7928 Section 6; median of {seeds} seeds per point)",
        fontsize=10,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.94))

    args.outDir.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        out = args.outDir / f"rtt-fairness.{ext}"
        fig.savefig(out, dpi=180)
        print("wrote", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
