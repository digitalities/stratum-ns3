"""RRUL goldenfile round-trip: CSV bundle in, .flent.gz out, reload, verify.

Asserts the Flent v4 JSON layout (top-level ``version``, uppercase
``metadata`` keys, results aligned to ``x_values``).
"""
from __future__ import annotations

import gzip
import json
from pathlib import Path

from ns3_csv_to_flent.__main__ import main

FIXTURES = Path(__file__).parent / "fixtures"


def test_rrul_emit_produces_valid_flent_json(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "rrul",
        "--indir", str(FIXTURES / "rrul-golden-input"),
        "--output", str(out),
        "--title", "Golden RRUL",
    ])
    assert rc == 0
    assert out.exists()
    doc = json.loads(gzip.decompress(out.read_bytes()))

    # Required Flent v4 keys
    assert doc["version"] == 4
    assert doc["metadata"]["TITLE"] == "Golden RRUL"
    assert doc["metadata"]["NAME"] == "rrul"
    assert "TIME" in doc["metadata"]
    assert "LENGTH" in doc["metadata"]
    assert "STEP_SIZE" in doc["metadata"]
    assert "TOTAL_LENGTH" in doc["metadata"]
    assert "SERIES_META" in doc["metadata"]
    assert doc["metadata"]["LOCAL_HOST"]
    assert doc["metadata"]["HOST"]
    assert "x_values" in doc
    assert "raw_values" in doc
    assert "results" in doc

    # 13 individual series; aggregates appear in results too
    expected_series = {
        "TCP download BE", "TCP download BK",
        "TCP download CS5", "TCP download EF",
        "TCP upload BE", "TCP upload BK",
        "TCP upload CS5", "TCP upload EF",
        "Ping (ms) ICMP",
        "Ping (ms) UDP BE", "Ping (ms) UDP BK",
        "Ping (ms) UDP CS5", "Ping (ms) UDP EF",
    }
    # Per-flow series live in raw_values; aggregates land alongside
    raw_keys = set(doc["raw_values"].keys())
    assert expected_series.issubset(raw_keys)
    # Aggregates exist in results
    assert "TCP download sum" in doc["results"]
    assert "TCP upload sum" in doc["results"]
    assert "TCP totals" in doc["results"]
    assert "Ping (ms) avg" in doc["results"]

    # Per-series UNITS metadata is populated
    sm = doc["metadata"]["SERIES_META"]
    assert sm["TCP download BE"]["UNITS"] == "Mbits/s"
    assert sm["Ping (ms) ICMP"]["UNITS"] == "ms"


def test_rrul_totals_sum_components(tmp_path):
    out = tmp_path / "out.flent.gz"
    main([
        "--test", "rrul",
        "--indir", str(FIXTURES / "rrul-golden-input"),
        "--output", str(out),
    ])
    doc = json.loads(gzip.decompress(out.read_bytes()))

    # TCP download sum at step i = sum of BE/BK/CS5/EF at step i
    download_total = doc["results"]["TCP download sum"]
    flow_results = [
        doc["results"][n]
        for n in ("TCP download BE", "TCP download BK",
                  "TCP download CS5", "TCP download EF")
    ]
    assert len(download_total) == 5
    for step in range(len(download_total)):
        present = [f[step] for f in flow_results if f[step] is not None]
        if present:
            expected = sum(present)
            assert abs(download_total[step] - expected) < 1e-9
        else:
            assert download_total[step] is None

    upload_total = doc["results"]["TCP upload sum"]
    flow_results = [
        doc["results"][n]
        for n in ("TCP upload BE", "TCP upload BK",
                  "TCP upload CS5", "TCP upload EF")
    ]
    for step in range(len(upload_total)):
        present = [f[step] for f in flow_results if f[step] is not None]
        if present:
            expected = sum(present)
            assert abs(upload_total[step] - expected) < 1e-9
        else:
            assert upload_total[step] is None


def test_rrul_x_values_match_input(tmp_path):
    out = tmp_path / "out.flent.gz"
    main([
        "--test", "rrul",
        "--indir", str(FIXTURES / "rrul-golden-input"),
        "--output", str(out),
    ])
    doc = json.loads(gzip.decompress(out.read_bytes()))
    assert doc["x_values"] == [0.2, 0.4, 0.6, 0.8, 1.0]


def test_rrul_raw_values_carry_event_pairs(tmp_path):
    """Ping samples preserved as {t, val} dicts in raw_values."""
    out = tmp_path / "out.flent.gz"
    main([
        "--test", "rrul",
        "--indir", str(FIXTURES / "rrul-golden-input"),
        "--output", str(out),
    ])
    doc = json.loads(gzip.decompress(out.read_bytes()))
    icmp = doc["raw_values"]["Ping (ms) ICMP"]
    assert len(icmp) == 5
    # First sample: t=0.18, rtt=20.0
    assert abs(icmp[0]["t"] - 0.18) < 1e-9
    assert abs(icmp[0]["val"] - 20.0) < 1e-9
    # Last sample: t=0.98, rtt=24.0
    assert abs(icmp[-1]["t"] - 0.98) < 1e-9
    assert abs(icmp[-1]["val"] - 24.0) < 1e-9
