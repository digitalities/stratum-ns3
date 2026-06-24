---
title: The L4S client
origin: 2026-written
status: filled
last-updated: 2026-06-07
---

# The L4S client

> **Hands-on**: see [L4S recipes](I-04-l4s.md) for runnable recipes; see
> [L4S validation](III-03-l4s.md) for the evidence behind the claims made here.

This chapter describes the L4S client of the Stratum substrate: what L4S is,
why it belongs in the substrate, how the DualPI2 coupled AQM is realised
across the four strategy slots, and where the composition can be extended.
It is the architecture companion to the [Stratum architecture
chapter](II-02-stratum-architecture.md) — read that first for the four-slot
vocabulary used throughout.

## What L4S is

L4S replaces the classic "drop = congestion signal" contract with a
finer-grained ECN-based scheme:

| Mechanism | Classic DiffServ | L4S |
|---|---|---|
| **Classification** | DSCP field (RFC 2474) | ECN code point ECT(1) (RFC 9331) |
| **Signalling** | WRED drops | CE-mark above a shallow target sojourn |
| **Sender behaviour** | TCP NewReno, cwnd /= 2 per CE or drop | Scalable CC (DCTCP, TCP Prague): cwnd reacts to per-packet marks in proportion |
| **Coupling** | n/a | DualPI2: one base probability $p'$ feeds both queues — classic drop $p_C = p'^2$, coupled L4S marking $p_L = k \cdot p'$ (RFC 9332 §2.1 eq. (1)) |

RFC 9330 motivates the mechanism, RFC 9331 defines the ECT(1) code
point, and RFC 9332 specifies the DualPI2 coupled AQM with its P.I
controller.

### Why ECT(1) instead of a new DSCP

L4S classifies on the two-bit ECN field rather than DSCP for two
reasons. First, DSCP-based priority gives the fast-lane flow
unconditional preference, which a misbehaving flow can monopolise;
L4S requires the fast-lane flow to back off on CE marks, giving the
network a throttle that a DSCP-only scheme lacks. Second, the
two-bit ECN field is preserved end-to-end through existing NATs and
middleboxes that strip or rewrite DSCPs, reducing deployment
friction.

## Why L4S belongs in the substrate

