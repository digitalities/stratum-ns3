"""tcp_download goldenfile round-trip."""
from __future__ import annotations

import gzip
import json
from pathlib import Path

from ns3_csv_to_flent.__main__ import main

FIXTURES = Path(__file__).parent / "fixtures"


def test_tcp_download_emit_produces_valid_flent_json(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "tcp_download",
        "--indir", str(FIXTURES / "tcp_download-golden-input"),
        "--output", str(out),
    ])
    assert rc == 0
    assert out.exists()
    doc = json.loads(gzip.decompress(out.read_bytes()))

    assert doc["version"] == 4
    assert doc["metadata"]["TITLE"] == "TCP download"
    assert doc["metadata"]["NAME"] == "tcp_download"

    # Single TCP series + single ping series; no totals
    assert set(doc["raw_values"].keys()) == {"TCP download", "Ping (ms) ICMP"}

    # Goodput values: 5.0, 5.5, 6.0, 6.5, 7.0
    expected_tcp = [5.0, 5.5, 6.0, 6.5, 7.0]
    assert len(doc["results"]["TCP download"]) == 5
    for got, want in zip(doc["results"]["TCP download"], expected_tcp):
        assert abs(got - want) < 1e-9
