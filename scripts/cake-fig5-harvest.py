#!/usr/bin/env python3
"""Harvest FIG5,... / FIG5DIAG,... lines from a stratum-cake-q15 test log into CSV.

Inputs (lines on stdin or in argv[1]):
    FIG5,<arm>,<rng>,<t_bucket_s>,<induced_owd_ms>
    FIG5DIAG,<arm>,<rng>,efGoodputMbps=..,bulkGoodputMbps=..,efLossPct=..
Outputs:
    output/ns3/cake-fig5/ns3-latency.csv      (arm,rng_run,t_s,induced_owd_ms)
    output/ns3/cake-fig5/ns3-goodput-loss.csv (arm,rng_run,ef_goodput_mbps,
                                               bulk_goodput_mbps,ef_loss_pct)

Usage:
    ./build/utils/ns3-dev-test-runner-default --suite=stratum-cake-q15 \
        --fullness=EXTENSIVE | python3 scripts/cake-fig5-harvest.py
    python3 scripts/cake-fig5-harvest.py /tmp/fig5-run.log
"""
import csv
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(os.environ.get("REPO_ROOT", Path(__file__).resolve().parent.parent))
OUT_DIR = Path(os.environ.get("OUT_DIR", REPO_ROOT / "output" / "ns3" / "cake-fig5"))

LAT_RE = re.compile(
    r"FIG5,(?P<arm>[^,]+),(?P<rng>\d+),(?P<t>[0-9.eE+-]+),(?P<owd>[0-9.eE+-]+)"
)
DIAG_RE = re.compile(
    r"FIG5DIAG,(?P<arm>[^,]+),(?P<rng>\d+),"
    r"efGoodputMbps=(?P<ef>[0-9.eE+-]+),"
    r"bulkGoodputMbps=(?P<bulk>[0-9.eE+-]+),"
    r"efLossPct=(?P<loss>[0-9.eE+-]+)"
)


def main() -> int:
    if len(sys.argv) >= 2:
        text = Path(sys.argv[1]).read_text()
    else:
        text = sys.stdin.read()

    lat_rows = []
    seen_lat = set()
    for m in LAT_RE.finditer(text):
        key = (m["arm"], int(m["rng"]), m["t"])
        if key in seen_lat:
            continue
        seen_lat.add(key)
        lat_rows.append({
            "arm": m["arm"],
            "rng_run": int(m["rng"]),
            "t_s": float(m["t"]),
            "induced_owd_ms": float(m["owd"]),
        })

    diag_rows = []
    seen_diag = set()
    for m in DIAG_RE.finditer(text):
        key = (m["arm"], int(m["rng"]))
        if key in seen_diag:
            continue
        seen_diag.add(key)
        diag_rows.append({
            "arm": m["arm"],
            "rng_run": int(m["rng"]),
            "ef_goodput_mbps": float(m["ef"]),
            "bulk_goodput_mbps": float(m["bulk"]),
            "ef_loss_pct": float(m["loss"]),
        })

    if not lat_rows:
        print("cake-fig5-harvest: no FIG5 lines found", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    lat_out = OUT_DIR / "ns3-latency.csv"
    with lat_out.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["arm", "rng_run", "t_s", "induced_owd_ms"])
        w.writeheader()
        w.writerows(lat_rows)

    diag_out = OUT_DIR / "ns3-goodput-loss.csv"
    with diag_out.open("w", newline="") as fh:
        w = csv.DictWriter(
            fh,
            fieldnames=["arm", "rng_run", "ef_goodput_mbps", "bulk_goodput_mbps", "ef_loss_pct"],
        )
        w.writeheader()
        w.writerows(diag_rows)

    arms = sorted({r["arm"] for r in lat_rows})
    print(f"Wrote {lat_out} ({len(lat_rows)} rows) and {diag_out} "
          f"({len(diag_rows)} rows); arms={arms}")
    if not diag_rows:
        print("cake-fig5-harvest: WARNING no FIG5DIAG lines found", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