The [Stratum architecture chapter](II-02-stratum-architecture.md) shows the substrate
as four strategy slots filled independently per node. A per-flow srTCM
edge meter, built on the `policyTableEntry` source/dest hook (see
[LLQ composition](II-05-diffserv-client.md#6-llq-composition) in the
DiffServ client chapter), keeps per-flow state at the edge.
L4S keeps no edge state: it sits at any DiffServ router and classifies
on ECN. The two are orthogonal:

| Approach | Edge state | Core state | Marking |
|---|---|---|---|
| Classic BA DiffServ (RFC 2475) | none | per-class | DSCP |
| **Per-flow srTCM edge meter (this work, 2026)** | per-flow | per-class | DSCP 3-colour |
| **L4S / DualPI2 (RFC 9331/9332, 2023)** | none | per-queue | ECT(1) + CE |

The parenthetical in the second row identifies this as the DiffServ4NS
contribution, not an IETF standard. It is RFC-compliant (RFC 2475
§2.3.4.1 permits per-flow state at edges; RFC 2697 §1 says srTCM
"meters a traffic stream" per-microflow or per-BA) but is itself not
an RFC — it is named "the per-flow srTCM edge meter" to keep it
distinct from the standards entries in this table.

RFC 9330 explicitly flags "per-flow admission + L4S queueing" as a
complementary but unexplored combination. The DiffServ4NS port
realises both sides of that combination, in separate prototypes, on
the same code base.

## Implementation overview

The L4S extension comprises five ns-3 classes (≈ 2 000 LOC total)
under `model/` and one coupled scheduler, all namespaced
`ns3::stratum`:

| Class | Role |
|---|---|
| `l4s::QueueDisc` | Descends from `QueueDisc`; composes an L4S FIFO at child idx 0 and a pluggable classic AQM at child idx 1. Overrides `DoEnqueue` to peek ECN and route ECT(1)/CE to the L4S lane, NotECT/ECT(0) to the classic inner (default `stratum::RedQueueDisc`; can be `FqCoDelQueueDisc` or any `Ptr<QueueDisc>` via `SetClassicAqmDisc`). |
| `l4s::TimestampTag` | Per-packet enqueue timestamp (dedicated tag, separate from ns-3 mainline's generic `TimestampTag` to avoid collisions) |
| `l4s::CoupledScheduler` | `Scheduler` subclass; serves L4S first with an RFC 9332 §2.5.1 bounded packet-based burst cap (default 8) |
| `PhbTable` | Helper extracted from `stratum::RedQueueDisc`; holds the DSCP → (queue, precedence) mapping. Shared by Red-backed composers. |
| *(+ test suite)* | 15 unit tests + 9 scenario fixtures in `stratum-l4s` |

### DualPI2 coupling formulas

Implemented from RFC 9332 §2.1 eq. (1) and Appendix A.1 Figure 6
(lines 4–5) inside `l4s::QueueDisc`:

```
L4S mark:              p_L = min(k * p', 1)        [stepping to 1 at the target sojourn]
Classic coupled drop:  p_C = p'^2                  [k-independent: eq. (1) is p_C = (p_L/k)^2]
```

where $p'$ is the P.I base probability driven by the controller. The
classic coupled drop runs at enqueue, before delegating to the inner
classic AQM, so inner WRED early-drops remain available as a secondary
signal under `ClassicAqm::Wred`, can be bypassed entirely under
`ClassicAqm::CoupledOnly`, or can be replaced with FqCoDel's per-flow
CoDel machinery under `ClassicAqm::FqCoDel`.

The L4S-lane CE mark is applied at dequeue, per packet, against the
dequeued packet's own enqueue timestamp: the step mark with certainty
once that packet's own sojourn reaches the L4S target, otherwise the
coupled mark $p_L = \min(k p', 1)$. The coupled mark and the classic
coupled drop are both suppressed while the total queue (both sub-queues)
sits below two MTUs — a near-empty queue takes no coupled action — while
the step mark is not floor-gated. This matches the RFC 9332 App. A.1
step AQM (mark a dequeued packet when its sojourn exceeds the target)
and the two-MTU suppression floor in the vendored GPRT DualPI2.

### RFC 9332 App. A.1 controller

A periodic (default 16 ms) self-rearming tick updates $p'$:

```
p' += alpha * (sojourn - target) + beta * (sojourn - prev_sojourn)
```

with the RFC-default coefficients $\alpha = 0.16$ Hz, $\beta = 3.2$ Hz
(App. A.1 Fig. 2 lines 13–14 at $T_{update} = 16$ ms,
$RTT_{max} = 100$ ms). Sojourn is the *classic* sub-queue's head
delay (App. A.1 Fig. 6 line 2: `curq = cq.time()`), measured via
`l4s::TimestampTag` with a bandwidth-derived proxy as defensive
fallback. The PI integrator runs only off this classic-queue sojourn;
the per-packet L4S step mark applied at dequeue (above) is a separate
mechanism and does not feed the integrator.
$p'$ is clamped to $[0, 1]$; an empty classic queue drives the error
term to $-\text{target}$ and $p'$ naturally drains to 0.

### Coupled scheduler

`l4s::CoupledScheduler` implements the RFC 9332 §2.5.1 priority bound
(starvation safeguard) as a packet-based burst cap:

```
SelectNextQueue():
  1. If L4S burst count >= cap and any classic queue has packets:
     - serve classic, reset L4S burst counter
  2. Else if L4S non-empty: serve L4S, increment burst counter
  3. Else: serve classic (RR among non-L4S queues)
```

Default cap = 8 packets (roughly 11 % classic floor under sustained
L4S backlog).

### Public attributes

All configuration is exposed via ns-3 attributes so scenarios can
tune without recompilation:

| Attribute | Default | Meaning |
|---|---|---|
| `L4sQueueIdx` | 1 | Scheduler-slot index for the L4S lane (must match the scheduler's `L4sQueueIdx`) |
| `L4sTargetSojournMs` | 1.0 | L4S step-marking threshold (Linux/GPRT default; RFC 9332 App. A.1 ramp example: 800 µs + 400 µs) |
| `CouplingFactor` ($k$) | 2.0 | $p_L = k \cdot p'$ on the L4S lane; $p_C = p'^2$ is $k$-independent (eq. (1)) |
| `ClassicAqm` | `Wred` | `Wred` keeps parent WRED pipeline; `CoupledOnly` bypasses WRED early-drops |
| `L4sBandwidthBps` | 1 Gbps | Fallback bandwidth for sojourn proxy |
| `ControllerInterval` | 16 ms | RFC 9332 $T_{update}$ (App. A.1) |

Inspection accessors `GetBaseProb()`, `GetLastClassicCoupledProb()`,
`GetLastL4sMarkProb()` expose the live controller state for
downstream analytics. Test hooks `ForceBaseProbForTest()` /
`ClearForcedBaseProbForTest()` / `AssignStreams()` let unit tests
pin $p'$ and verify coupling formulas independently of controller
dynamics.

## Composition with the substrate

The L4S queue disc descends from `QueueDisc` and composes two children:
an L4S FIFO lane and a pluggable classic AQM. The `PhbTable` helper is
a first-class class shared by Red-backed composers. The Briscoe
atomic-DualQ compliance audit
(`draft-briscoe-tsvwg-l4s-diffserv-02` §6) confirms all five
invariants pass on the landed code.

The pluggable classic slot accepts ns-3 mainline's
`FqCoDelQueueDisc`: the `CheckConfig` RED-only guard is relaxed, a
`ClassicAqm::FqCoDel` enum variant is added, and the Red-specific
forwarders are preserved as Red-only (foreign inners must configure
through their own API).

The same composition pattern extends to the DiffServ edge and core;
see the [Edge/core composition and meter injection](#edgecore-composition-and-meter-injection) section for the current architecture.

## Known limitations and future work

1. **Scalable-CC TCP — closed.** `TcpDctcp` with `UseEct0=false`
   is available in the pinned ns-3 snapshot and emits ECT(1) on
   outgoing segments, satisfying the Scalable-congestion-control
   definition of RFC 9331 §4. The responsive-flow coexistence binary
   (`diffserv-l4s-s2-coexistence`, see [Responsive-flow coexistence](III-03-l4s.md#responsive-flow-coexistence)) uses this transport.
   A dedicated `TcpPrague` model with RTT-independence is under
   development upstream and would allow a stricter equivalence
   demonstration; that is future work once the model lands in
   mainline.

2. **UDP CBR is the only traffic model exercised in this evaluation.**
   Bulk-TCP, responsive VoIP, and PagePool/WebTraf mixes are
   untested under L4S.

3. **GPRT cross-validation complete; Linux kernel head-to-head remains future work.**
   The DualPI2 inner has been cross-validated against the GPRT reference
   implementation (Veras et al. 2026): per-cell mean JFI difference is
   within 0.01, with JFI 0.998 ± 0.003 for both Stratum and the GPRT
   replication at 100 Mbit/s × 5 ms. A direct head-to-head against
   Linux kernel `sch_dualpi2` (measuring byte-identical coupling
   probabilities at matched operating points) is the natural next
   validation step and is catalogued under P1 (validation replications)
   in the [Forward research program](#forward-research-program--scenario-inventory) section.

4. **Overload handling is mark-saturation only.** When load is so
   heavy that the coupled probability saturates (`k·p' ≥ 1`), this
   implementation keeps CE-marking the L4S lane at 100 % and keeps
   applying the classic coupled drop `p_C = p'²` (which may rise to
   1.0), but it does not switch the L4S queue to dropping and does
   not cap `p_C` at the `p_Cmax = 1/k²` overload threshold of
   RFC 9332 App. A.2 — so the §2.5.1.1 requirement to introduce drop
   for both ECN-capable traffic types under persistent overload is
   not implemented (the GPRT reference drops L4S packets with
   probability `p_C` in that regime). Protection of the L4S lane
   against unresponsive overload traffic (RFC 9332 §4.2) is out of
   scope; in the validated scenarios that reach this regime the
   excess L4S load is bounded by the L4S FIFO's queue limit instead.

### Inner classic AQM is pluggable

`l4s::QueueDisc` descends from `QueueDisc` and composes two children:
a FIFO for the L4S lane and a `Ptr<QueueDisc>` for the classic AQM.
`stratum::RedQueueDisc` is the default, but any `QueueDisc` is accepted via
`SetClassicAqmDisc`. The coupled-drop pipeline
(`p_C = p'^2`) fires inner-agnostically in `MaybeCoupledDrop`
before delegating to whichever inner is installed.

Callers pick the inner via the `ClassicAqm` enum — `Wred`,
`CoupledOnly`, or `FqCoDel` — or pre-build any `Ptr<QueueDisc>` and
inject it via `SetClassicAqmDisc`. Red-specific forwarders
(`AddPhbEntry`, `ConfigQueue`, `SetMredMode`, and friends) still
assert on non-Red inners — foreign-inner callers must pre-configure
their chosen disc through its own API before injecting it. This is a
deliberate interface choice: the Red-specific concepts (PHB tables,
WRED thresholds, precedence-per-virtual-queue) have no analogue in
FqCoDel, and a translation layer would be lossy.

The comparison example at
`examples/diffserv-l4s-fqcodel-comparison.cc` has five
selectable modes:

| `--mode=` | Inner classic AQM |
|---|---|
| `l4s-wred`           | `stratum::RedQueueDisc` (WRED + coupled drop) |
| `l4s-coupled-only`   | `stratum::RedQueueDisc` reduced to pass-through (coupled drop is sole AQM) |
| `l4s-fqcodel-classic` | `FqCoDelQueueDisc` (per-flow fair queueing + CoDel target) |
| `fqcodel`            | pure mainline FqCoDel as the bottleneck disc (no L4S lane) |
| `classic-only`       | plain `FifoQueueDisc` drop-tail baseline (no L4S lane) |

Future inner-AQM strategies (PIE, CoDel-without-FQ, research discs)
require only a test case and a comparison-example mode; no core
change to `l4s::QueueDisc`.

### Edge/core composition and meter injection

Both `EdgeQueueDisc` and
`CoreQueueDisc` descend directly from `QueueDisc`, the
same parent as `l4s::QueueDisc`.
The existing PHB / mark-rule / policy-classifier APIs are preserved as
forwarders on the edge disc. See the
[Stratum architecture chapter](II-02-stratum-architecture.md) for the four-slot
composition that all three clients share.

Meters follow the same pattern. The meter pool on
`EdgeQueueDisc` is a `Ptr<Meter>` slot array; the classifier
consults the edge disc via an `EdgeMeterProvider` lookup hook rather
than hard-coding a specific meter type. All seven shipped meter
classes (`SrTcmMeter`, `TrTcmMeter`, `Tsw2cmMeter`, `Tsw3cmMeter`,
`TokenBucketMeter`, `FWMeter`, `DumbMeter`) extend the `Meter` base
class and are injectable at configuration time. This closes the
symmetric strategy-extraction design described in the 2001 thesis
§3.3.2: schedulers were extracted in thesis Figure 3.11 (2001),
droppers in 2026, and meters complete the trio.

The meter-as-strategy design enables the highest-value open research
gap from Briscoe's L4S–DiffServ analysis — gap 2, "DualQ +
guaranteed-latency policer" — which requires a latency-target meter
injected alongside the DualPI2 inner.

### Forward research program — scenario inventory

The substrate currently supports a catalogued scenario backlog
organised by research priority:

| Priority | Purpose | Examples |
|---|---|---|
| **P1 — Validation replications** | Replicate published DualPI2 results | De Schepper et al. 2022 (DualPI2 paper Figs. 5–7), Linux `sch_dualpi2` head-to-head, Veras et al. 2026 numerical comparison. (The RFC 9332 Appendix A.1 golden controller vector landed in-tree together with ECN codepoint vectors; see `specs/02-structural.md`.) |
| **P2 — Open research gaps** | Briscoe's five open L4S–DiffServ gaps | DualQ + AF, **DualQ + guaranteed-latency policer** (highest value; requires a latency-target meter injected via the meter-as-strategy slot), DualQ + LE scavenger, weighted L-lane, DSCP↔ECT(1) classifier-order empirics |
| **P3 — Coexistence studies** | L4S–DiffServ coexistence | Baseline coexistence, ECN bleaching (~12% of paths, per Catchpoint 2024), local-DSCP-for-ECT(1) remap, DiffServ coexistence under load |
| **P4 — Handbook demonstrations** | Illustrative multi-class scenarios | Heterogeneous-meter side-by-side, FqCoDel-vs-RED at scale, DOCSIS-flavour topology, three-class hierarchy |

Limitation 3 above (Linux kernel head-to-head) falls under the P1
validation-replication category in that inventory.
