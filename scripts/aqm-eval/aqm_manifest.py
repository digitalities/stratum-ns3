"""Loader for the AQM registry manifest.

Wrapper over `scripts/common/manifest_loader.py`. The committed snapshot
ships next to this module so plotting scripts work without first
building or running the C++ binary.

Regenerate the snapshot after adding a new AQM cell to the registry:

    cd ns3/ns-3-dev
    ./ns3 run "aqm-eval-runner --manifest=$(git rev-parse --show-toplevel)/scripts/aqm-eval/aqm-manifest.json"
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Dict, List, Optional

# Make `scripts/common/` importable when this module is loaded from
# arbitrary working directories.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "common"))
import manifest_loader  # noqa: E402

DEFAULT_MANIFEST = Path(__file__).resolve().parent / "aqm-manifest.json"
ARRAY_KEY = "aqms"

# Per-family marker shape — single-queue → circle, FQ-class → triangle,
# Stratum → square. Editorial; lives here, not in the C++ registry.
FAMILY_MARKERS: Dict[str, str] = {
    "single":  "o",
    "fq":      "^",
    "stratum": "s",
    "ds4":     "s",  # legacy family token in pre-rename recorded manifests
}


def load(path: Optional[Path] = None) -> dict:
    """Load and parse the manifest JSON. Backward-compatible signature
    returning the full manifest dict (legacy plotting code reads
    `m["aqms"]` directly)."""
    src = Path(path) if path is not None else DEFAULT_MANIFEST
    import json
    with open(src) as f:
        return json.load(f)


def entries(path: Optional[Path] = None) -> List[dict]:
    """List of AQM rows, in registration order. Preferred over load()
    for new code."""
    return manifest_loader.load_entries(path or DEFAULT_MANIFEST, ARRAY_KEY)


def families(manifest: Optional[dict] = None) -> Dict[str, str]:
    """Return a dict of {fileTag: family} for every registered AQM."""
    rows = manifest["aqms"] if manifest is not None else entries()
    return {e["fileTag"]: e["family"] for e in rows}


def display_names(manifest: Optional[dict] = None) -> Dict[str, str]:
    """Return a dict of {fileTag: displayName}."""
    rows = manifest["aqms"] if manifest is not None else entries()
    return {e["fileTag"]: e["displayName"] for e in rows}


def supports_ecn(manifest: Optional[dict] = None) -> Dict[str, bool]:
    """Return a dict of {fileTag: supportsEcn}."""
    rows = manifest["aqms"] if manifest is not None else entries()
    return {e["fileTag"]: e["supportsEcn"] for e in rows}


def file_tags(manifest: Optional[dict] = None) -> List[str]:
    """Return all known fileTag values, in registry order."""
    rows = manifest["aqms"] if manifest is not None else entries()
    return [e["fileTag"] for e in rows]


def markers_by_family(manifest: Optional[dict] = None) -> Dict[str, str]:
    """Return a dict of {fileTag: marker_shape} derived from family."""
    fam = families(manifest)
    return {tag: FAMILY_MARKERS.get(family, "o") for tag, family in fam.items()}
