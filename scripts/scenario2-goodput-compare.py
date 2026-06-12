#!/usr/bin/env python3
"""Cross-simulator goodput comparison from Layer 2.E + Layer 4.A sweeps.

Reads the new "Retx Statistics" block from run.log files in four sweep
trees (ns-2.35 port-based, ns-2.35 srTCM, ns-3 port-based, ns-3 srTCM)
and produces both a numeric markdown table and a per-DP bar chart of
measured goodput vs thesis Table 4.4.

Output:
  <out_dir>/goodput-comparison.md   — numeric table per set per DP
  <out_dir>/goodput-comparison.png  — 3-panel bar chart (DP0/DP1/DP2)

Goodput definition: TCPbGoTX(x) / (TCPbGoTX(x) + TCPbReTX(x)) per DSCP,
matching thesis Andreozzi-2001 §3.3.4. Both simulators emit the metric
in the same "Retx Statistics" stdout block as of the goodput 4-layer
plan (Tasks 8-12 ns-3 side, Task 16 ns-2.35 side).
"""
from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path

# Reuse the parser from scenario2-table44.py to avoid drift. The hyphenated
# filename precludes a normal `import`, so load it via importlib.
_TABLE44_PATH = Path(__file__).parent / "scenario2-table44.py"
_spec = importlib.util.spec_from_file_location("scenario2_table44", _TABLE44_PATH)
assert _spec and _spec.loader, f"cannot load {_TABLE44_PATH}"
_table44 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_table44)
parse_runlog = _table44.parse_runlog


# DSCP -> drop-precedence index in thesis Table 4.4 (AF11 only).
DSCP_TO_DP = {10: 0, 12: 1, 14: 2}

# Thesis Table 4.4 reference values (TCPbGoTX / (TCPbGoTX + TCPbReTX) per DP).
THESIS_GOODPUT = {
    1: {0: 0.87, 1: 0.91, 2: 0.78},
    2: {0: 0.87, 1: 0.91, 2: 0.79},
    3: {0: 0.88, 1: 0.91, 2: 0.78},
    4: {0: 0.89, 1: 0.92, 2: 0.81},
    5: {0: 0.89, 1: 0.91, 2: 0.83},
    6: {0: 0.89, 1: 0.91, 2: 0.83},
}


def collect_goodput(sweep_dir: Path) -> dict[int, dict[int, float]]:
    """Walk set-N/run.log; return {set: {dp: measured_goodput}}."""
    out: dict[int, dict[int, float]] = {}
    for s in range(1, 7):
        log = sweep_dir / f"set-{s}" / "run.log"
        if not log.exists():
            continue
        stats = parse_runlog(log)
        per_dp: dict[int, float] = {}
        for dscp, dp in DSCP_TO_DP.items():
            entry = stats.get(dscp)
            if not entry or "origBytes" not in entry:
                continue
            denom = entry["origBytes"] + entry["retxBytes"]
            per_dp[dp] = entry["origBytes"] / denom if denom > 0 else 1.0
        if per_dp:
            out[s] = per_dp
    return out


def emit_markdown(out_path: Path, sources: dict[str, dict[int, dict[int, float]]]) -> None:
    """Numeric per-set per-DP table with deviation against thesis."""
    lines = []
    lines.append("# Scenario 2 — measured goodput per DP per set (cross-simulator)")
    lines.append("")
    lines.append("Goodput = origBytes / (origBytes + retxBytes), per DSCP, end of simulation.")
    lines.append("Each cell shows `measured (deviation vs thesis)`. Sources:")
    lines.append("")
    for label in sources:
        lines.append(f"- **{label}**")
    lines.append("")
    lines.append("Thesis Table 4.4 reference values (Andreozzi-2001, p.78):")
    lines.append("")
    lines.append("| Set | DP0 | DP1 | DP2 |")
    lines.append("|-----|-----|-----|-----|")
    for s in range(1, 7):
        row = THESIS_GOODPUT[s]
        lines.append(f"|  {s}  | {row[0]:.2f} | {row[1]:.2f} | {row[2]:.2f} |")
    lines.append("")

    for label, data in sources.items():
        lines.append(f"## {label}")
        lines.append("")
        lines.append("| Set | DP0 (Telnet g=10) | DP1 (FTP g=12)    | DP2 (HTTP r=14)   |")
        lines.append("|-----|-------------------|-------------------|-------------------|")
        for s in range(1, 7):
            cells = [f"  {s}  "]
            for dp in (0, 1, 2):
                if s in data and dp in data[s]:
                    m = data[s][dp]
                    t = THESIS_GOODPUT[s][dp]
                    dev = m - t
                    cells.append(f" {m:.3f} ({dev:+.3f})    ")
                else:
                    cells.append(" — (missing data)  ")
            lines.append("|" + "|".join(cells) + "|")
        lines.append("")
    out_path.write_text("\n".join(lines))


