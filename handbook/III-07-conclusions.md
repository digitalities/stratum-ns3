---
title: Conclusions
origin: 2026-written
status: filled
last-updated: 2026-06-06
---

# Conclusions

What Stratum is, what the evidence in this book established, where the
honest fidelity boundaries lie, and what the substrate enables next.

## What was built

Stratum is a composable QoS substrate for ns-3 hosting three
first-class clients — DiffServ, L4S, and CAKE — through a
shared set of four pluggable strategy slots:

| Slot | Role |
|---|---|
| **Classify-and-Meter** | Ingress policing: sr-TCM, tr-TCM, TSW2CM/3CM, byte-accounting |
| **Mark-and-Route** | DSCP tag plus PHB table lookup |
| **Per-class Slot array** | Up to eight independent inner queue discs |
| **Across-slot Service Policy** | Scheduler or dispatcher: PQ, WFQ, WF2Q+, LLQ, WRR, SCFQ, SFQ, RR, WIRR |

Each client composes a queueing discipline by picking one strategy per
slot; the substrate does not change. The three clients share the same
edge classifier, the same DSCP-to-slot table, and the same helper
infrastructure. Only the dispatcher and the per-slot inner disc change
between them.

The architecture is registry-based: downstream consumers — CLI
catalogues, plot palettes, and smoke-registry tests — auto-derive
coverage from the in-tree set of registered cells. The substrate ships
13 AQM cells and 9 scheduler cells, so the substrate-claim is
machine-checkable rather than document-only.

## What the evidence established

### Reconstruction as verification

The DiffServ client is a 2026 reconstruction of the author's
2001 DiffServ4NS ns-2 module. The two simulator generations share no
code; a 2026 ns-2.35 port layer is an independent bridge. Running
all three on the same scenarios turns agreement across them into a
verification instrument: when three independent simulators converge on
the same per-class outcome, that is evidence the reconstruction
reproduces the original behaviour.

The three-simulator campaign is detailed in
[Three-way comparative results](III-02-three-way-validation.md).
The headline figures: aggregate throughput in Scenario 3 matches within
0.8 % across ns-2.35 and ns-3; Premium (CBR/EF) agrees within 0.1 %;
Silver, Bronze, and BE slip −2.5 to −2.9 %, inside their ±3 %
tolerance. One-way delay means agree within 1 % across all three
simulators. 26 ns-2.29-vs-ns-2.35 pairs and 21 ns-2.35-vs-ns-3 pairs
record zero FAIL at the calibrated tolerances.

Reconstruction at sufficient resolution verifies the surrounding context
too. Two latent defects emerged that originated outside the port: a
finite-FTP classification slip dormant in the 2001 source since its
original release, and a null-pointer dereference in modern ns-3
mainline's TCP persist timer, fixed by an eight-line null-guard and
submitted upstream. Two abstraction-layer asymmetries visible at scenario
scale — the 2001 UDP agent's missing 28-byte IP/UDP header and ns-3's
explicit NetDevice wire-byte accounting — account for the systematic
offset between ns-2 and ns-3 one-way delay measurements.

#### Gold residual — a generator approximation

The one per-class result outside the ±3 % band is Gold (ns-3 +9.9 %
versus ns-2.35), which carries a RealAudio-like on/off workload that
ns-3 approximates with an exponential off-time distribution. A
byte-identical trace-replay confirms this is a generator approximation,
not a policer divergence: feeding the ns-2.35 Gold ingress
packet-for-packet through the ns-3 TSW2CM/RIO-C policer reproduces the
out-of-profile fraction within sampling noise (0.1731 vs 0.1736 across
three seeds, ≈ 1.3× the binomial σ). Given identical input, the two
policers mark identically. The full analysis is in
[Three-way comparative results](III-02-three-way-validation.md).

### L4S conformance

The L4S client implements a native RFC 9332 DualPI2 coupled AQM with
DCTCP as a stand-in for TCP Prague. Validation covers RFC 9331/9332
structural conformance vectors, coupling-formula accuracy, starvation
safety, and substrate-side throughput equivalence against the GPRT
reference implementation. At the headline 100 Mbit/s × 5 ms cell the
per-cell mean JFI over 30 replicates is 0.998 ± 0.003, indistinguishable
from the GPRT reference at the same operating point. Full evidence is in
[L4S validation](III-03-l4s.md).

End-host TCP Prague integration is deferred pending the upstream ns-3
merge; DCTCP is the stand-in throughout this work.

### CAKE Linux-faithfulness

The CAKE client composes the mainline `FqCobaltQueueDisc` under a new
deficit-round-robin dispatcher. CAKE's four canonical components —
bandwidth shaping, per-flow fair queueing within its set-associative
hash, DiffServ handling, and ACK filtering — are each integrated and
verified individually.

