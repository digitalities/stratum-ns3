"""host_isolation goldenfile round-trip: CSV bundle in, .flent.gz out."""
from __future__ import annotations

import gzip
import json
from pathlib import Path

from ns3_csv_to_flent.__main__ import main

FIXTURES = Path(__file__).parent / "fixtures"


def test_host_isolation_emit_produces_valid_flent_json(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "host_isolation",
        "--indir", str(FIXTURES / "host_isolation-golden-input"),
        "--output", str(out),
        "--title", "Golden host isolation",
    ])
    assert rc == 0
    assert out.exists()
    doc = json.loads(gzip.decompress(out.read_bytes()))
    assert doc["version"] == 4
    assert doc["metadata"]["NAME"] == "host_isolation"
    assert doc["metadata"]["TITLE"] == "Golden host isolation"
    assert "Host A flow 1" in doc["results"]
    assert "Host A flow 4" in doc["results"]
    assert "Host B flow 1" in doc["results"]
    assert "Host A total" in doc["results"]
    assert "Host B total" in doc["results"]


def test_host_isolation_x_values_match_input(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "host_isolation",
        "--indir", str(FIXTURES / "host_isolation-golden-input"),
        "--output", str(out),
    ])
    assert rc == 0
    doc = json.loads(gzip.decompress(out.read_bytes()))
    assert doc["x_values"] == [0.0, 0.5, 1.0, 1.5, 2.0]


def test_host_isolation_raw_values_carry_event_pairs(tmp_path):
    """Ping samples preserved as {t, val} dicts in raw_values."""
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "host_isolation",
        "--indir", str(FIXTURES / "host_isolation-golden-input"),
        "--output", str(out),
    ])
    assert rc == 0
    doc = json.loads(gzip.decompress(out.read_bytes()))
    raw_ping = doc["raw_values"]["Ping (ms) ICMP"]
    assert len(raw_ping) == 4
    first = raw_ping[0]
    last = raw_ping[-1]
    assert abs(first["t"] - 0.25) < 1e-6
    assert abs(first["val"] - 40.5) < 1e-6
    assert abs(last["t"] - 1.75) < 1e-6
    assert abs(last["val"] - 42.1) < 1e-6


def test_host_isolation_totals_sum_components(tmp_path):
    """Host A total = sum of host A flows; Host B total = host B flow."""
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "host_isolation",
        "--indir", str(FIXTURES / "host_isolation-golden-input"),
        "--output", str(out),
    ])
    assert rc == 0
    doc = json.loads(gzip.decompress(out.read_bytes()))
    a_flows = [doc["results"][f"Host A flow {i}"] for i in range(1, 5)]
    b_flow = doc["results"]["Host B flow 1"]
    a_total = doc["results"]["Host A total"]
    b_total = doc["results"]["Host B total"]
    n = len(doc["x_values"])
    for i in range(n):
        a_sum = sum(a_flows[k][i] for k in range(4) if a_flows[k][i] is not None)
        if a_total[i] is not None:
            assert abs(a_total[i] - a_sum) < 0.01, f"row {i}: a_total {a_total[i]} != sum {a_sum}"
        if b_total[i] is not None and b_flow[i] is not None:
            assert abs(b_total[i] - b_flow[i]) < 0.01, f"row {i}: b_total {b_total[i]} != {b_flow[i]}"
    # Spot check last row: A total = 4 * 12.5 = 50; B total = 50
    assert abs(a_total[-1] - 50.0) < 0.01
    assert abs(b_total[-1] - 50.0) < 0.01
