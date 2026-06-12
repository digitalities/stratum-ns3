"""tcp_upload test: single TCP upload flow + ICMP ping."""
from __future__ import annotations

TCP_UPLOAD_SCHEMA: dict = {
    "name": "tcp_upload",
    "title": "TCP upload",
    "series": [
        ("TCP upload", "tcp_up.csv"),
        ("Ping (ms) ICMP", "ping_icmp.csv"),
    ],
    "totals": {},
}
