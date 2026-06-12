#!/usr/bin/env python3
"""Side-by-side comparison: Lima 8-CPU vs Hetzner CCX33.

Reads verdict.json from both platforms' output directories and emits:
- A markdown table for paper §8
- A LaTeX table for paper/diffserv4ns-substrate.tex
- A JSON summary at output/hetzner-lima-comparison.json

Run after rsync'ing Hetzner outputs back to laptop.
"""
import json
import math
import statistics
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "output"

LIMA_BASE   = OUT / "stratum-bridge-closed-loop-8cpu"
LIMA_CTRL   = OUT / "stratum-bridge-closed-loop-8cpu-control-noHI"
HETZ_BASE   = OUT / "stratum-bridge-closed-loop-hetzner-ccx33"
LIMA_2X2    = OUT / "cake-l4s-composition-16-1"
HETZ_2X2    = OUT / "cake-l4s-composition-16-1-hetzner-ccx33"


def _load_verdict(d: Path) -> dict | None:
    f = d / "verdict.json"
    return json.loads(f.read_text()) if f.exists() else None


def _share_from_replica_dir(rdir: Path) -> float | None:
    """Try Hetzner naming (iperf3-src-{a,b}.json), then Lima naming (iperf3-{a,b}.json)."""
    for a_name, b_name in [("iperf3-src-a.json", "iperf3-src-b.json"),
                            ("iperf3-a.json", "iperf3-b.json")]:
        sa = rdir / a_name
        sb = rdir / b_name
        if sa.exists() and sb.exists():
            a = json.loads(sa.read_text()).get("end", {}).get("sum_received", {}).get("bytes")
            b = json.loads(sb.read_text()).get("end", {}).get("sum_received", {}).get("bytes")
            if a and b:
                return a / (a + b)
    return None


def _share_from_composition_json(rdir: Path) -> float | None:
    f = rdir / "composition.json"
    if not f.exists():
        return None
    j = json.loads(f.read_text())
    a = j.get("a_bytes")
    b = j.get("b_bytes")
    if a is None or b is None:
        return None
    return a / (a + b) if (a + b) > 0 else None


def _aggregate_plain_lima(cell_dir: Path) -> dict:
    """Lima plain-* uses flat r{N}.json files at top level (no per-replica subdirs)."""
    if not cell_dir.exists():
        return {"available": False, "path": str(cell_dir)}
    shares = []
    for f in sorted(cell_dir.glob("r*.json")):
        j = json.loads(f.read_text())
        a = j.get("a_bytes"); b = j.get("b_bytes")
        if a is not None and b is not None and (a + b) > 0:
            shares.append(a / (a + b))
    stats = _compute_stats(shares)
    stats["available"] = bool(shares)
    stats["path"] = str(cell_dir)
    return stats


def _compute_stats(shares: list[float]) -> dict:
    if not shares:
        return {"mean": None, "ci95_lo": None, "ci95_hi": None, "n": 0}
    m = statistics.mean(shares)
    if len(shares) < 2:
        return {"mean": m, "ci95_lo": m, "ci95_hi": m, "n": 1, "replicas": shares}
    sd = statistics.stdev(shares)
    se = sd / math.sqrt(len(shares))
    half = 1.96 * se
    return {
        "mean": m,
        "ci95_lo": m - half,
        "ci95_hi": m + half,
        "n": len(shares),
        "replicas": shares,
        "stdev": sd,
    }


def _aggregate_cell(cell_dir: Path, replica_extractor) -> dict:
    if not cell_dir.exists():
        return {"available": False, "path": str(cell_dir)}
    shares = []
    for r in sorted(cell_dir.iterdir()):
        if not r.is_dir() or not r.name.startswith("r"):
            continue
        s = replica_extractor(r)
        if s is not None:
            shares.append(s)
    stats = _compute_stats(shares)
    stats["available"] = bool(shares)
    stats["path"] = str(cell_dir)
    return stats


