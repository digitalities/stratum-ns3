"""tcp_download test: single TCP download flow + ICMP ping."""
from __future__ import annotations

TCP_DOWNLOAD_SCHEMA: dict = {
    "name": "tcp_download",
    "title": "TCP download",
    "series": [
        ("TCP download", "tcp_down.csv"),
        ("Ping (ms) ICMP", "ping_icmp.csv"),
    ],
    "totals": {},
}
