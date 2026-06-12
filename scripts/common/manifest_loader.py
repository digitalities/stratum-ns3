"""Domain-agnostic loader for substrate-registry manifests.

The Stratum substrate emits manifests via `aqm-eval-runner --manifest=PATH`
(AQM cells) and `aqm-eval-runner --scheduler-manifest=PATH` (scheduler
cells); future registries (meters, presets) will follow the same shape.
Each manifest is a JSON object whose first array entry holds the registry
rows:

    {"<arrayKey>": [ {"fileTag": "...", "family": "...", ...}, ... ]}

This module provides one generic primitive — `load_entries` — that pulls
that array out by key. Domain shims (`aqm_manifest.py`,
`scheduler_manifest.py`) wrap it with the right default path + array key
and add domain-specific helpers (markers-by-family, ECN flags, parameter
shapes).

Why a generic primitive instead of one ad-hoc loader per domain: every
new registry instantiation gets a working Python loader for free, and
the auto-generation tool (`scripts/regen-registry-tables.py`) consumes
the same primitive without knowing the domain.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable, List, Optional


def load_entries(path: Path | str, array_key: str) -> List[dict]:
    """Load `path` as JSON and return the list at top-level `array_key`.

    Raises FileNotFoundError if the file is missing, KeyError if
    `array_key` is absent, TypeError if the value is not a list.
    """
    src = Path(path)
    with open(src) as f:
        manifest = json.load(f)
    if array_key not in manifest:
        raise KeyError(
            f"manifest at {src} has no top-level array '{array_key}' "
            f"(present keys: {sorted(manifest.keys())})"
        )
    entries = manifest[array_key]
    if not isinstance(entries, list):
        raise TypeError(
            f"manifest at {src} has '{array_key}' but it is "
            f"{type(entries).__name__}, not a list"
        )
    return entries


def filter_by(entries: Iterable[dict], field: str, value) -> List[dict]:
    """Subset of `entries` whose `field` equals `value`. Skips entries
    missing the field rather than KeyError-ing — registries evolve and
    older snapshots may lack newer fields."""
    out: List[dict] = []
    for e in entries:
        if e.get(field) == value:
            out.append(e)
    return out


def project(entries: Iterable[dict], fields: Iterable[str]) -> List[List]:
    """Return a list-of-lists projecting each entry to `fields` in order.
    Missing fields render as empty strings — a downstream Markdown table
    cell renders as blank rather than crashing the regeneration."""
    cols = list(fields)
    rows: List[List] = []
    for e in entries:
        rows.append([e.get(c, "") for c in cols])
    return rows


def by_file_tag(entries: Iterable[dict]) -> dict:
    """Index by `fileTag`. Convenience for callers that need O(1) lookup
    when iterating over a separate ordering."""
    return {e["fileTag"]: e for e in entries if "fileTag" in e}
