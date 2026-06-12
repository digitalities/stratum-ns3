# Linux TCP core provenance — pinned commit `67dc6c56b871`

This directory holds a frozen, citable snapshot of four Linux kernel TCP
core files used for a static cadence comparison between ns-3 and Linux
TCP, and consumed by probes targeting cross-flow phase coherence at the
variant-invariant TCP layer.

The pinned SHA is the same as `provenance/linux-sch-cake-67dc6c56b871/`
so that all "recent-stable" Linux references in the project anchor to a
single kernel snapshot (per ADR-0092).

## Pinned commit

- **Full SHA:** `67dc6c56b871617deac85b9f72500b69b1fdf835`
- **Short SHA:** `67dc6c56b871`
- **Commit subject:** net/sched: sch_cake: annotate data-races in cake_dump_class_stats (II)
- **Commit author:** Eric Dumazet
- **Commit date:** 2026-05-02
- **Upstream URL:** https://github.com/torvalds/linux/commit/67dc6c56b871617deac85b9f72500b69b1fdf835

## Files

| File | Lines | Purpose |
|---|---|---|
| `tcp_input.c` | 7767 | ACK reception, DelAck threshold logic, ATO tuning, congestion-control state machine, fast-retransmit predicate, reordering. The variant-invariant input-side path. |
| `tcp_output.c` | 4658 | `tcp_write_xmit` (the emission loop analog of ns-3 `SendPendingData`), `tcp_pace_kick` (pacing hrtimer), `tcp_send_delayed_ack` (RTT-adaptive ATO-based DelAck scheduling), `tcp_delack_max`. The variant-invariant output-side path. |
| `tcp_ipv4.c` | 3667 | `tcp_v4_rcv` (IP layer demux to socket; runs in BH/softirq context invoked from NAPI poll), `tcp_add_backlog` (the per-socket backlog when `sock_owned_by_user`). The NAPI-boundary entry point. |
| `inet_connection_sock.c` | 1563 | `inet_csk_init_xmit_timers`, `inet_csk_clear_xmit_timer`, `inet_csk_reset_xmit_timer`. The timer-management infrastructure shared by DelAck, RTO, keepalive. |

## What this captures

The four files together contain the **variant-invariant** TCP machinery:
the send-loop, the receive-side demux, the ACK-processing state machine,
the DelAck scheduling, the pacing/TSQ infrastructure, and the timer
management. They do NOT contain congestion-control variant implementations
(those live in `tcp_cubic.c`, `tcp_bbr.c`, etc., which are out of scope for
the cross-flow phase coherence question per the TCP-variant-invariance
prior established earlier in the project).

## Authoritative use

These files are **reference material** for static comparison and citation.
They are NEVER modified in place. Cite by `provenance/linux-tcp-67dc6c56b871/<file>:<line>`
per the dual-anchoring discipline.

## Fetched

2026-05-18 via `curl -sSL https://raw.githubusercontent.com/torvalds/linux/<SHA>/net/ipv4/<file>`.
Total: 17,655 lines.

## See also

- ADR-0107 — this provenance addition + static comparison rationale
- ADR-0092 — sch_cake.c provenance pattern (model for this addition)
- `provenance/linux-sch-cake-67dc6c56b871/` — sister provenance dir at the same SHA
