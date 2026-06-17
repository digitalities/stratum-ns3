---
title: CAKE validation
origin: 2026-written
status: filled
last-updated: 2026-06-10
---

# CAKE validation

This chapter presents the validation evidence for the CAKE client:
spec-tier coverage, dispatcher instrumentation, the host-fairness empirical
anchor, and the Linux-netns cross-validation backend. The companion
[figure pack](III-04A-cake-flent-figure-pack.md) collects the Flent runs.
For the client's architecture — tins, shaping modes, the ACK filter, and
composition with the substrate — see [The CAKE client](II-07-cake-client.md).

## Validation

The quality-tier suite pins the validation targets, mirroring the
CAKE paper's experiments E1–E5:

| # | Source | Reference metric |
|---|---|---|
| E1 | CAKE RRUL latency under load (induced-latency budget; no paper figure pins an absolute value) | 4 TCP up + 3 EF latency probes, ACK-only reverse path; the queue disc owns the bottleneck (one-packet device TX queue); induced probe p99 (p99 − min OWD) < 15 ms one-way, with a < 25 ms floor-sanity bound |
| E2 | CAKE paper Fig 4 | 4 greedy TCP one per tin; tin rates within 3% of share weights |
| E3 | CAKE §III-B per-flow FQ | 32 TCP flows in one tin; Jain fairness > 0.95 after 10 s |
| E4 | CAKE §III-B + Fig. 1 | 128 5-tuples collide into 16 baseline buckets; set-associative hashing expands to ≥4× more active flow-queues than plain hashing (unit-style test, no simulation) |
| E5 | CAKE paper Fig. 6 | 30/1 Mbps DSL profile, 4 TCP up + 4 TCP down, ACK filter (Conservative); ~15% downstream gain on Linux, surfaced in-simulator at the 100:1 regime (see [ACK filter](II-07-cake-client.md#ack-filter)) |

A three-way calibration compares Linux `tc-cake` Flent output against
the substrate's CAKE client against an ns-3 FqCoDel baseline, with a
±15% envelope matching the established ns-2/ns-3 calibration
tolerance.

## Dispatcher instrumentation

The patched-mainline `FqCobaltQueueDisc` exposes per-host-bucket
statistics when `EnableHostIsolation=true`. The host-isolation path is
exercised by the `cake-flent-host-attribution-smoke.cc` coverage
fixture, which confirms per-host-bucket dispatch across the canonical
asymmetric workload — four flows from one host against a single flow
from another, sharing a triple-isolate CAKE `besteffort` tin —
and by the EXTENSIVE test suite host-fairness cases (see the
[Host-fairness empirical anchor](#host-fairness-empirical-anchor) section).

The host-isolation algorithm follows Linux `sch_cake.c @ 67dc6c56b871`
(`cake_get_flow_quantum` @ line 688): per-flow DRR with per-flow quantum
scaling by host-load count via `max(srcCount, dstCount)`. Per-host
fairness emerges directly from the quantum divisor. This is the
Linux-faithful path; the [host-fairness empirical anchor](#host-fairness-empirical-anchor) below gives the direct measurement.

## Host-fairness empirical anchor

**In brief.** The substrate's host-isolation quantum arithmetic is bit-equivalent
to Linux `sch_cake`, and in the per-flow regime its host share matches Linux
within ≤4.3 pp. Where host isolation genuinely *discriminates* — contending hosts
sending to **different** destinations — the substrate stays mechanism-faithful,
but the deterministic simulator under-shoots the *magnitude* of the fairness
effect. A matched-configuration measurement against a real Linux kernel pins that
gap to the one ingredient the simulator lacks, hardware timing jitter, and finds a
small (~2–3 pp) residual that real Linux shares. The evidence runs from the anchor
outward to that cross-stack decomposition; the forensic detail sits in a deep-dive
a reader after the verdict can skip.

Host isolation (`triple-isolate`) is the fourth canonical CAKE
component and the one most sensitive to implementation detail. With
the patched-mainline `FqCobaltQueueDisc`, the pure-ns-3 host share at
the canonical 16-flow-versus-1-flow anchor matches pure Linux
`tc-cake` within ≤4.3 percentage points across CUBIC, NewReno, and
BBR, on a 100 Mbit/s shared bottleneck with 20 ms one-way delay. The
per-side-max keying (`max(srcCount, dstCount)`) is the mechanism: each
flow's DRR quantum is divided by its host's per-side flow count on flat
per-flow DRR (there is no outer per-host queue), so a host running many
flows does not crowd out a host running few. At this shared-sink anchor
the destination-host count saturates uniformly across all flows, so the
divisor cancels and the outcome is per-flow fairness, which coincides
with the host-fair split here only because the two sides carry equal flow
counts (16 versus 16).

A complementary 8-host-versus-1-host test (8 hosts × 8 flows against
1 host × 64 flows) is a second shared-sink control. With one sink the
destination-host count again saturates uniformly, so the shipped
`max(srcCount, dstCount)` divisor cancels and the outcome is per-flow
fairness: a Group-A / Group-B byte ratio of ≈ 0.99× across the
64-versus-64 flows, with neither host starved. Both anchors therefore
bound *per-flow DRR*, which is dispatch-timing-robust; neither exercises
host isolation, which discriminates only in the split-destination regime
below.

A four-protocol cross-check confirms the result is not transport- or
topology-specific. CUBIC, NewReno, and BBR cluster within 0.046 of one
another at the (16, 1) anchor; at a (16, 16) symmetric control all
three TCP variants and a UDP cell sit within ±0.01 of the host-fair
midpoint on both stacks, ruling out topology- or qdisc-side structural
confounders. At the (16, 1) UDP cell the two stacks agree to within
0.001, so the wire-level scheduler arithmetic is at parity and the
residual TCP-side spread is reactive-transport-specific.

### Where isolation discriminates — the split-destination regime

The anchors above share a single bottleneck sink, so the
destination-host count saturates uniformly and triple-isolate reduces to
per-flow fairness; the ≤4.3 pp Linux agreement there is the agreement of
per-flow DRR, which is dispatch-timing-robust. Host isolation only
*discriminates* when the contending hosts use **different** destinations
— then the source-host count drives the divisor. In that regime the
substrate stays mechanism-faithful to Linux (the host-isolation-off
control recovers per-flow fairness, and the quantum arithmetic is
bit-equivalent to `sch_cake`), but the pure-ns-3 *outcome* sits a little
above native Linux's: at a 4-flows-versus-1-flow split-destination probe with
socket buffers sized to the path bandwidth-delay product, ns-3 settles
near a 0.65 host-A share, while native Linux measured at the *same* fixed
buffers settles at 0.53–0.59 (0.53 with segmentation offload disabled, so it
sends single segments as ns-3 does; 0.59 with the offload on) — and at its
*default autotuned* buffers Linux sits near 0.51. At the ns-3 default
128 KB socket buffers the lone host's flow is receive-window-limited
(its window pins ~4× below the ~550 KB BDP) and the share is inflated
to ≈0.76.

This is a fidelity boundary in the discriminating regime, not a scheduler defect.
The substrate stays mechanism-faithful: the quantum arithmetic is bit-equivalent
to `sch_cake`, the host-isolation-off control recovers per-flow fairness, and an
open-loop UDP control in the simulator reaches per-host fairness (≈0.50). The
residual appears only under closed-loop TCP, and a matched-configuration
cross-stack measurement resolves it. Sweeping injected dispatch jitter — a
stand-in for the interrupt and softirq timing real hardware produces — drives the
ns-3 share down from 0.65 into a host-fair floor: a seed ensemble around 0.55,
reaching 0.522 at a single replica, bracketing where native Linux `sch_cake` sits
at the *same* fixed 4 MB buffers (0.53, with segmentation offload off so it sends
single segments as ns-3 does). This is reproducible from the
`cake-host-iso-jitter-floor` example and its gated test (S-17.68 / S-17.69 in
`specs/02-structural.md`). The simulator's deterministic event schedule is the only
thing between the two: real Linux runs with finite hardware jitter and already sits
at the floor the simulator reaches only when jitter is injected.

![CAKE host-isolation — the discriminating-regime gap is absent hardware timing jitter. Host-A share against aggregate throughput in the (4,1) split-destination regime as injected dispatch jitter is swept in deterministic ns-3 (colour = jitter amplitude, marker = redraw period). The deterministic baseline sits at 0.65 and full throughput; injecting jitter walks the share down to a floor near 0.52 along the full-throughput edge, bracketing native Linux `sch_cake` at the same fixed 4 MB buffers (0.53, segmentation offload off, dashed). Cells that push the jitter harder lose throughput — they fall to the left — without lowering the share, so ≈0.52 is a genuine floor rather than an arbitrary stopping point. The dotted line marks ideal host-fairness (0.50).](figures/cake-jitter-floor/cake-jitter-floor.svg)

The gap decomposes into two parts of different kind — an isolable socket-buffer
artefact and the coupled timing effect just shown — plus a small reactive residual
real Linux shares. The deep dive below measures each; the technical report's
Stratum-bridge chapter carries the full platform sweep and the host-isolation-off
control.

#### Deep dive: how the gap decomposes

Two components, different in kind. The first — the receiver's socket buffer — is a
cleanly isolated effect with its own control knob, positively attributed to a
measured quantity (the lone host's backlog occupancy). The second is a single
coupled timing-and-reactive-transport effect; an earlier account split it into a
"cadence" figure and a "transport residual", but a direct per-flow timing analysis
shows those are two readings of one effect, and the matched cross-stack reference
separates the deviation by *kind* rather than by subtraction.

**Socket-buffer window-limit (~11 pp) — directly measured.** Sweeping only the
receiver's socket buffer, everything else fixed, moves the host-A share from 0.76
at the ns-3 default 128 KB to 0.65 at 4 MB (well above the path's ~550 KB
bandwidth-delay product). The mechanism is confirmed independently of the share:
below the BDP the lone host's in-flight bytes pin at the buffer — provably
receive-window-limited — so its flow cannot keep its CAKE bucket filled to use its
full quantum. A per-flow timing analysis ties the shift to occupancy directly: the
lone flow is backlogged only ~57 % of the time at 128 KB, rising to ~86 % once the
window covers the BDP, and the share tracks that occupancy. The same sweep at a
1448-byte MSS gives 0.80 → 0.70, and an open-loop UDP flow is unaffected at every
buffer size, so the sensitivity is the window limit, not a segment-size or queue
artefact. The small non-monotonic dip near 256 KB is a per-socket-buffer ×
flow-count interaction: there the lone flow's window cap happens to rate-limit it
to near its fair share, smoothing its delivery so it stays backlogged, while the
four-flow host over-subscribes and idles.

![CAKE host-isolation — socket-buffer sweep. Host-A share against receiver socket-buffer size: below the ~550 KB bandwidth-delay product the lone host's flow is receive-window-limited and the share is inflated to ≈0.76; above it the share settles near 0.65. The non-monotonic dip near 256 KB is a per-socket-buffer × flow-count smoothing effect, and an open-loop UDP flow (never window-limited) is flat across the sweep. The same shape at MSS 1448 rules out a segment-size artefact.](figures/cake-buffer-sweep/cake-buffer-sweep.svg)

**Coupled reactive-transport × dispatch-cadence remainder — measured, not
subtracted.** Once the buffer is sized above the BDP, the rest of the gap is one
effect of reactive transport meeting deterministic event scheduling. At
buffer-adequate settings both hosts are backlogged for near-equal fractions of
time on both stacks (native Linux ≈97 %/97 %; ns-3 ≈87 %/86 %), a per-host ratio
near 1.0 (far below the ≈1.8 byte-share ratio), so the deviation is a
within-contention dispatch bias, not an occupancy asymmetry. The conclusion is
robust to how occupancy is defined: the model-free per-host measure in the
`cake-host-iso-jitter-floor` example reads mildly higher (ratio ≈1.2) but still
well below the share ratio. Two manipulations show it is one coupled effect:

- It is **reactive-gated**: an open-loop UDP control collapses the
  within-contention bias on both stacks, so the bias is specific to reactive,
  closed-loop TCP. Replaying ns-3's recorded arrivals through the real Linux
  `sch_cake` likewise reproduces the ns-3 outcome, not Linux's, placing the cause
  in the TCP-shaped arrival pattern rather than the queue.
- It is **timing-driven**: sweeping bottleneck rate-jitter drives the share from
  0.65 to the ≈0.52 floor and no lower without losing throughput — the
  share-versus-throughput sweep above makes that trade-off explicit — reaching the
  matched-buffer native-Linux value; physically-grounded alternatives
  (segmentation offload, transmit pacing) did *not* move it, so the effect lives
  in the deterministic event ordering. The live-kernel capture below quantifies
  the real dispatch cadence the injected jitter stands in for.

![CAKE host-isolation — live-kernel dispatch timing. Left: the time between successive packet departures at the real Linux `sch_cake` bottleneck — a tight mode at 32–64 µs (the ≈47 µs shaped spacing) with an idle tail at 8–64 ms from TCP backoff. Right: NET_RX softirq service duration (mode 8–16 µs). This hardware-driven jitter is what the simulator's injected rate-jitter proxy emulates; deterministic ns-3 has none of it.](figures/cake-dispatch-timing/cake-dispatch-timing.svg)

Removing *either* the reactivity or the determinism collapses the bulk of the
excess, so the two are tightly coupled inside the simulator. The matched
cross-stack reference then separates the deviation by kind: a larger
deterministic-dispatch component — the absent-hardware-jitter gap, closed by
realistic jitter and not present on hardware — and a small reactive-transport
residual of ~2–3 pp that *survives* realistic dispatch jitter. Native Linux itself
settles at ≈0.53, about 3 pp above ideal host-fairness, with symmetric occupancy,
so that residual is physical and shared by both stacks, not an ns-3 artefact. One
small open item remains: under always-backlogged UDP, native Linux's per-host
quantum slightly favours the single-flow host (≈0.44 versus the simulator's exact
0.50), a minor deficit-round-robin granularity difference that does not affect the
finding.

The canonical host-isolation reference is the CAKE paper's Figure 3 — two
source hosts to four destination hosts, split destinations, the discriminating
regime. A four-mode replication of it (no-isolation, source, destination,
triple) confirms each isolation mode moves the per-flow shares toward its
mode-specific fair target relative to the no-isolation baseline, verifying the
source / destination / triple modes functional on the patched-mainline path.
The replication also exposes the phase-effects floor directly: in deterministic
ns-3 the no-isolation baseline is itself ≈ 2:1 by source node (same-node flows
synchronise), so the paper's absolute fractions are unreachable in simulation
and the conformance claim is necessarily relative.

Running the *same* Figure-3 topology on a real Linux kernel `sch_cake` closes
the argument. The figure below sets three series side by side for every flow
and mode — the paper's analytic ideal, pure ns-3 (three RNG seeds, byte-identical
results), and Linux `sch_cake` (three replicas). Linux reproduces the ideal to
within a few thousandths in every cell, including triple-isolate's tall
B→destD share, while pure ns-3 stays compressed toward the phase-effects floor.
Because Linux runs the very algorithm the substrate ports, the magnitude gap is
the deterministic simulator's dispatch cadence, not the host-isolation
mechanism: the ideal and Linux bars overlap at full magnitude, and ns-3 is the
single attenuated series.

![CAKE Figure-3 host isolation — paper-ideal vs pure ns-3 vs Linux `sch_cake`, across the four flow-isolation modes (no-isolation, source, destination, triple). Six flows run from two source hosts to four destination hosts; the ideal and Linux series overlap at full magnitude while deterministic ns-3 is attenuated toward the phase-effects floor.](figures/cake-fig3/cake-fig3-three-way.svg)

## Prioritising a real-time flow

Fair queueing protects flows from one another, but it cannot help a flow that
asks for more than its share and refuses to slow down. A fixed-rate voice or
video stream is exactly that case. The figure below runs a 2 Mbit/s
constant-rate flow, marked latency-sensitive, alongside thirty-two bulk
downloads on a 10 Mbit/s link, with the bulk traffic starting at five seconds.
The stream's fair share is barely 0.3 Mbit/s, so under plain per-flow fair
queueing it overruns its own queue, and because it ignores the drops that
queueing uses to signal congestion, it never backs off.

The two panels tell the whole story. The left panel is the stream's added
latency over time; the right panel is how much of its 2 Mbit/s actually
arrives. FQ-CoDel lets the stream's own backlog swell: its latency spikes past
900 ms when the bulk traffic arrives and then oscillates for the rest of the
run — the behaviour the CAKE paper reports. CAKE in best-effort mode looks
better on the latency panel, because its drop logic (Cobalt, with its BLUE
fallback for persistent overload) trims the backlog within a couple of seconds
— but the right panel shows the catch: it does so by discarding roughly seven
packets in ten, so the low latency belongs to a stream that has been gutted.
Only when CAKE reads the stream's marking and assigns it the latency-sensitive
tin does the stream get what it needs: its full 2 Mbit/s, no loss, and no
added delay. For an unresponsive real-time flow, priority marking, not fair
queueing, is what isolates.

![CAKE Figure-5 latency isolation — a 2 Mbit/s latency-sensitive fixed-rate flow against thirty-two bulk flows on 10 Mbit/s. Left: induced one-way delay over time (FQ-CoDel oscillates in the hundreds of milliseconds; CAKE best-effort and CAKE DiffServ stay low). Right: the flow's delivered goodput against the 2 Mbit/s offered, ns-3 next to real Linux sch_cake, with packet loss annotated — only CAKE DiffServ delivers the full rate at zero loss on both, while best-effort and FQ-CoDel are starved at seventy to eighty-five per cent loss.](figures/cake-fig5/cake-fig5.svg)

It is worth setting this beside the host-isolation result above. There, the
*magnitude* of the fairness effect was muted: the simulator's deterministic
event schedule damps the cross-flow timing that real hardware relies on, and
the simulated shares fell short of the published spread. Here the result lands
cleanly. The difference is the mechanism. Tin priority is a scheduling
decision: the latency-sensitive tin is served first whenever it has traffic,
irrespective of how events happen to interleave, so it is indifferent to the
timing the simulator cannot reproduce. Scheduling priority transfers at full
strength; aggregate fairness magnitude is bounded by the same timing fidelity
that limited the host-isolation picture. One honest wrinkle: the substrate's
CAKE turns the over-sending flow away by *dropping* it, whereas the original
2018 measurement showed plain CAKE letting the backlog stand at a couple of
hundred milliseconds. Running the very same scenario on a real Linux kernel
settles which is right today — the right-hand panel sets the two side by side.
Linux `sch_cake` in DiffServ mode serves the flow at its full 2 Mbit/s with no
loss, exactly as the substrate does; under plain per-flow fair queueing, real
`sch_cake` and `fq_codel` drop seventy-nine to eighty-five per cent of it, the
same way the substrate does. The drop-not-queue outcome is the live kernel's,
not a simulator artefact: current CAKE caps an unresponsive flow's queue with
its Cobalt drop logic rather than letting it stand at the older trace's couple
of hundred milliseconds.

## Linux-netns cross-validation backend

The substrate also carries a backend-pluggable cross-validation
direction: each strategy can in principle run against a *Linux-netns
backend* alongside the deterministic ns-3 backend. When the Linux
backend is selected, every component of the scenario — TCP stacks,
routing, and the cake qdisc — runs in Linux netns rather than in the
ns-3 simulator. This is full-scenario delegation, not a hybrid proxy
where ns-3 senders feed a Linux qdisc.

The scenario-emission half of the architecture is prototyped at
`scripts/stratum-bridge/`. A scenario IR (YAML, schema
`stratum-bridge/scenario/v1`) is mechanically translated by
`emit-netns.py` into a self-contained bash script that builds the
equivalent Linux netns testbed via a Lima VM. Eight bundled
scenarios cover a four-protocol × two-cell surface; the emitted
testbed reproduces the Linux reference host-share band on all eight
cells within ±0.01 tolerance (max Δ ≈ 0.008), giving an independent
cross-check of the host-isolation anchor of the [Host-fairness empirical anchor](#host-fairness-empirical-anchor) section.

The prototype validates the configuration-translation primitive
only — the packet-level proxying half (a real-time bridge with a
C++ `SetBackend` API) is deferred to a follow-on effort.  See
`scripts/stratum-bridge/README.md` for the scenario IR schema and
emitter design, and `docs/REPRODUCIBILITY.md` §7 for the
reproduction recipe.

## Cross-references

- CAKE paper: Hoeiland-Joergensen, Taeht, Morton, Chromy.
  *"Piece of CAKE: A Comprehensive Queue Management Solution for
  Home Gateways"*, arXiv:1804.07617, 2018.
- Linux reference: `tc-cake(8)` manual; `iproute2` `tc/q_cake.c`.
- Data deposit: the matched-buffer cross-stack measurements behind the
  host-fairness analysis (methods note, figures, and raw per-replicate records)
  are archived at DOI
  [10.5281/zenodo.20724064](https://doi.org/10.5281/zenodo.20724064).
- Client chapter: [The CAKE client](II-07-cake-client.md) (tins, shaping modes,
  ACK filter, feature scope, composition with the substrate).
- Companion chapter: [The L4S client](II-06-l4s-client.md) (the substrate's other
  modern client); [L4S validation](III-03-l4s.md) for the evidence.
- Figure pack: [CAKE Flent figure pack](III-04A-cake-flent-figure-pack.md).
- Linux-faithfulness verdict: patched-mainline `FqCobaltQueueDisc`
  matches Linux `tc-cake` within ≤4.3 percentage points at the
  16-flow-versus-1-flow shared-sink anchor (the per-flow regime) across
  CUBIC, NewReno, and BBR; the split-destination regime where isolation
  discriminates carries a fidelity boundary that is substantially a
  measurement-configuration effect (an isolable socket-buffer window-limit
  artefact ~11 pp + a single coupled reactive-transport × dispatch-cadence
  remainder), confirmed against a matched-buffer native-Linux reference: at the
  same fixed buffers Linux settles at 0.53–0.59 and the de-phased simulator lands
  at that value, attributing the deterministic excess to absent hardware timing
  jitter; mechanism byte-faithful, not a scheduler divergence.
