#!/usr/bin/env python3
"""Q-17 Parekh-Gallager Theorem 1 conformance — visual audit plot.

Parses stderr report lines from `diffserv-q17-parekh-theorem1` test
runs and renders a 3-panel figure summarising the cross-scheduler
behaviour:

  panel 1: max(F̂ − F) vs weight regime, grouped by scheduler.
           Reference lines at L_max/r (strict Theorem 1 bound) and
           2·L_max/r (Choice-B gate).
  panel 2: strict-Theorem-1 violation rate (#viol / #pkts) vs regime.
  panel 3: under-share / over-share of the lighter-weight flow vs
           the configured target ratio.

Input: a text file containing the stderr report lines (capture the
runner's stderr; e.g. via `2>&1 | grep '\\[Q-17.1' > q17-report.txt`).
Or pipe the runner's stderr directly via stdin.

Output: output/chang-comparison/q17-parekh-theorem1.png
        output/chang-comparison/q17-summary.csv

Usage:
    python3 scripts/plot-q17-parekh.py q17-report.txt
    cat q17-report.txt | python3 scripts/plot-q17-parekh.py
"""
import argparse
import csv
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

LINE_RE = re.compile(
    r"\[Q-17\.1\s+(?P<sched>\S+)\s+(?P<regime>\S+)\]\s+"
    r"post-warmup packets=(?P<n>\d+)\s+"
    r"\(per-flow:\s+(?P<a>\d+)/(?P<b>\d+)\)\s+"
    r"max\(F_hat - F\)=(?P<max_gap>[-\d.eE+]+)\s+s\s+"
    r"mean\(F_hat - F\)=(?P<mean_gap>[-\d.eE+]+)\s+s\s+"
    r"L_max/r=(?P<lmax_r>[-\d.eE+]+)\s+s\s+"
    r"strict-Thm1 violations.*?:\s+(?P<viol>\d+)/(?P<total>\d+)"
)

# Target ratios for asymmetric regimes (lighter/heavier flow weight).
# Symmetric regime is 1.0; asymmetric is named like asym1to2, asym1to10.
REGIME_TARGET = {
    "sym": 1.0,
    "asym1to2": 0.5,
    "asym1to3": 1.0 / 3,
    "asym1to5": 0.2,
    "asym1to10": 0.1,
}
REGIME_ORDER = ["sym", "asym1to2", "asym1to3", "asym1to5", "asym1to10"]
REGIME_LABEL = {
    "sym": "1:1",
    "asym1to2": "1:2",
    "asym1to3": "1:3",
    "asym1to5": "1:5",
    "asym1to10": "1:10",
}

SCHED_COLOUR = {
    "WFQ": "#1f77b4",
    "WF2Q+": "#2ca02c",
    "SCFQ": "#d62728",
}


def parse(text: str) -> list[dict]:
    rows = []
    seen = set()  # de-dup repeated stderr blocks
    for m in LINE_RE.finditer(text):
        key = (m.group("sched"), m.group("regime"))
        if key in seen:
            continue
        seen.add(key)
        n = int(m.group("n"))
        a = int(m.group("a"))
        b = int(m.group("b"))
        viol = int(m.group("viol"))
        target = REGIME_TARGET.get(m.group("regime"))
        # observed light-share fraction of total packets
        light_share = min(a, b) / n if n else 0.0
        target_light = target / (1 + target) if target else 0.5
        deviation_pp = (light_share - target_light) * 100.0
        rows.append(
            {
                "sched": m.group("sched"),
                "regime": m.group("regime"),
                "n": n,
                "a": a,
                "b": b,
                "max_gap_ms": float(m.group("max_gap")) * 1000.0,
                "mean_gap_ms": float(m.group("mean_gap")) * 1000.0,
                "lmax_r_ms": float(m.group("lmax_r")) * 1000.0,
                "violations": viol,
                "viol_rate": viol / n if n else 0.0,
                "light_share": light_share,
                "deviation_pp": deviation_pp,
            }
        )
    return rows


def write_csv(rows: list[dict], path: Path) -> None:
    fields = [
        "sched",
        "regime",
        "n",
        "a",
        "b",
        "max_gap_ms",
        "mean_gap_ms",
        "lmax_r_ms",
        "violations",
        "viol_rate",
        "light_share",
        "deviation_pp",
    ]
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            w.writerow(r)