Host isolation is the component most sensitive to implementation detail.
With the patched-mainline `FqCobaltQueueDisc`, the pure-ns-3 host share
at the canonical 16-flow-versus-1-flow anchor matches pure Linux
`tc-cake` within ≤4.3 percentage points across CUBIC, NewReno, and BBR
on a 100 Mbit/s shared bottleneck at 20 ms one-way delay. The
per-side-max keying (`max(srcCount, dstCount)`) is the mechanism that
produces agreement.

The ACK-filter port is confirmed Linux-faithful by a direct audit against
`sch_cake.c` (see [CAKE validation](III-04-cake.md)). The CAKE paper's downstream goodput gain (around 15 % at
30/1 Mbit/s) is confirmed on Linux at the paper-strict operating point
(1.09×–1.14×); the ns-3 substrate surfaces the same gain at higher
link asymmetry (100:1, 1.105×–1.171×). Reproducing the gain at the
paper-strict 30/1 Mbit/s operating point *inside* deterministic ns-3 is
a fidelity boundary addressed under future work below. Full evidence is
in [The CAKE implementation](III-04-cake.md).

### CAKE + L4S composition

A composition neither mainline Linux nor BSD expresses: CAKE's
DSCP-to-tin classifier and across-tin DRR provide diffserv-aware
host-fairness shaping; each tin's inner queue is a `l4s::QueueDisc`
providing per-queue marking for scalable congestion controls. Measured
at 40 Mbit/s, 50 ms RTT with one scalable (DCTCP) and one classic
(CUBIC) flow sharing a tin, the composition is indistinguishable from a
bare DualPI2 inner and the GPRT reference on goodput and fairness.
`cake::Helper::SetAsCakeDiffserv4` exposes this composition through a
`useDualPi2Inner` parameter.

## Fidelity boundaries

ns-3 is the appropriate evaluation environment for RFC-conformance work,
cross-simulator equivalence studies, AQM-envelope characterisation
(the substrate ships a 13-AQM × 9-scenario sweep), independent
reproductions of published results, and exploration of compositions the
deployed Linux kernel does not currently express. Window-flow-controlled
CAKE host-fairness and the DiffServ-classified L4S composition are both
within the substrate's validated scope.

One boundary is worth stating explicitly: the
paper-strict 30/1 Mbit/s reproduction of the CAKE ACK-filter gain is
confirmed on Linux but cannot yet be surfaced inside deterministic ns-3.
The gap is not an algorithmic defect — the mechanism is Linux-faithful (per the `sch_cake.c` audit in [CAKE validation](III-04-cake.md)) —
but a timing phenomenon: the ACK-filter gain at that particular operating
point is mediated by NAPI/softirq jitter that the simulator does not
model. The sub-millisecond cross-flow jitter phenomena that distinguish
Linux kernel scheduling from ns-3's discrete-event model are the narrow
carve-out where the simulator is not the right tool without a
jitter-capable execution backend.

## Future directions

Three extensions are identified in the paper for the next release:

- **TCP Prague at the L4S end-host.** DCTCP is the stand-in throughout
  this work. TCP Prague, once upstreamed into ns-3 mainline, would
  close the end-to-end TCP Prague / RFC 9332 validation chain and
  enable starvation-recovery and RTT-stability studies under mixed
  Prague/Classic traffic.

- **Four kernel-coupled `tc-cake(8)` flags.** The overhead-accounting
  flag, the split-GSO flag, and two further `tc-cake(8)` mode flags are
  identified for a follow-on implementation pass.

- **A jitter-capable execution backend.** Reproducing the CAKE
  ACK-filter gain at the paper-strict 30/1 Mbit/s operating point in
  simulation is contingent on a Stratum-bridge backend that surfaces
  Linux NAPI/softirq jitter. The Stratum-bridge prototype (prototyped
  at `scripts/stratum-bridge/`) demonstrates the configuration-translation
  direction; the real-time packet-level proxying half is the deferred
  follow-on.

## Closing

Stratum is usable today in standard ns-3 mainline workflows: the module
lives in `contrib/stratum`, builds against the pinned ns-3 release (recorded once in the fetch script)
via `scripts/fetch-ns3.sh`, and ships open-source under GPLv2. The
validated scope covers the three clients described in this book: the
faithful DiffServ reconstruction, the L4S DualPI2 prototype,
and the Linux-calibrated CAKE client. Sharing four primitives across all
three lets one substrate realise compositions (such as the per-tin
DualPI2 inside CAKE's diffserv shaping) that neither mainline Linux nor
BSD currently expresses. Further clients reach the same surface as new
dispatcher and inner-disc subclasses, without touching the edge
classifier, the PHB table, or the meter layer.

The 25-year heritage — from the author's original 2001 module, through
the 2026 ns-2.35 port, to this ns-3 substrate — is the subject of the
preface and the heritage repository; what this book records is the
evidence.
