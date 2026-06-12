# iproute2 `q_cake.c` + `tc-cake(8)` provenance — pinned commit `87c66f79d8b0`

This directory holds a frozen, citable snapshot of iproute2's userspace
CAKE tooling per ADR-0093 (frozen iproute2 provenance, companion to
ADR-0092). It anchors the four S-tier specs that mirror the `tc-cake(8)`
command-line semantics and the `tc -s qdisc show cake` diagnostic
output — surfaces that live outside the kernel `sch_cake.c` snapshot at
`provenance/linux-sch-cake-67dc6c56b871/`.

## Pinned commit

- **Full SHA:** `87c66f79d8b09779c01c07122b0846f83b566dc1`
- **Short SHA:** `87c66f79d8b0`
- **Branch:** `main` HEAD at fetch time
- **Commit date:** 2026-05-07
- **Upstream URL:** https://github.com/iproute2/iproute2/commit/87c66f79d8b09779c01c07122b0846f83b566dc1
- **Raw blobs:**
  - `tc/q_cake.c` — https://raw.githubusercontent.com/iproute2/iproute2/87c66f79d8b09779c01c07122b0846f83b566dc1/tc/q_cake.c
  - `man/man8/tc-cake.8` — https://raw.githubusercontent.com/iproute2/iproute2/87c66f79d8b09779c01c07122b0846f83b566dc1/man/man8/tc-cake.8
- **Fetched:** 2026-05-15

## Snapshot scope

Two files are preserved verbatim, GPL-2.0 headers included. No excerpting,
no reformatting:

- `q_cake.c` — iproute2 userspace tc(8) plugin that parses `tc qdisc add
  dev … cake …` arguments and renders the kernel's CAKE statistics back
  to the operator. Defines the canonical name-to-mode mapping for
  flow-isolation modes (`flowblind`, `srchost`, `dsthost`, `hosts`,
  `flows`, `dual-srchost`, `dual-dsthost`, `triple-isolate`) and the
  textual layout of `tc -s qdisc show cake` output.
- `tc-cake.8` — operator-facing man page. Defines flag semantics
  authoritatively for substrate API contracts that mirror tc-cake.

Both surfaces are stable across iproute2 releases: flag spellings,
mode names, and the `tc -s` output ordering have not changed since the
2018 CAKE introduction (the man page header still records "19 July 2018"
as its content date even though the file has received maintenance
commits since).

## Why this snapshot, not the CAKE-paper deposit

The CAKE-paper Zenodo deposit (`10.5281/zenodo.1226887`, kernel SHA
`16d7fed7`) is the scientific anchor for **shape-equivalence claims**
(S-17.30 link-layer overhead, S-17.33 set-associative hash) — what the
peer-reviewed paper described, archived contemporaneously.

This iproute2 snapshot is the engineering anchor for **API-contract
claims** (S-17.35 srchost, S-17.38 mode round-trip, S-17.51 xstats
output, S-17.53 autorate-ingress) — what the operator-visible tc-cake
surface promises, anchored to a recent stable iproute2 to match the
discipline ADR-0092 established for the kernel side. The two anchors
serve different scientific roles; conflating them onto one SHA would
either dilute the paper-deposit claim or anchor structural specs to a
2018 surface that has received maintenance since.

## Citation pointers (line numbers verified at this SHA)

The line numbers below were verified against this SHA during the
iproute2-provenance-anchor cycle (2026-05-15). They are stable for this
snapshot; future snapshots at newer SHAs must re-verify before citing.

### `q_cake.c`

- `cake_flow_names[]` — string mapping for the eight flow-isolation
  modes consumed by `tc qdisc add … cake … <mode>`. Array entries on
  lines **51-58**. Cited by S-17.35, S-17.38.
- `cake_parse_opt` — argument parser that handles all tc-cake flags
  (`bandwidth`, `overhead`, `mpu`, `atm`, `raw`, `wash`, `memlimit`,
  `ack-filter`, `ack-filter-aggressive`, `triple-isolate`,
  `autorate-ingress`, …). Function definition line: **91**. Cited by
  S-17.53 (autorate-ingress API contract — the parser entry is the
  authoritative spelling of the flag name).
- `cake_print_xstats` — renders the per-tin diagnostic dump that
  `tc -s qdisc show cake` displays. Function definition line: **617**.
  Cited by S-17.51 (`DsCakeHelper::PrintTcStats` output-format mirror).
- `cake_qdisc_util` — the `struct qdisc_util` registration that wires
  the parser, the option printer, and `cake_print_xstats` into iproute2's
  tc(8) dispatcher. Line: **829**.

### `tc-cake.8`

- `.SH FLOW ISOLATION PARAMETERS` — operator-facing definition of the
  eight modes (`flowblind` / `flows` / `srchost` / `dsthost` / `hosts`
  / `dual-srchost` / `dual-dsthost` / `triple-isolate`). Section starts
  at line **460**. Cited by S-17.35, S-17.38.
- `.SH OTHER PARAMETERS` — operator-facing definition of
  `autorate-ingress`, `ingress`, `memlimit`, `ptm`. Section starts at
  line **600**. Cited by S-17.53.

## Update policy

This snapshot is **frozen** at SHA `87c66f79d8b0`. Newer snapshots may
be added side-by-side in future cycles (`provenance/linux-iproute2-<newer-sha>/`)
without modifying this directory. Specs and ADRs cite the SHA they were
drafted against.

## Related ADRs

- ADR-0092 — Frozen `sch_cake.c` provenance excerpt (kernel-side
  companion; defined the discipline this directory extends).
- ADR-0093 — Frozen iproute2 provenance (defines this snapshot; closes
  the userspace-surface gap surfaced by the 2026-05-14 discipline-coverage
  audit).
- ADR-0091 — "Divergence from reference" ADR template section.
- ADR-0088 — Reference-conformance fixtures.

## Citation pattern

Specs and ADRs cite this snapshot as:

> iproute2 `q_cake.c @ 87c66f79d8b0` (function `cake_print_xstats`, line 617)

or:

> `tc-cake(8) @ 87c66f79d8b0` (.SH FLOW ISOLATION PARAMETERS, line 460)

The short SHA is sufficient inside the repo; the full SHA is recorded
above for outside-the-repo reproducibility.
