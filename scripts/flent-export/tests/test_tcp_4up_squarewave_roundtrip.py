"""tcp_4up_squarewave goldenfile round-trip: CSV bundle in, .flent.gz out."""
from __future__ import annotations

import gzip
import json
from pathlib import Path

from ns3_csv_to_flent.__main__ import main

FIXTURES = Path(__file__).parent / "fixtures"


def test_squarewave_emit_produces_valid_flent_json(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "tcp_4up_squarewave",
        "--indir", str(FIXTURES / "tcp_4up_squarewave-golden-input"),
        "--output", str(out),
        "--title", "Golden TCP 4-up square wave",
    ])
    assert rc == 0
    assert out.exists()
    doc = json.loads(gzip.decompress(out.read_bytes()))

    # Required Flent v4 keys
    assert doc["version"] == 4
    assert doc["metadata"]["TITLE"] == "Golden TCP 4-up square wave"
    assert doc["metadata"]["NAME"] == "tcp_4up_squarewave"
    assert "TCP upload 1" in doc["results"]
    assert "TCP upload 4" in doc["results"]
    assert "Ping (ms) ICMP" in doc["results"]
    assert "TCP upload sum" in doc["results"]


def test_squarewave_raw_values_carry_event_pairs(tmp_path):
    """Ping samples preserved as {t, val} dicts in raw_values."""
    out = tmp_path / "out.flent.gz"
    main([
        "--test", "tcp_4up_squarewave",
        "--indir", str(FIXTURES / "tcp_4up_squarewave-golden-input"),
        "--output", str(out),
    ])
    doc = json.loads(gzip.decompress(out.read_bytes()))
    icmp = doc["raw_values"]["Ping (ms) ICMP"]
    assert len(icmp) == 5
    # First sample: t=0.1, rtt=40.5
    assert abs(icmp[0]["t"] - 0.1) < 1e-9
    assert abs(icmp[0]["val"] - 40.5) < 1e-9
    # Last sample: t=0.9, rtt=43.5
    assert abs(icmp[-1]["t"] - 0.9) < 1e-9
    assert abs(icmp[-1]["val"] - 43.5) < 1e-9


def test_squarewave_totals_sum_components(tmp_path):
    """TCP upload sum at each step equals sum of individual upload flows."""
    out = tmp_path / "out.flent.gz"
    main([
        "--test", "tcp_4up_squarewave",
        "--indir", str(FIXTURES / "tcp_4up_squarewave-golden-input"),
        "--output", str(out),
    ])
    doc = json.loads(gzip.decompress(out.read_bytes()))

    upload_total = doc["results"]["TCP upload sum"]
    flow_results = [
        doc["results"][n]
        for n in ("TCP upload 1", "TCP upload 2", "TCP upload 3", "TCP upload 4")
    ]
    for step in range(len(upload_total)):
        present = [f[step] for f in flow_results if f[step] is not None]
        if present:
            expected = sum(present)
            assert abs(upload_total[step] - expected) < 1e-9
        else:
            assert upload_total[step] is None


def test_squarewave_x_values_match_input(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "tcp_4up_squarewave",
        "--indir", str(FIXTURES / "tcp_4up_squarewave-golden-input"),
        "--output", str(out),
    ])
    assert rc == 0
    doc = json.loads(gzip.decompress(out.read_bytes()))
    assert doc["x_values"] == [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]