def main() -> int:
    print("=" * 70)
    print(" Hetzner CCX33 (8 dedicated cores, Debian 13) vs Lima 8-CPU")
    print("=" * 70)

    # Bridge sweep (3 protocols × n=3)
    print("\n## 1. Bridge sweep (host-iso ON)\n")
    bridge_rows = []
    for variant in ["cubic", "reno", "bbr"]:
        lima = _aggregate_cell(LIMA_BASE / variant, _share_from_replica_dir)
        hetz = _aggregate_cell(HETZ_BASE / variant, _share_from_replica_dir)
        bridge_rows.append({"variant": variant, "lima": lima, "hetzner": hetz})
        if lima["available"] and hetz["available"]:
            delta = (hetz["mean"] - lima["mean"]) * 100
            print(
                f"  {variant.upper():8s}  Lima {lima['mean']:.4f} ± "
                f"{(lima['ci95_hi']-lima['ci95_lo'])/2:.4f}   "
                f"Hetzner {hetz['mean']:.4f} ± "
                f"{(hetz['ci95_hi']-hetz['ci95_lo'])/2:.4f}   "
                f"Δ {delta:+.1f} pp"
            )

    # Control noHI (CUBIC)
    print("\n## 2. Host-isolation-OFF control (CUBIC)\n")
    lima_ctrl = _aggregate_cell(LIMA_CTRL, _share_from_replica_dir)
    hetz_ctrl = _aggregate_cell(HETZ_BASE / "control-noHI", _share_from_replica_dir)
    print(f"  Lima      mean={lima_ctrl['mean']:.4f}  replicas={[round(r,4) for r in lima_ctrl.get('replicas',[])]}")
    print(f"  Hetzner   mean={hetz_ctrl['mean']:.4f}  replicas={[round(r,4) for r in hetz_ctrl.get('replicas',[])]}")
    print(f"  Theoretical 16/17 = {16/17:.4f}")
    print(f"  Lima    deviation from 16/17:  {(lima_ctrl['mean']-16/17)*100:+.2f} pp")
    print(f"  Hetzner deviation from 16/17:  {(hetz_ctrl['mean']-16/17)*100:+.2f} pp")

    # 2x2 CAKE+L4S
    print("\n## 3. CAKE+L4S 2x2 at (16,1)\n")
    cells_2x2 = ["plain-cubic", "plain-dctcp", "bridge-cubic", "bridge-dctcp"]
    rows_2x2 = []
    for cell in cells_2x2:
        # Lima plain-* uses flat r{N}.json files; Hetzner plain-* uses r{N}/composition.json
        # Both bridge-* use per-replica subdirs (with different iperf3 file-name prefixes)
        if cell.startswith("plain-"):
            lima = _aggregate_plain_lima(LIMA_2X2 / cell)
            hetz = _aggregate_cell(HETZ_2X2 / cell, _share_from_composition_json)
        else:
            lima = _aggregate_cell(LIMA_2X2 / cell, _share_from_replica_dir)
            hetz = _aggregate_cell(HETZ_2X2 / cell, _share_from_replica_dir)
        rows_2x2.append({"cell": cell, "lima": lima, "hetzner": hetz})
        if lima["available"] and hetz["available"]:
            print(
                f"  {cell:14s}  Lima {lima['mean']:.4f} ± {(lima['ci95_hi']-lima['ci95_lo'])/2:.4f}   "
                f"Hetzner {hetz['mean']:.4f} ± {(hetz['ci95_hi']-hetz['ci95_lo'])/2:.4f}   "
                f"Δ {(hetz['mean']-lima['mean'])*100:+.2f} pp"
            )
        else:
            l = f"{lima['mean']:.4f}" if lima["available"] else "(missing)"
            h = f"{hetz['mean']:.4f}" if hetz["available"] else "(missing)"
            print(f"  {cell:14s}  Lima {l}   Hetzner {h}")

    # JSON dump
    out_json = OUT / "hetzner-lima-comparison.json"
    out_json.write_text(json.dumps({
        "bridge_sweep": bridge_rows,
        "control_noHI": {"lima": lima_ctrl, "hetzner": hetz_ctrl, "theoretical_16_17": 16/17},
        "cake_l4s_2x2": rows_2x2,
    }, indent=2))
    print(f"\nJSON summary: {out_json.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
