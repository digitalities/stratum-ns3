"""host_isolation: 4 flows on host A, 1 flow on host B, ICMP ping.

Designed to expose the throughput-per-host signal that distinguishes
flowblind hashing (proportional to flow count) from triple-isolate
hashing (equal per host) under CAKE.

Maps host A's 4 flows to tcp_up_flow{0..3}.csv and host B's single flow
to tcp_up_flow4.csv. The ns-3 example uses
DsFlentCsvSink::AddTcpUpFlow(idx, sink) with idx 0..4; no sink API
change is required.
"""
from __future__ import annotations

_HOST_A = [
    ("Host A flow 1", "tcp_up_flow0.csv"),
    ("Host A flow 2", "tcp_up_flow1.csv"),
    ("Host A flow 3", "tcp_up_flow2.csv"),
    ("Host A flow 4", "tcp_up_flow3.csv"),
]
_HOST_B = [
    ("Host B flow 1", "tcp_up_flow4.csv"),
]
_PING = [("Ping (ms) ICMP", "ping_icmp.csv")]

_HOST_A_NAMES = [n for n, _ in _HOST_A]
_HOST_B_NAMES = [n for n, _ in _HOST_B]

HOST_ISOLATION_SCHEMA: dict = {
    "name": "host_isolation",
    "title": "Host isolation - 4 flows on host A vs 1 flow on host B",
    "series": _HOST_A + _HOST_B + _PING,
    "totals": {
        "Host A total": {"kind": "sum", "members": _HOST_A_NAMES},
        "Host B total": {"kind": "sum", "members": _HOST_B_NAMES},
    },
}
