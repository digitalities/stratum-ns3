# Changelog

All notable changes to the Stratum ns-3 module are documented here.
This project follows [Semantic Versioning](https://semver.org).

## [Unreleased]

_Nothing yet._

## v2.0.0 — 2026-06-24

A feature and API release. The headline is IPv6 support across all three clients;
the configuration API is reshaped onto C++23 designated-initializer structs, which
retires the positional helpers and makes this a major (source-breaking) release. The
queueing and AQM behaviour of existing IPv4 scenarios is unchanged.

### Added

- IPv6: DiffServ, L4S, and CAKE classify, mark, meter, and shape over IPv6 as well as IPv4, through address-family-agnostic DS-field and ECN accessors.
- `diffserv::Helper::SetAsDiffserv` and `l4s::Helper::SetAsL4s`: preset edge composers that build a configured client edge in one call (alongside `cake::Helper::SetAsCake*`).
- `stratum::InstallRoot`: one cross-client primitive to install a built edge as a device's root queue disc.
- `CakeOptions`: a designated-initializer struct for the `SetAsCake*` composers.
- Example recipes for DiffServ, L4S, and CAKE over IPv6, plus a CAKE+L4S IPv6 composition.
- A generated command-line reference (handbook Appendix E) built from each example's `--PrintHelp`, with a `--check` drift guard in the audit gate.

### Changed

- **Breaking:** the configuration API moves to designated-initializer structs — `RedQueueConfig` (`ConfigQueue`), the per-meter policy specs (sr-TCM / tr-TCM / TSW2CM / TSW3CM / token-bucket), `PolicerEntry`, the sr-TCM meter rule, and `MarkRule`; the positional helper overloads are retired.
- **Breaking:** `MarkRule` source/destination matching uses a typed, family-aware `AddrMatch` (exact IPv4 or IPv6); the legacy `int32_t` / any-host sentinel convention is removed.
- `SetMredMode`'s all-queues form splits into `SetMredModeAllQueues`.

## v1.3.0 — 2026-06-17

Correctness release: behavioural defects fixed against the primary references (RFCs,
Linux `sch_cake.c`, the reference DualPI2, the fair-queueing papers), each pinned by a
regression test, plus one backward-compatible option (configurable TSW window). No
breaking API changes. The WFQ and L4S fixes change the output of affected simulations.

### Schedulers

- **WFQ:** fixed a virtual-time blow-up after a full drain and reactivation that collapsed weighted service to strict priority (a non-integer-weight ULP residue); the weight sum is pinned to zero when the queue empties.
- **LLQ:** the priority-lane rate cap now engages and releases correctly (the inner scheduler now observes its own departures).
- **Hybrid-LLQ across-tin DRR:** the round-robin cursor advances off a slot that drains empty, matching the pure-DRR sibling and Linux `cake_dequeue`.

### CAKE

- **ACK filter:** a data-bearing ACK, or one with an unknown/malformed/truncated TCP option, is now a valid filtering trigger, matching `sch_cake.c`.
- **Autorate-ingress:** the rate reconfigure fires on each measurement-window close after warm-up (kernel cadence), not on a per-packet timer.
- **diffserv4 DSCP map (rate-based shaper):** corrected four code points (DSCP 4, 10, 12, 14) to the Linux `diffserv4[]` table.
- **diffserv-precedence DRR weights:** now follow the Linux `sch_cake` precedence quantum ladder.

### L4S

- **Dequeue-time marking:** the congestion signal is applied at dequeue from each packet's own sojourn, suppressed below a two-MTU standing queue, matching the reference DualPI2.
- **DualPI2 controller:** the proportional term uses a zero previous-queue on the first tick, matching the reference and the RFC 9332 pseudocode.

### Meters and edge

- **Flow-weighted meter:** the deterministic, probabilistic, and periodic penalty modes are honoured (previously all behaved deterministically).
- **DSCP routing:** the code-point index is masked to six bits on enqueue.
- **RED / RIO sub-queue:** the per-precedence state array is bounds-checked on the RIO and PHB paths.
- **TSW meters:** the averaging-window length is configurable through the helper (was fixed at one second).

### Tests

- **Host-isolation jitter-floor test:** re-grounded a fragile single-replica band to the mean of five jitter replicas (behaviour unchanged).

## v1.2 — 2026-06-16

Adds a reproducible CAKE host-fairness example and regression test, and revises the
handbook. The substrate model is unchanged from v1.1.

### Reproducibility

- **CAKE host-fairness, reproducible from the module:** a new example and gated regression test reproduce the per-host share and backlog occupancy directly (previously external measurement scripts).

### Handbook

- Tightened notation and terminology across all three parts.
- Aligned the CAKE evidence chapter with the reproducible host-fairness result; added a share-versus-throughput figure.
- Filled the deferred recipe walkthroughs and their run-time estimates.

## v1.1 — 2026-06-12

First standalone release of the ns-3 QoS substrate composing DiffServ, L4S, and CAKE
as three first-class clients of one edge queue disc, carried forward from the
[DiffServ4NS](https://github.com/digitalities/diffserv4ns) lineage (the 2001 ns-2
module and the frozen `v1.0-icns3-submission` snapshot live upstream). Built against
the pinned ns-3 release (`scripts/fetch-ns3.sh --print-pin`).

### Difference from the diffserv4ns repository

- **Standalone ns-3 module:** ships as a drop-in contrib module (`model/`, `helper/`, `test/`, `examples/`, `CMakeLists.txt` at the root), independent of the ns-2 sources, paper, and lineage archive.
- **`ns3::stratum` namespace and rename:** moved from `ns3::diffserv` / `Ds*` to the `ns3::stratum::{diffserv,l4s,cake}` hierarchy, with prefix-free class names, `stratum-` file names, and a `stratum` build target.
- **RFC 9332 L4S coupling correction:** classic coupled-drop `p'^2` (was `(k*p')^2`), L4S coupled-mark `min(k*p', 1)` honouring `CouplingFactor` (was a hardcoded factor 2), PI gains at RFC defaults (`alpha = 0.16 Hz`, `beta = 3.2 Hz`); pinned by a golden vector, ECN/DSCP checks, and a three-seed fairness-parity gate.
- **CAKE fidelity corrections:** Linux-verbatim DSCP-to-tin maps; diffserv3 Latency-Sensitive share on the kernel quantum ladder; `SetBandwidth` via a compose-time profile record; host-pair isolation across the sparse-to-bulk transition; IPv6 ACK-filter parity; a byte-exact autorate-ingress estimator.
- **Scheduler and harness corrections:** corrected WF2Q+ / PQ attributions and round-robin docs; spec-tier gates and a classic-lane collapse fix in the multi-AQM harness; a buffer-adequate host-isolation gate.

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
