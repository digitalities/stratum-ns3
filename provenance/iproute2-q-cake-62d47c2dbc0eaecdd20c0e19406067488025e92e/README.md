# iproute2 `q_cake.c` provenance — pinned commit `62d47c2dbc0e`

This directory holds a frozen, citable snapshot of the iproute2 userspace
CAKE qdisc plugin at the time the DiffServ4NS CAKE Tier 1 feature parity
cycle was completed (2026-05-23). It anchors the two tables that underlie
the T1.2 LinkPreset enum and T1.3 RttPreset enum; documented in ADR-0117.

## Pinned commit

- **Full SHA:** `62d47c2dbc0eaecdd20c0e19406067488025e92e`
- **Short SHA:** `62d47c2dbc0e`
- **Upstream URL:** https://git.kernel.org/pub/scm/network/iproute2/iproute2.git
- **Fetched:** 2026-05-23

## Snapshot scope

One file is preserved verbatim, GPL-2.0 header included:

- `q_cake.c` — iproute2 userspace tc(8) plugin for the CAKE qdisc.
  Defines the canonical name-to-value mappings for link-layer overhead
  presets (`cake_link_layer_keywords[]`) and RTT presets (`presets[]`).

## Key tables (ADR-0117 citations)

- `cake_link_layer_keywords[]` — string-to-mode mapping for the fifteen
  link-layer overhead presets consumed by `tc qdisc add … cake … <mode>`.
  Cited by T1.2 LinkPreset enum in `src/ns-3/helper/ds-cake-helper.h`.

- `presets[]` — name + target-interval array for the eight RTT presets
  consumed by `tc qdisc add … cake … <preset>`. Cited by T1.3 RttPreset
  enum in `src/ns-3/helper/ds-cake-helper.h`.

## Relationship to `linux-iproute2-87c66f79d8b0`

The earlier snapshot `provenance/linux-iproute2-87c66f79d8b0/` (ADR-0093,
2026-05-15) anchors flow-isolation mode names and `tc -s` xstats output
format. This snapshot is a fresh pin for the Tier 1 feature-parity cycle
and specifically documents the link-layer and RTT preset tables that the
earlier snapshot's LINEAGE.md does not enumerate. Per the update policy in
ADR-0093, newer snapshots are added side-by-side without modifying the
previous directory.

## Related ADRs

- ADR-0117 — CAKE Tier 1 MikroTik feature parity (this snapshot's primary
  consumer; defines T1.2 LinkPreset, T1.3 RttPreset, T1.1 PTM, T1.4
  live bulk counter).
- ADR-0093 — Prior iproute2 provenance snapshot at `87c66f79d8b0`.
- ADR-0092 — Frozen `sch_cake.c` kernel-side companion.