def plot(rows: list[dict], out_path: Path) -> None:
    by_sched = {}
    for r in rows:
        by_sched.setdefault(r["sched"], {})[r["regime"]] = r
    schedulers = [s for s in ("WFQ", "WF2Q+", "SCFQ") if s in by_sched]

    fig, (ax_gap, ax_viol, ax_dev) = plt.subplots(1, 3, figsize=(15, 4.5))

    x = list(range(len(REGIME_ORDER)))
    bar_w = 0.25
    offsets = {s: (i - (len(schedulers) - 1) / 2) * bar_w for i, s in enumerate(schedulers)}

    # --- panel 1: max(F̂ − F) ---
    lmax_r = max((r["lmax_r_ms"] for r in rows), default=8.224)
    for s in schedulers:
        bars = [by_sched[s].get(reg, {}).get("max_gap_ms", float("nan")) for reg in REGIME_ORDER]
        positions = [xi + offsets[s] for xi in x]
        ax_gap.bar(positions, bars, bar_w, label=s, color=SCHED_COLOUR.get(s))
    ax_gap.axhline(lmax_r, color="black", linestyle="--", linewidth=1, label=f"L_max/r = {lmax_r:.2f} ms")
    ax_gap.axhline(2 * lmax_r, color="grey", linestyle=":", linewidth=1, label=f"2·L_max/r (Choice-B) = {2*lmax_r:.2f} ms")
    ax_gap.set_yscale("log")
    ax_gap.set_xticks(x)
    ax_gap.set_xticklabels([REGIME_LABEL[r] for r in REGIME_ORDER])
    ax_gap.set_xlabel("Weight ratio (light : heavy)")
    ax_gap.set_ylabel("max(F̂ − F) (ms)")
    ax_gap.set_title("Q-17.1 Theorem 1 envelope — per-packet finish-time gap")
    ax_gap.legend(loc="upper left", fontsize=8)
    ax_gap.grid(axis="y", linestyle=":", alpha=0.5)

    # --- panel 2: strict-Theorem-1 violation rate ---
    for s in schedulers:
        rates = [by_sched[s].get(reg, {}).get("viol_rate", 0.0) * 100 for reg in REGIME_ORDER]
        positions = [xi + offsets[s] for xi in x]
        ax_viol.bar(positions, rates, bar_w, label=s, color=SCHED_COLOUR.get(s))
    ax_viol.set_xticks(x)
    ax_viol.set_xticklabels([REGIME_LABEL[r] for r in REGIME_ORDER])
    ax_viol.set_xlabel("Weight ratio (light : heavy)")
    ax_viol.set_ylabel("strict-Thm1 violations (%)")
    ax_viol.set_title("Q-17.1 strict-Theorem-1 violation rate")
    ax_viol.legend(loc="upper left", fontsize=8)
    ax_viol.grid(axis="y", linestyle=":", alpha=0.5)

    # --- panel 3: light-flow share deviation from target ---
    for s in schedulers:
        devs = [by_sched[s].get(reg, {}).get("deviation_pp", 0.0) for reg in REGIME_ORDER]
        positions = [xi + offsets[s] for xi in x]
        ax_dev.bar(positions, devs, bar_w, label=s, color=SCHED_COLOUR.get(s))
    ax_dev.axhline(0, color="black", linewidth=0.8)
    ax_dev.set_xticks(x)
    ax_dev.set_xticklabels([REGIME_LABEL[r] for r in REGIME_ORDER])
    ax_dev.set_xlabel("Weight ratio (light : heavy)")
    ax_dev.set_ylabel("Light-flow share deviation (pp)")
    ax_dev.set_title("Q-17.1 throughput share deviation from target")
    ax_dev.legend(loc="upper left", fontsize=8)
    ax_dev.grid(axis="y", linestyle=":", alpha=0.5)

    fig.suptitle("Q-17 Parekh-Gallager 1993 Theorem 1 conformance — DiffServ4NS ns-3", fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(out_path, dpi=150)
    plt.close(fig)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("input", nargs="?", default="-",
                   help="path to stderr-report file, or '-' for stdin")
    p.add_argument("--out-dir", default="output/chang-comparison",
                   help="directory for the .png + .csv (default: output/chang-comparison)")
    args = p.parse_args()

    if args.input == "-":
        text = sys.stdin.read()
    else:
        text = Path(args.input).read_text()

    rows = parse(text)
    if not rows:
        print("error: no Q-17.1 stderr lines found in input", file=sys.stderr)
        return 1

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "q17-summary.csv"
    png_path = out_dir / "q17-parekh-theorem1.png"
    write_csv(rows, csv_path)
    plot(rows, png_path)

    print(f"parsed {len(rows)} (scheduler, regime) cases")
    print(f"  CSV: {csv_path}")
    print(f"  PNG: {png_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
