# Changelog

All notable changes to the Stratum ns-3 module are documented here.
This project follows [Semantic Versioning](https://semver.org).

## v1.2 — 2026-06-16

This release adds a reproducible CAKE host-fairness example and regression test,
and revises the handbook. The substrate model is unchanged from v1.1.

### Reproducibility

- **CAKE host-fairness, reproducible from the module.** A new example and a
  gated regression test reproduce the CAKE host-fairness result directly from
  the module: the per-host bandwidth share and backlog occupancy. Earlier this
  needed external measurement scripts. The handbook's CAKE evidence chapter
  covers the analysis in full.

### Handbook

- Tightened notation and terminology across all three parts.
- Aligned the CAKE evidence chapter with the reproducible host-fairness result
  and added a figure of the share-versus-throughput trade-off.
- Filled deferred recipe walkthroughs and their estimated run times in the
  introductory chapters.

## v1.1 — 2026-06-12

Stratum is the standalone, actively developed home of the ns-3 QoS
substrate that composes Differentiated Services, L4S, and CAKE as three
first-class clients of one edge queue disc. It carries forward the
[DiffServ4NS](https://github.com/digitalities/diffserv4ns) lineage, where
the 2001 ns-2 module and the frozen `v1.0-icns3-submission` ns-3 snapshot
are preserved. This single entry records the substrate as shipped here and
how it differs from that snapshot; the granular development history lives
upstream. Built and tested against the pinned ns-3 release
(`scripts/fetch-ns3.sh --print-pin`).

### Difference from the diffserv4ns repository

- **Standalone ns-3 module.** Stratum ships as a drop-in ns-3 contrib
  module — `model/`, `helper/`, `test/`, `examples/`, and `CMakeLists.txt`
  at the repository root — independent of the ns-2 sources, the paper, and
  the lineage archive that the diffserv4ns repository carries.
- **`ns3::stratum` namespace and rename.** The substrate moved from
  `ns3::diffserv` (`Ds*` class names, contrib module `diffserv`) to the
  `ns3::stratum` namespace hierarchy — `ns3::stratum::{diffserv,l4s,cake}` —
  with prefix-free class names, `stratum-` file names, and a `stratum`
  build target. The diffserv4ns snapshot retains the pre-rename names.
- **RFC 9332 L4S coupling correction.** The DualPI2 coupled cascade now
  matches RFC 9332 Section 2.1 equation (1): classic coupled-drop
  probability `p'^2` (was `(k*p')^2`), L4S coupled-mark probability
  `min(k*p', 1)` honouring the `CouplingFactor` attribute (was a hardcoded
  factor 2), and PI gains at the RFC defaults (`alpha = 0.16 Hz`,
  `beta = 3.2 Hz`). Pinned by a golden controller vector, ECN and
  DSCP-preservation checks, and a three-seed fairness-parity gate against a
  reference DualPI2 implementation.
- **CAKE fidelity corrections.** DSCP-to-tin maps reproduce the Linux
  `sch_cake` lookup tables verbatim; the diffserv3 Latency-Sensitive share
  follows the kernel quantum ladder; `SetBandwidth` resolves diffserv3 and
  diffserv8 via a compose-time profile record; host-pair isolation counts
  bulk flows across the sparse-to-bulk transition with per-side slot
  resolution; ACK filtering reaches parity for IPv6; and a byte-exact
  autorate-ingress peak-bandwidth estimator with its measured convergence
  account.
- **Scheduler and harness corrections.** Corrected WF2Q+ and PQ
  attributions and round-robin documentation; spec-tier gates and a
  classic-lane collapse fix in the multi-AQM evaluation harness; and a
  buffer-adequate host-isolation validation gate.

### Substrate

Four pluggable per-node primitives — Classify-and-Meter, Mark-and-Route, a
per-class slot array, and an across-slot service policy — let each client
build a queueing discipline by selecting one strategy per stage.

### DiffServ client

- RFC 2697 (sr-TCM), RFC 2698 (tr-TCM), and TSW2CM/TSW3CM meters.
- DSCP classification and PHB marking with a configurable PHB table.
- RED / RIO / drop-tail inner queue discs.
- Schedulers: PQ, WRR, WFQ, WF2Q+, SCFQ, SFQ, LLQ, and hybrid-LLQ.

### L4S client

- DualPI2 coupled AQM (RFC 9332) with ECT(1) classification (RFC 9331).
- Coupled classic / scalable congestion marking.

### CAKE client

- DSCP-to-tin mapping (besteffort, precedence, diffserv3, diffserv4,
  diffserv8) over per-tin FqCobalt with DRR++ scheduling.
- Host-pair isolation, ACK filtering, optional per-tin shaping, named
  link-layer-overhead and RTT presets, and per-tin diagnostics aligned with
  `tc -s qdisc show`.

### Validation

- 17 test suites covering the three clients, RFC conformance vectors
  (RFC 2697 / 2698 / 2859 / 9331 / 9332), and cross-implementation reference
  fixtures.
- Reproduction tooling for the accompanying paper's figures
  (`scripts/reproduce-paper.sh`).

### Requirements

- A small set of ns-3 mainline patches (bundled in `patches/ns3/`), applied
  at the pinned revision. See `README.md` for the three install paths: into
  an existing ns-3 tree, a script-managed sibling clone, or Bake.
