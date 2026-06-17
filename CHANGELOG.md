# Changelog

All notable changes to the Stratum ns-3 module are documented here.
This project follows [Semantic Versioning](https://semver.org).

## v1.3.0 — 2026-06-17

Primarily a correctness release. An adversarial review of the substrate against its
primary references (the RFCs, Linux `sch_cake.c`, the reference DualPI2, and the
fair-queueing papers) found and fixed a set of behavioural defects, each pinned by a
regression test. It also adds one backward-compatible option — a configurable TSW
averaging-window length (hence a minor, not a patch, release). No breaking API
changes. The most significant changes are the WFQ scheduler losing weighted fairness
after a full drain and reactivation, and the L4S queue now applying its congestion
signal at dequeue from each packet's own sojourn time; both alter the output of
affected simulations.

### Schedulers

- **WFQ:** fixed a virtual-time blow-up after the busy set fully drains and a flow
  reactivates. With non-integer weights the running weight sum retained a
  one-ULP floating-point residue, which drove the virtual time to roughly 1e16 on
  the next arrival and collapsed weighted service to strict priority by queue
  index. The weight sum is now pinned to zero whenever the queue empties (Parekh &
  Gallager 1993).
- **LLQ:** the priority-lane rate cap now engages and releases correctly. The inner
  priority scheduler never observed its own departures, and its lone queue's
  work-conserving fallback served the priority lane even over the cap; the lane is
  now policed to its configured rate and is served again once its measured rate
  decays back under the cap (RFC 3246).
- **Hybrid-LLQ across-tin DRR:** the round-robin cursor now advances off a slot
  that drains to empty (matching the pure-DRR sibling and Linux `cake_dequeue`), so
  a flow that bounces on and off empty no longer monopolises service.

### CAKE

- **ACK filter:** a data-bearing (piggybacked) ACK, or one carrying an unknown,
  malformed, or truncated TCP option, is now a valid filtering trigger, matching
  `sch_cake.c`; the pure-ACK and option checks apply to the queued drop candidates,
  not the triggering packet.
- **Autorate-ingress:** the rate reconfigure now fires on each measurement-window
  close after the shaper warm-up, matching the kernel's effective cadence, instead
  of on a rolling per-packet timer; the window byte count uses the raw packet
  length, as the kernel does.
- **diffserv4 DSCP map (rate-based shaper):** corrected the tin mapping for four
  code points (DSCP 4, 10, 12, 14) to match the Linux `diffserv4[]` table the
  default composition already uses.
- **diffserv-precedence DRR weights:** the per-tin weights now follow the Linux
  `sch_cake` precedence quantum ladder; the earlier weights were ordered against
  the kernel's.

### L4S

- **Dequeue-time marking:** the L4S congestion signal is now applied at dequeue,
  from each packet's own queue-sojourn time, rather than at enqueue from the
  head-of-queue packet; the coupled mark and drop are suppressed while the standing
  queue is below two MTUs. This matches the reference DualPI2.
- **DualPI2 controller:** the proportional term is now applied against an initial
  previous-queue of zero on the first controller tick, matching the reference
  DualPI2 implementation and the RFC 9332 pseudocode.

### Meters and edge

- **Flow-weighted meter:** the deterministic, probabilistic, and periodic penalty
  modes are now honoured on the classification path (previously every configuration
  behaved deterministically).
- **DSCP routing:** the code-point index is masked to six bits on the enqueue path
  so a misconfigured value cannot read past the routing table.
- **RED / RIO sub-queue:** the per-precedence state array is now bounds-checked on
  the RIO and PHB paths, closing a latent out-of-bounds access from a misconfigured
  precedence index.
- **TSW meters:** the averaging-window length is now configurable through the
  helper (previously fixed at one second).

### Tests

- **Host-isolation jitter-floor test:** re-grounded a fragile single-replica band
  to the mean of five jitter replicas (a single replica of this chaotic scenario
  is not portable across hosts). Behaviour is unchanged; the band is now derived
  from the measured ensemble rather than one low replica.

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
