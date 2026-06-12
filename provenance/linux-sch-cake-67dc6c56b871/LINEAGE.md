# Linux `sch_cake.c` provenance — pinned commit `67dc6c56b871`

This directory holds a frozen, citable snapshot of Linux mainline
`net/sched/sch_cake.c` per ADR-0092 (frozen `sch_cake.c` provenance).

## Pinned commit

- **Full SHA:** `67dc6c56b871617deac85b9f72500b69b1fdf835`
- **Short SHA:** `67dc6c56b871`
- **Commit subject:** net/sched: sch_cake: annotate data-races in cake_dump_class_stats (II)
- **Commit author:** Eric Dumazet
- **Commit date:** 2026-05-02
- **Upstream URL:** https://github.com/torvalds/linux/commit/67dc6c56b871617deac85b9f72500b69b1fdf835
- **Raw blob:** https://raw.githubusercontent.com/torvalds/linux/67dc6c56b871617deac85b9f72500b69b1fdf835/net/sched/sch_cake.c
- **Fetched:** 2026-05-14

## Snapshot scope

The full file is preserved verbatim, GPL-2.0 header included. No excerpting,
no reformatting. ADR-0092 §Decision documents the rationale (full-file
preserves token-splitting, sparse-vs-bulk reclassification, and host-load
refcount mechanics that excerpts lose).

## Citation pointers (line numbers verified at this SHA)

The line numbers below were verified against this SHA during Task 1 of the
G method-disciplines cycle (2026-05-14). They are stable for this snapshot;
future snapshots at newer SHAs must re-verify before citing.

- `cake_dequeue` function — canonical Shreedhar-Varghese DRR with cursor
  advance atomic with deficit top-up. Function definition line: **2010**.
- `cake_get_flow_quantum` (per-flow quantum scaling, `quantum >> ilog2(host_load)`).
  Function definition line: **688**.

## Update policy

This snapshot is **frozen** at SHA `67dc6c56b871`. Newer snapshots may be
added side-by-side in future cycles (`provenance/linux-sch-cake-<newer-sha>/`)
without modifying this directory. ADRs cite the SHA they were drafted against.

## Related ADRs

- ADR-0088 — Reference-conformance fixtures (cites this snapshot as the CAKE
  reference source).
- ADR-0091 — "Divergence from reference" ADR template section (cites this
  snapshot in the worked example on ADR-0047).
- ADR-0092 — Frozen `sch_cake.c` provenance excerpt (defines this discipline).
- ADR-0047 — Host isolation via nested-FQ wrapper (retroactive
  "Divergence from reference" addendum cites this snapshot).

## Citation pattern

ADRs cite this snapshot as:

> Linux `sch_cake.c @ 67dc6c56b871` (function `cake_dequeue`, line N)

The short SHA is sufficient inside the repo; the full SHA is recorded above
for outside-the-repo reproducibility.
