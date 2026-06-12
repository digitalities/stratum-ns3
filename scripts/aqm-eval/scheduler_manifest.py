"""Loader for the scheduler registry manifest.

Wrapper over `scripts/common/manifest_loader.py`. Mirrors the AQM
manifest loader (`aqm_manifest.py`) for scheduler cells; downstream
plotting and table-generation tools see the same surface for both
domains.

Regenerate the committed snapshot after adding a new scheduler cell:

    cd ns3/ns-3-dev
    ./ns3 run "aqm-eval-runner --scheduler-manifest=$(git rev-parse --show-toplevel)/scripts/aqm-eval/scheduler-manifest.json"
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Dict, List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "common"))
import manifest_loader  # noqa: E402

DEFAULT_MANIFEST = Path(__file__).resolve().parent / "scheduler-manifest.json"
ARRAY_KEY = "schedulers"


def entries(path: Optional[Path] = None) -> List[dict]:
    """List of scheduler rows in registration order."""
    return manifest_loader.load_entries(path or DEFAULT_MANIFEST, ARRAY_KEY)


def families(path: Optional[Path] = None) -> Dict[str, str]:
    """{fileTag: family} mapping (priority/round-robin/fair-queue/hybrid)."""
    return {e["fileTag"]: e["family"] for e in entries(path)}


def display_names(path: Optional[Path] = None) -> Dict[str, str]:
    return {e["fileTag"]: e["displayName"] for e in entries(path)}


def parameter_shapes(path: Optional[Path] = None) -> Dict[str, str]:
    """{fileTag: parameterShape} — encodes which SchedulerArgs fields
    matter for each scheduler (none, priority-winlen, rr-weights,
    fq-shares, hybrid-llq)."""
    return {e["fileTag"]: e["parameterShape"] for e in entries(path)}


def needs_link_bandwidth(path: Optional[Path] = None) -> Dict[str, bool]:
    return {e["fileTag"]: e["needsLinkBandwidth"] for e in entries(path)}


def file_tags(path: Optional[Path] = None) -> List[str]:
    return [e["fileTag"] for e in entries(path)]
