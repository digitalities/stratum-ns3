"""RRUL test: 4 TCP down + 4 TCP up + ICMP ping + 4 UDP probes.

Series names mirror Flent's canonical ``rrul.conf`` naming so that
``flent --plot`` can dispatch on test ``rrul`` without renaming. The
four TCP flows map to the four DSCP markings used by the upstream
Flent rrul test (BE / BK / CS5 / EF). Our cake-rrul example does not
mark traffic, so the labels are positional aliases for flow0..flow3
rather than DSCP-derived; the relabelling is purely for plot
dispatch compatibility.
"""
from __future__ import annotations

# Positional alias: flow0 -> BE, flow1 -> BK, flow2 -> CS5, flow3 -> EF.
_TCP_DOWN = [
    ("TCP download BE", "tcp_down_flow0.csv"),
    ("TCP download BK", "tcp_down_flow1.csv"),
    ("TCP download CS5", "tcp_down_flow2.csv"),
    ("TCP download EF", "tcp_down_flow3.csv"),
]
_TCP_UP = [
    ("TCP upload BE", "tcp_up_flow0.csv"),
    ("TCP upload BK", "tcp_up_flow1.csv"),
    ("TCP upload CS5", "tcp_up_flow2.csv"),
    ("TCP upload EF", "tcp_up_flow3.csv"),
]
_UDP_PROBES = [
    ("Ping (ms) UDP BE", "udp_probe_flow0.csv"),
    ("Ping (ms) UDP BK", "udp_probe_flow1.csv"),
    ("Ping (ms) UDP CS5", "udp_probe_flow2.csv"),
    ("Ping (ms) UDP EF", "udp_probe_flow3.csv"),
]
_PING = [("Ping (ms) ICMP", "ping_icmp.csv")]


_TCP_DOWN_NAMES = [n for n, _ in _TCP_DOWN]
_TCP_UP_NAMES = [n for n, _ in _TCP_UP]
_PING_ALL_NAMES = [n for n, _ in _PING + _UDP_PROBES]


RRUL_SCHEMA: dict = {
    "name": "rrul",
    "title": "RRUL - Realtime Response Under Load",
    "series": _TCP_DOWN + _TCP_UP + _PING + _UDP_PROBES,
    "totals": {
        "TCP download sum": {"kind": "sum", "members": _TCP_DOWN_NAMES},
        "TCP upload sum": {"kind": "sum", "members": _TCP_UP_NAMES},
        "TCP download avg": {"kind": "average", "members": _TCP_DOWN_NAMES},
        "TCP upload avg": {"kind": "average", "members": _TCP_UP_NAMES},
        "TCP totals": {
            "kind": "sum",
            "members": _TCP_DOWN_NAMES + _TCP_UP_NAMES,
        },
        "Ping (ms) avg": {"kind": "average", "members": _PING_ALL_NAMES},
    },
}
