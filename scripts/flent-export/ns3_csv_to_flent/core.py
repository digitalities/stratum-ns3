"""CSV bundle to Flent JSON v1 conversion core."""
from __future__ import annotations

import csv
import gzip
import json
from pathlib import Path
from typing import Any

from . import flent_format


def read_metadata(indir: Path) -> dict[str, Any]:
    """Load metadata.json. Coerces numeric-looking strings to int/float."""
    raw = json.loads((indir / "metadata.json").read_text())
    coerced: dict[str, Any] = {}
    for k, v in raw.items():
        if isinstance(v, str):
            # Try int first, then float, else keep as string
            try:
                coerced[k] = int(v)
                continue
            except ValueError:
                pass
            try:
                coerced[k] = float(v)
                continue
            except ValueError:
                pass
            coerced[k] = v
        else:
            coerced[k] = v
    return coerced


def read_x_values(indir: Path) -> list[float]:
    """Read x_values.csv. Returns [] if missing (e.g., probe-only test)."""
    path = indir / "x_values.csv"
    if not path.exists():
        return []
    with path.open() as f:
        reader = csv.DictReader(f)
        return [float(row["t"]) for row in reader]


def read_tcp_csv(path: Path) -> list[float]:
    """Read tcp_down*/tcp_up* CSV; returns list of goodput_mbps values, one per row."""
    if not path.exists():
        return []
    with path.open() as f:
        reader = csv.DictReader(f)
        return [float(row["goodput_mbps"]) for row in reader]


def read_event_csv(path: Path) -> list[tuple[float, float]]:
    """Read ping_icmp / udp_probe CSV; returns list of (t_seconds, rtt_ms)."""
    if not path.exists():
        return []
    with path.open() as f:
        reader = csv.DictReader(f)
        return [(float(row["t"]), float(row["rtt_ms"])) for row in reader]


def emit_flent_doc(
    metadata: dict[str, Any],
    schema: dict[str, Any],
    indir: Path,
    title: str | None = None,
) -> dict[str, Any]:
    """Build the Flent JSON v1 document from a CSV bundle."""
    return flent_format.build_doc(metadata, schema, indir, title=title)


def write_flent_gz(doc: dict[str, Any], output: Path) -> None:
    """Write the JSON doc to a .flent.gz file (gzipped JSON)."""
    output.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(output, "wt", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
