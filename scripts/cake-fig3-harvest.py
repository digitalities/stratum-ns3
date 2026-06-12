#!/usr/bin/env python3
"""Harvest FIG3,... lines from a stratum-cake-q15 test log into a per-flow CSV.

Input:  a test.py log on stdin or as argv[1] (lines containing
        'FIG3,<mode>,<rng>,<flow>,<src>,<dst>,<share>').
Output: output/ns3/cake-fig3/ns3-perflow.csv
        (columns: mode,rng_run,flow_idx,src,dst,share)

Usage:
    python3 test.py -s stratum-cake-q15 -v | python3 scripts/cake-fig3-harvest.py
    python3 scripts/cake-fig3-harvest.py /tmp/fig3-q15.log
"""
import csv
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(os.environ.get("REPO_ROOT", Path(__file__).resolve().parent.parent))
OUT_DIR = Path(os.environ.get("OUT_DIR", REPO_ROOT / "output" / "ns3" / "cake-fig3"))

# Match the harvest line anywhere in a (possibly test-runner-prefixed) line.
LINE_RE = re.compile(
    r"FIG3,(?P<mode>[^,]+),(?P<rng>\d+),(?P<flow>\d+),"
    r"(?P<src>[^,]+),(?P<dst>[^,]+),(?P<share>[0-9.eE+-]+)"
)


def main() -> int:
    if len(sys.argv) >= 2:
        text = Path(sys.argv[1]).read_text()
    else:
        text = sys.stdin.read()

    rows = []
    seen = set()
    for m in LINE_RE.finditer(text):
        key = (m["mode"], int(m["rng"]), int(m["flow"]))
        if key in seen:  # de-dup if the log repeats a line
            continue
        seen.add(key)
        rows.append({
            "mode": m["mode"],
            "rng_run": int(m["rng"]),
            "flow_idx": int(m["flow"]),
            "src": m["src"],
            "dst": m["dst"],
            "share": float(m["share"]),
        })

    if not rows:
        print("cake-fig3-harvest: no FIG3 lines found", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out = OUT_DIR / "ns3-perflow.csv"
    with out.open("w", newline="") as fh:
        w = csv.DictWriter(
            fh, fieldnames=["mode", "rng_run", "flow_idx", "src", "dst", "share"])
        w.writeheader()
        w.writerows(rows)

    modes = sorted({r["mode"] for r in rows})
    print(f"Wrote {out} ({len(rows)} rows; modes={modes})")
    expected = 4 * 3 * 6
    if len(rows) != expected:
        print(f"WARNING: expected {expected} rows (4 modes x 3 seeds x 6 flows), "
              f"got {len(rows)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
