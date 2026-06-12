#!/usr/bin/env python3
"""Concatenate Stratum + Linux per-flow CSVs into one file."""

import os
import sys
from pathlib import Path

import pandas as pd

REPO_ROOT = Path(os.environ.get("REPO_ROOT") or Path(__file__).resolve().parent.parent)
OUT_DIR = REPO_ROOT / "output" / "ns3" / "cake-host-fairness"


def main() -> int:
    stratum = OUT_DIR / "sweep-perflow-stratum.csv"
    linux = OUT_DIR / "sweep-perflow-linux.csv"
    out = OUT_DIR / "sweep-perflow.csv"

    for p in (stratum, linux):
        if not p.exists():
            print(f"missing: {p}", file=sys.stderr)
            return 1

    df_s = pd.read_csv(stratum)
    df_l = pd.read_csv(linux)
    df = pd.concat([df_s, df_l], ignore_index=True)

    df.to_csv(out, index=False)
    print(f"Wrote {out} ({len(df)} rows: {len(df_s)} stratum + {len(df_l)} linux).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
