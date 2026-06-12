"""tcp_upload goldenfile round-trip."""
from __future__ import annotations

import gzip
import json
from pathlib import Path

from ns3_csv_to_flent.__main__ import main

FIXTURES = Path(__file__).parent / "fixtures"


def test_tcp_upload_emit_produces_valid_flent_json(tmp_path):
    out = tmp_path / "out.flent.gz"
    rc = main([
        "--test", "tcp_upload",
        "--indir", str(FIXTURES / "tcp_upload-golden-input"),
        "--output", str(out),
    ])
    assert rc == 0
    assert out.exists()
    doc = json.loads(gzip.decompress(out.read_bytes()))

    assert doc["version"] == 4
    assert doc["metadata"]["TITLE"] == "TCP upload"
    assert doc["metadata"]["NAME"] == "tcp_upload"

    assert set(doc["raw_values"].keys()) == {"TCP upload", "Ping (ms) ICMP"}

    # Goodput values: 4.0, 4.5, 5.0, 5.5, 6.0
    expected_tcp = [4.0, 4.5, 5.0, 5.5, 6.0]
    for got, want in zip(doc["results"]["TCP upload"], expected_tcp):
        assert abs(got - want) < 1e-9