def emit_plot(out_path: Path, sources: dict[str, dict[int, dict[int, float]]]) -> bool:
    """3-panel grouped-bar chart per DP, with thesis horizontal lines."""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        return False

    fig, axes = plt.subplots(1, 3, figsize=(15, 5), sharey=True)
    sets = list(range(1, 7))
    n_sources = len(sources)
    width = 0.8 / n_sources
    colors = {
        "ns-2.35 port-based": "#1f77b4",
        "ns-2.35 srTCM":      "#ff7f0e",
        "ns-3 port-based":    "#2ca02c",
        "ns-3 srTCM":         "#d62728",
    }

    for dp_idx, dp in enumerate((0, 1, 2)):
        ax = axes[dp_idx]
        x = np.arange(len(sets))
        for src_idx, (label, data) in enumerate(sources.items()):
            ys = [data.get(s, {}).get(dp, np.nan) for s in sets]
            offset = (src_idx - (n_sources - 1) / 2) * width
            ax.bar(x + offset, ys, width=width, label=label,
                   color=colors.get(label, None))
        # Thesis target line per set.
        ax.plot(x, [THESIS_GOODPUT[s][dp] for s in sets],
                marker="x", color="black", linestyle="--",
                label="Thesis Table 4.4")
        ax.set_xticks(x)
        ax.set_xticklabels([f"Set{s}" for s in sets])
        ax.set_title(f"DP{dp}")
        ax.set_ylim(0, 1.1)
        ax.grid(True, axis="y", alpha=0.3)
        if dp_idx == 0:
            ax.set_ylabel("goodput = origBytes / (origBytes + retxBytes)")
        if dp_idx == 1:
            ax.legend(loc="lower center", bbox_to_anchor=(0.5, -0.25),
                      ncol=n_sources + 1, fontsize=8)

    fig.suptitle("Scenario 2 — measured goodput vs thesis Table 4.4 (cross-simulator)")
    fig.tight_layout()
    # Save the canonical PNG, plus SVG + PDF siblings for the handbook.
    fig.savefig(out_path, dpi=120, bbox_inches="tight")
    for ext in (".svg", ".pdf"):
        fig.savefig(out_path.with_suffix(ext), bbox_inches="tight")
    plt.close(fig)
    return True


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--ns235-port", type=Path,
                   default=Path("output/ns2-35/example-2-fullscale"))
    p.add_argument("--ns235-srtcm", type=Path,
                   default=Path("output/ns2-35/example-2-fullscale-srtcm"))
    p.add_argument("--ns3-port", type=Path,
                   default=Path("output/ns3/example-2-fullscale"))
    p.add_argument("--ns3-srtcm", type=Path,
                   default=Path("output/ns3/example-2-fullscale-srtcm"))
    p.add_argument("--out-dir", type=Path,
                   default=Path("output/comparison/goodput"))
    args = p.parse_args()

    sources = {
        "ns-2.35 port-based": collect_goodput(args.ns235_port),
        "ns-2.35 srTCM":      collect_goodput(args.ns235_srtcm),
        "ns-3 port-based":    collect_goodput(args.ns3_port),
        "ns-3 srTCM":         collect_goodput(args.ns3_srtcm),
    }

    args.out_dir.mkdir(parents=True, exist_ok=True)

    md_path = args.out_dir / "goodput-comparison.md"
    emit_markdown(md_path, sources)
    print(f"Wrote {md_path}")

    plot_path = args.out_dir / "goodput-comparison.png"
    if emit_plot(plot_path, sources):
        print(f"Wrote {plot_path}")
    else:
        print("matplotlib not available — skipping plot")

    # Console summary: count how many cells have data per source.
    print()
    print("Coverage per source:")
    for label, data in sources.items():
        cells = sum(len(v) for v in data.values())
        print(f"  {label:30s}  {len(data)} sets / {cells} (set,DP) cells")
    return 0


if __name__ == "__main__":
    sys.exit(main())
