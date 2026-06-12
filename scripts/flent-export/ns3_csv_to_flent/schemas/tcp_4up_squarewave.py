"""tcp_4up_squarewave: 4 TCP uploads with staggered start/stop, ICMP ping.

Mirrors CAKE 2018 paper Fig 4. Flows start at t=0/5/10/15 s and stop in
reverse order at t=45/50/55/60 s. The intended plot is a stacked-throughput
view that resembles a square wave when fairness holds.
"""
from __future__ import annotations

_TCP_UP = [
    ("TCP upload 1", "tcp_up_flow0.csv"),
    ("TCP upload 2", "tcp_up_flow1.csv"),
    ("TCP upload 3", "tcp_up_flow2.csv"),
    ("TCP upload 4", "tcp_up_flow3.csv"),
]
_PING = [("Ping (ms) ICMP", "ping_icmp.csv")]

_TCP_UP_NAMES = [n for n, _ in _TCP_UP]

TCP_4UP_SQUAREWAVE_SCHEMA: dict = {
    "name": "tcp_4up_squarewave",
    "title": "TCP 4-up square wave - DRR fairness under staggered load",
    "series": _TCP_UP + _PING,
    "totals": {
        "TCP upload sum": {"kind": "sum", "members": _TCP_UP_NAMES},
        "TCP upload avg": {"kind": "average", "members": _TCP_UP_NAMES},
    },
}
