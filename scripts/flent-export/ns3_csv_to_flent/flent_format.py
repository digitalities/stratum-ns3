"""Flent JSON v4 layout.

Mirrors the schema produced by Flent's own ``ResultSet.serialise()``
(see ``flent/resultset.py``: ``FILEFORMAT_VERSION = 4``). The top-level
keys are ``version``, ``metadata``, ``x_values``, ``results``, and
``raw_values``. Metadata keys are uppercase, mirroring Flent's
``RECORDED_SETTINGS`` tuple.
"""
from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from . import core as _core  # avoid circular import via late binding

FLENT_VERSION = 4  # matches FILEFORMAT_VERSION in flent/resultset.py


def _utc_now_isoformat() -> str:
    """ISO-8601 timestamp with microseconds, no timezone suffix.

    Flent's ``parse_date()`` consumes either ``YYYY-MM-DDTHH:MM:SS.ffffff``
    (assumed UTC) or with an explicit offset.
    """
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")


def build_doc(
    metadata: dict[str, Any],
    schema: dict[str, Any],
    indir: Path,
    title: str | None = None,
) -> dict[str, Any]:
    """Compose a Flent JSON v4 doc from per-flow CSVs and a schema mapping."""
    # x_values: time axis (already in seconds, list of floats)
    x_values = _core.read_x_values(indir)

    # raw_values: dict of series-name -> list of {"t": float, "val": float}
    # results: per-x-step aligned series (list of float | None)
    raw_values: dict[str, list[dict[str, float]]] = {}
    results: dict[str, list[float | None]] = {}

    for series_name, src_csv in schema["series"]:
        path = indir / src_csv
        if series_name.startswith("Ping (ms)"):
            samples = _core.read_event_csv(path)
            raw_values[series_name] = [{"t": t, "val": rtt} for t, rtt in samples]
            # Aligned to x_values: bin samples to the nearest step.
            results[series_name] = _align_to_x(samples, x_values)
        else:
            # TCP series: one value per x_values step
            tcp_vals = _core.read_tcp_csv(path)
            raw_values[series_name] = [
                {"t": t, "val": v} for t, v in zip(x_values, tcp_vals)
            ]
            padded: list[float | None] = list(tcp_vals)
            if len(padded) < len(x_values):
                padded.extend([None] * (len(x_values) - len(padded)))
            results[series_name] = padded

    # Aggregates (sum / average) computed across the requested member series.
    for total_name, spec in schema.get("totals", {}).items():
        if isinstance(spec, dict):
            kind = spec.get("kind", "sum")
            members = spec["members"]
        else:
            kind = "sum"
            members = spec
        member_lists = [results[m] for m in members]
        if not member_lists:
            results[total_name] = [None] * len(x_values)
            raw_values[total_name] = []
            continue
        n = len(x_values)
        agg: list[float | None] = []
        for i in range(n):
            present = [
                lst[i]
                for lst in member_lists
                if i < len(lst) and lst[i] is not None
            ]
            if not present:
                agg.append(None)
            elif kind == "average":
                agg.append(sum(present) / len(present))
            else:  # default sum
                agg.append(sum(present))
        results[total_name] = agg
        raw_values[total_name] = [
            {"t": x, "val": v}
            for x, v in zip(x_values, agg)
            if v is not None
        ]

    # ------------------------------------------------------------------
    # Compose metadata. Flent expects uppercase keys mirroring the
    # ``RECORDED_SETTINGS`` tuple in flent/resultset.py.
    # ------------------------------------------------------------------
    test_name = metadata.get("test_name") or schema.get("name", "rrul")
    length_s = float(metadata.get("length_s", 0.0)) or (
        max(x_values) if x_values else 0.0
    )
    step_size = float(metadata.get("step_size_s", 0.2)) or 0.2

    # Build per-series metadata. Flent indexes ping/UDP series by units
    # so ``ping_cdf`` and similar plots can pick the right axis.
    series_meta: dict[str, dict[str, Any]] = {}
    all_series: list[str] = list(raw_values.keys()) + [
        n for n in results.keys() if n not in raw_values
    ]
    for name in all_series:
        if name.startswith("Ping (ms)"):
            series_meta[name] = {"UNITS": "ms", "MEAN_VALUE": None}
        else:
            series_meta[name] = {"UNITS": "Mbits/s", "MEAN_VALUE": None}

    # Total length: pad the final ping interval (used by Flent for
    # x-axis bounds; the field is required by ResultSet.unserialise()).
    total_length = length_s if length_s > 0 else (
        max(x_values) if x_values else 0.0
    )

    # NAME is mandatory (ResultSet.__init__ raises if missing).
    # LOCAL_HOST + HOST are required by Flent's plot annotation
    # (plotters.py: ``Local/remote: %s/%s ...`` line). They are free-
    # form labels - the simulation does not bind them to real hosts.
    flent_metadata: dict[str, Any] = {
        "NAME": test_name,
        "TITLE": title or schema.get("title", test_name),
        "TIME": _utc_now_isoformat(),
        "T0": _utc_now_isoformat(),
        "LENGTH": length_s,
        "TOTAL_LENGTH": total_length,
        "STEP_SIZE": step_size,
        "FLENT_VERSION": "ns3-csv-to-flent v1",
        "IP_VERSION": 4,
        "LOCAL_HOST": "ns-3-client",
        "HOST": "ns-3-bottleneck",
        "SERIES_META": series_meta,
        "DATA_FILENAME": f"{test_name}.flent.gz",
        "TEST_PARAMETERS": {},
        # Pass through ns-3-side context for downstream tooling. Flent
        # ignores unknown keys; we keep the original lowercase fields
        # alongside so the bundle stays self-describing.
        "NS3_AQM": metadata.get("aqm"),
        "NS3_BANDWIDTH_BPS": metadata.get("bandwidth_bps"),
        "NS3_RTT_MS": metadata.get("rtt_ms"),
        "NS3_TOPOLOGY_CLASS": metadata.get("topology_class"),
        "NS3_BUILD_SHA": metadata.get("ns3_build_sha"),
        "NS3_DSCP_MAP": metadata.get("dscp_map", {}),
    }

    return {
        "version": FLENT_VERSION,
        "metadata": flent_metadata,
        "x_values": list(x_values),
        "raw_values": raw_values,
        "results": results,
    }


def _align_to_x(
    samples: list[tuple[float, float]],
    x_values: list[float],
) -> list[float | None]:
    """Bin event-based RTT samples to the closest step in x_values; per-step mean (or None)."""
    if not x_values:
        return [s[1] for s in samples]
    if not samples:
        return [None] * len(x_values)

    # Compute bin half-width from x_values spacing
    step = x_values[1] - x_values[0] if len(x_values) >= 2 else 0.2
    half = step / 2.0

    binned: list[list[float]] = [[] for _ in x_values]
    for t, val in samples:
        # Find closest x bin
        idx = min(range(len(x_values)), key=lambda i: abs(x_values[i] - t))
        if abs(x_values[idx] - t) <= half:
            binned[idx].append(val)

    return [sum(b) / len(b) if b else None for b in binned]
