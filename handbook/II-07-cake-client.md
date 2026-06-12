---
title: The CAKE client
origin: 2026-written
status: filled
last-updated: 2026-06-07
---

# The CAKE client

> **Hands-on**: see [CAKE recipes](I-05-cake.md) for runnable recipes; see
> [CAKE validation](III-04-cake.md) and the
> [Flent figure pack](III-04A-cake-flent-figure-pack.md) for the evidence
> behind the claims made here.

This chapter describes the CAKE client of the Stratum substrate: what CAKE
is, why it belongs in the substrate, how tins, shaping, and the ACK filter
are realised across the four strategy slots, and which Linux `sch_cake`
features the client carries. It is the architecture companion to the
[Stratum architecture chapter](II-02-stratum-architecture.md).

## What CAKE is

CAKE bundles flow isolation, AQM, optional rate-shaping, and a
DiffServ-aware multi-tin scheduler into a single queueing discipline.
Its components had previously been deployed individually; CAKE's
contribution is the coherent integration plus a few targeted
additions (ACK-filtering, set-associative hashing, host isolation):

| Mechanism | What CAKE provides | Origin |
|---|---|---|
| **Per-flow AQM** | COBALT — a CoDel variant with a Blue probability accumulator that addresses CoDel's slow-start latency spike | Palmei et al. 2019 |
| **Flow queueing** | DRR++ over per-flow buckets (the "++ " is a sparse-flow promotion) | Hoeiland-Joergensen 2018 |
| **Set-associative hash** | 8-way set-associative hashing on flow buckets — collisions move to the next bucket in the set rather than colliding into one starvation point | CAKE paper §4.3 |
| **Per-tin AQM** | Each DiffServ tin gets its own DRR++ + COBALT | CAKE paper |
| **Across-tin scheduling** | Deficit round-robin with per-tin quanta proportional to tin share weights | CAKE paper |
| **Rate shaping** | Per-tin token-bucket ceiling (optional) | CAKE paper |
| **ACK filtering** | Per-flow scan that drops older queued ACKs when a newer one makes them obsolete | CAKE paper §4.5 |
| **Tin profiles** | DSCP-to-tin mappings: `diffserv4` (4 tins), `diffserv3` (3 tins), `diffserv8` (8 tins) | tc-cake(8) |

Hoeiland-Joergensen, Taeht, Morton, and Chromy's 2018 paper
"Piece of CAKE: A Comprehensive Queue Management Solution for Home
Gateways" (arXiv:1804.07617) is the canonical reference; the Linux
manual page `tc-cake(8)` documents the deployed defaults.

### Why DSCP and not (just) ECN

L4S classifies on ECN; CAKE classifies on DSCP. The two clients are
complementary rather than alternative because they target different
deployment regimes:

- **L4S** assumes endpoints participate in a Scalable congestion
  control protocol; the network's job is to mark CE before queues
  fill. The classifier is two-bit (ECT(1)).
- **CAKE** assumes endpoints are heterogeneous (anything from a
  legacy TCP NewReno laptop to a Scalable-CC server) and that the
  operator wants service differentiation by traffic class. The
  classifier is six-bit DSCP, mapped to one of three to eight tins.

CAKE-as-DSCP-aware was a deliberate deployment choice: home gateways
need to differentiate between voice, video, bulk, and best-effort
traffic regardless of whether endpoints use Scalable CC, and the
DSCP field is the standard mechanism for that differentiation.

## Why CAKE belongs in the substrate

The [Stratum architecture chapter](II-02-stratum-architecture.md) describes the substrate's
four strategy slots. CAKE's tin scheduler is a
**scheduler extension**: it is a deficit-round-robin policy across slots
that is
distinct from the strict-priority default of the per-DSCP inner
dispatch substrate. CAKE's per-tin AQM is a **queue-disc choice**:
each tin's `Ptr<QueueDisc>` is set to a flow-queueing AQM
(`FqCobaltQueueDisc`), composed with the existing DSCP classifier
machinery without modification.

| Approach | Edge state | Core state | Marking | Across-class |
|---|---|---|---|---|
| Classic BA DiffServ (RFC 2475) | none | per-class | DSCP | strict priority |
| Per-flow srTCM edge meter (Stratum, 2026) | per-flow | per-class | DSCP 3-colour | strict priority |
| L4S / DualPI2 (RFC 9331/9332, 2023) | none | per-queue | ECT(1) + CE | strict priority |
| **CAKE (Linux 2018; this prototype 2026)** | **none (per-flow at queue level)** | **per-tin + per-flow** | **DSCP** | **deficit round-robin** |

CAKE's row reads almost like a hybrid: per-flow state (in the queue
discipline) but no edge-side per-flow meter; DSCP marking for tin
selection but per-flow fair queueing within each tin. The
Stratum substrate accommodates this by making the across-slot
dispatcher pluggable and the per-slot inner queue disc generic.

## Implementation overview

The CAKE client comprises four dispatcher classes plus a helper and
uses patched-mainline `FqCobaltQueueDisc` for per-tin queueing:

| Class | Role |
|---|---|
| `SlotDispatcher` | Base interface for across-slot dequeue policy |
| `StrictPriorityDispatcher` | Default policy — slot-0-first walk; preserves the pre-existing strict-priority dispatch behaviour byte-identically |
| `cake::TinShaperDispatcher` | DRR-across-slots policy with per-slot byte quanta; the across-tin scheduler CAKE installs |
| `cake::RateBasedShaperDispatcher` | Alternative across-slot dispatcher driven by per-tin and global virtual clocks; selectable via `cake::Helper::ShaperMode::RateBased` |
| `FqCobaltQueueDisc` (mainline, patched) | Per-tin inner queue disc. `EnableHostIsolation=true`, `HostIsolationMode=Triple`, `EnableSetAssociativeHash=true`, `SetWays=8` when host-isolation is enabled. The host-isolation attribute surface is added by a local ns-3 patch carried in the published artefact. |
| `cake::Helper` | Static composers `SetAsCakeDiffserv{3,4,8}` plus the `ShaperMode` selector wiring the per-tin inners + dispatcher + DSCP-to-tin map |

Per-flow AQM (COBALT) and set-associative-hash flow queueing are
inherited unchanged from mainline `FqCobaltQueueDisc` (`SetWays=8`,
`Quantum=1514`); per-flow byte accounting is the COBALT default,
which a smoke test confirmed produces fairness within 0.7% across
four saturating bulk flows on a single tin.

**Host isolation** is implemented via patched-mainline
`FqCobaltQueueDisc` with `EnableHostIsolation=true`,
`HostIsolationMode=Triple`, `EnableSetAssociativeHash=true`,
`SetWays=8` (attributes added by the local host-isolation patch
carried in the published artefact). The keying
follows Linux `sch_cake.c @ 67dc6c56b871`: each flow's DRR quantum is
divided by `max(srcCount, dstCount)` — the larger of its source-host and
destination-host active-flow counts — on flat per-flow DRR (there is no
separate outer queue per host), so a flow whose host runs many flows is
served with a proportionally smaller quantum. This includes the Amanakis
2019 bulk-only fix (kernel commit `712639929912`) and the Toke 2024/2025
bounds-check follow-ups (commits `546ea84d07e3`, `737d4d91d35b`).
Measurement confirms the patched-mainline path matches Linux `tc-cake`
within ≤4.3 percentage points across CUBIC, NewReno, and BBR at the
16-flow-versus-1-flow shared-sink anchor; the asymmetric split-destination
regime where isolation discriminates carries a fidelity boundary that is
substantially a measurement-configuration effect rather than a scheduler
divergence (see [Host-fairness empirical anchor](III-04-cake.md#host-fairness-empirical-anchor)).

### The composition

A `cake::Helper::SetAsCakeDiffserv4(edge, totalRate)` call wires:

```
EdgeQueueDisc
├── DscpToSlot[64]                            <-- Linux tc-cake diffserv4 map
├── m_inners[0..3]                            <-- four FqCobalt tins
│   ├── slot 0  Bulk      -> FqCobalt (8-way set-assoc, Quantum 1514)
│   ├── slot 1  BE        -> FqCobalt (idem)
│   ├── slot 2  Video     -> FqCobalt (idem)
│   └── slot 3  Voice     -> FqCobalt (idem)
└── m_slotDispatcher                          <-- cake::TinShaperDispatcher
    └── m_quantum[0..3]                       <-- proportional to tin share
                                                  (Bulk 6.25%, BE 100%, Video 50%, Voice 25%)
```

DRR with share-proportional quanta yields, in steady state on a
saturated bottleneck, byte-shares matching tin weights. Idle-tin
capacity redistributes to busy tins automatically — this is the
**work-conserving** form that Linux `tc-cake(8)` ships by default.

### Hard rate-caps: three coexisting paths

The CAKE paper's tin shaper is, strictly speaking, a per-tin
token-bucket rate-cap stacked above the per-tin DRR. The
substrate exposes three coexisting realisations of this cap,
selected through the `cake::Helper::ShaperMode` enum:

1. **Path α (`TokenBucket`, default).** A `cake::TinTokenBucket` POD
   inside `cake::TinShaperDispatcher` gates the DRR loop's serve
   decision before deficit credit. See [Tin-shaping mode](#tin-shaping-mode).
2. **Path β (`RateBased`).** Linux's verbatim rate-based
   virtual-clock shaper (Hoeiland-Joergensen et al. 2018, §III-A)
   with per-tin and global virtual clocks advanced via
   line-for-line correspondence to `cake_advance_shaper` and
   `cake_enqueue` from Linux `sch_cake.c @ 67dc6c56b871` (lines
   1533 and 1759). See [Rate-based virtual-clock shaper](#rate-based-virtual-clock-shaper-path-beta).
3. **Path γ (`TbfInner`).** Mainline `TbfQueueDisc` composed as
   the per-tin inner queue disc. The naive composition aborts at
   runtime because `TbfQueueDisc::DoDequeue` schedules a wakeup
   `Run()` event whose default body calls `Transmit(item)` against
   a null `m_send` callback (inner discs do not have a NetDevice
   aggregate, so the callback is never set). A one-line guard in
   `TbfQueueDisc::DoDequeue` defers the pacing wake to the parent
   qdisc when the TBF is composed as an inner disc; the guard ships
   as a local ns-3 patch in the published artefact, with an upstream
   merge request queued. See [Diagnostic output](#diagnostic-output-tc--s-qdisc-show-cake-mirror).

The DRR-quantum-proportional bandwidth allocation described in [The composition](#the-composition)
remains the default behaviour when no path is selected: the link
itself is the only hard cap; per-tin shares are enforced by
relative quanta. This matches Linux `tc-cake`'s default
`besteffort` and `diffserv*` modes (no `bandwidth` parameter ⇒
work-conserving).

Stacking `Helper::SetBandwidth` on a diffserv3-, diffserv4-, or
diffserv8-composed edge yields the integrated shaped composition — the analog
of `tc-cake bandwidth N <profile>`: the aggregate virtual-clock pair is the
only hard gate, per-tin clocks demote selection priority without capping, and
each tin keeps its per-flow fair queueing and Cobalt AQM. Each composer
records its profile on the edge (an aggregated marker object), and
`SetBandwidth` derives that profile's Linux tin-rate ladder from the record —
the analog of Linux re-deriving tin parameters from the stored `tin_mode`
whenever any knob changes, so the caller never re-declares the profile at
rate-set time. Without a bandwidth, the edge runs the work-conserving deficit
round-robin across tins (the unshaped mode); the standalone rate-based queue
disc remains available where plain FIFO tins suffice.

### ACK filter

CAKE's ACK filter scans the per-flow queue when a new TCP ACK
arrives and drops older queued ACKs that have been superseded
(same 5-tuple, strictly-smaller cumulative ACK number, no SACK
and no flag change). On asymmetric links this frees upstream
capacity for data and recovers downstream throughput; the CAKE
paper reports around a 15% downstream gain at 30/1 Mbit/s
(Figure 6).

The substrate exposes the feature via the
`enableAckFilter` opt-in on every `cake::Helper` composer. The
helper toggles the `EnableAckFilter` boolean attribute on
mainline `FqCobaltQueueDisc`, which mainline gained via a local
ns-3 patch carried in the published artefact (awaiting upstream
review; auto-applied by `scripts/fetch-ns3.sh`).
On every arriving TCP ACK the queue disc walks the active flow
set, drops older redundant ACKs in matching 5-tuples by draining
and re-enqueuing the inner per-flow queue around them, and then
delegates to the parent's standard enqueue path. The candidate
classifier and the per-flow scan parse raw IP+TCP bytes from
`QueueDiscItem::GetPacket()` rather than `TcpHeader` /
`Ipv4QueueDiscItem`, mirroring how Linux `sch_cake.c @ 67dc6c56b871`
walks raw `skb` bytes. This avoids a circular module dependency:
mainline `traffic-control` links `network` only, so the helpers stay
inside the `traffic-control` module without pulling in
`internet`. The 5-tuple match itself uses the polymorphic
`QueueDiscItem::Hash` declared in `network`.

Two filter modes are exposed, matching Linux `tc-cake`:

- **Conservative** (default): SACK-bearing ACKs are never candidates
  and never drop targets. The strictly-less-than cumulative-ACK
  trigger preserves dup-ACK fast-retransmit signalling.
- **Aggressive** (`EnableAckFilterAggressive=true` on top of
  `EnableAckFilter=true`): SACK-bearing ACKs are admitted both as
  scan triggers and as drop targets. The cumulative-ACK trigger is
  unchanged, so dup-ACK signalling is still preserved; only the
  SACK-presence rejection is dropped. Useful on heavily-asymmetric
  paths where ACK volume dominates the upstream link enough that
  even SACK-bearing ACKs are worth thinning when superseded.

The aggressive toggle is reachable via the attribute path on the
per-tin instance, or via the optional `enableAckFilterAggressive`
parameter appended at the end of every `cake::Helper::SetAsCake*`
signature (default `false`; preserves source compatibility for
existing positional callers).

**Fidelity boundary.** The filter is a faithful port of the Linux
`cake_ack_filter()` algorithm — candidate classification, the
cumulative-ACK trigger, the SACK and flag-change guards, and the
conservative/aggressive split all follow `sch_cake.c`, verified by
side-by-side source audit. Both IPv4 and IPv6 TCP flows are covered:
`Ipv4QueueDiscItem` and `Ipv6QueueDiscItem` each store their L3 header
separately and expose the TCP header at byte-0 of `GetPacket()`, so the
same parse path applies to both; `GetIpProtocol()` supplies the IP-version
discriminant used by the flow-key gate (matching `sch_cake.c`'s
`iph_check->version == 6` branch at line 1279). The observable benefit, however, depends
on the operating regime. On Linux the paper-strict 30/1 Mbit/s,
40 ms workload (4 TCP down + 4 TCP up) yields a 1.09×–1.14×
downstream gain over three seeds and two traffic generators (Flent,
iperf3), consistent with the CAKE paper's reported ~15%. Deterministic
ns-3 at 30/1 does not surface that gain: without the NAPI/softirq
timing jitter that mediates ACK-clocking recovery on a real kernel,
the downstream channel sits near its ceiling and there is little for
the filter to recover. The in-simulator benefit appears at the more
asymmetric 100:1 regime (50/0.5 Mbit/s), where the substrate records
a 1.105×–1.171× gain. The gap is a property of the simulator
environment, not of the filter algorithm; surfacing the 30/1 gain
in-simulator would require modelling NAPI/softirq-level timing, which
is outside the current scope. For empirical results see
[CAKE validation](III-04-cake.md#validation).

Compositionally, under host-isolation the ACK filter runs within each per-host
bucket of the patched-mainline `FqCobaltQueueDisc`. Each bucket
independently scans its own flow classes for redundant ACKs in
matching 5-tuples. Cross-bucket comparison is structurally moot —
host-isolation by construction places distinct 5-tuples into distinct
buckets, and ACK redundancy is defined by 5-tuple identity. The
`EnableAckFilter` and `EnableAckFilterAggressive` attributes are
set on each per-tin `FqCobaltQueueDisc` instance before
`Initialize()`, following the same attribute-propagation pattern
as the other per-tin knobs.

## Tin profiles (DSCP-to-tin maps)

`cake::Helper` provides three Linux `tc-cake(8)` profile composers.
Each DSCP-to-tin mapping reproduces the corresponding `sch_cake`
lookup table byte-exactly (the `diffserv3[]`/`diffserv4[]`/
`diffserv8[]` arrays in the frozen provenance excerpt), and an
in-suite fixture walks all 64 codepoints per profile against those
tables:

### `SetAsCakeDiffserv4` (4 tins)

| Tin | Name | Share | DSCPs (decimal) |
|---|---|---|---|
| 0 | Bulk | 6.25% | LE=1, CS1=8 |
| 1 | Best-Effort | 100% | DF=0, TOS2=2, AF11=10, AF12=12, AF13=14, and all unspecified |
| 2 | Video | 50% | TOS4=4, CS2=16, AF21..AF23, CS3=24, AF31..AF33, AF41..AF43 |
| 3 | Voice | 25% | CS4=32, CS5=40, VA=44, EF=46, CS6=48, CS7=56 |

This is the standard CAKE deployment for residential gateways where
voice and video classes need protection from bulk download traffic.
Note that the AF1x codepoints are Best-Effort, not Bulk or Video —
`sch_cake` reserves Bulk for the explicit scavenger codepoints
(LE, CS1).

### `SetAsCakeDiffserv3` (3 tins)

| Tin | Name | Share | DSCPs (decimal) |
|---|---|---|---|
| 0 | Bulk | 6.25% | LE=1, CS1=8 |
| 1 | Latency-Sensitive | 25% | TOS4=4, VA=44, EF=46, CS6=48, CS7=56 |
| 2 | Best-Effort | 100% | DF=0 and all others (default) |

Simpler split: scavenge / latency-sensitive / best-effort. The
Latency-Sensitive tin is deliberately narrow — only the five
codepoints `sch_cake` itself elevates; AF classes and CS2..CS5 ride
Best-Effort.

### `SetAsCakeDiffserv8` (8 tins)

| Tin | Coverage | Share |
|---|---|---|
| 0 | Background: LE | 100.0% |
| 1 | High Throughput: TOS2, CS1, AF11..AF13 | 87.5% |
| 2 | Bog Standard: DF and all unspecified | 76.6% |
| 3 | Video Streaming: CS3, AF31..AF33, AF41..AF43 | 67.0% |
| 4 | Low-Latency Transactions: TOS4, AF21..AF23 | 58.6% |
| 5 | Interactive Shell: CS2 | 51.3% |
| 6 | Minimum Latency: CS4, CS5, VA, EF | 44.9% |
| 7 | Network Control: CS6, CS7 | 39.3% |

The eight classes and their order follow `sch_cake`'s pruned
traffic-class list; the share column is the kernel's geometric
quantum ladder (each tin at 7/8 of the previous, anchored at the
first tin), under which lower tins carry larger bandwidth-sharing
weights and higher tins rely on selection priority instead. The
defaults may be adjusted after construction via
`cake::TinShaperDispatcher::SetQuantum(slot, bytes)`.

## Composition with the substrate

CAKE shares the substrate with DiffServ and L4S:

```
                                edge classifier
                                       |
                              meter / policer
                                       |
                              DSCP-to-slot map
                              /     |        \
              [DiffServ]   [L4S]            [CAKE]
              priority      priority          DRR
              dispatcher    dispatcher        dispatcher
                  |             |               |
              RED inners    DualPI2         FqCobalt(set-assoc)
                                            x N tins
```

The pluggable dispatcher abstraction is the substrate point that
makes CAKE possible: the same edge class, the same classifier table,
and the same DSCP-to-slot map work for all three clients. Only the
across-slot dispatch policy (and the per-slot inner queueing) changes.

## Tin-shaping and Cisco MQC LLQ modes

Two further dispatcher modes raise the per-slot
configuration matrix to four entries (work-conserving DRR, LLQ-on-EF,
tin-shaping, LLQ × tin-shaping) and give the helper API parity with
the most-deployed production CAKE configurations. Both modes are
opt-in and orthogonal: the default helper invocation remains
byte-identical to the work-conserving DRR baseline.

### Tin-shaping mode

Tin-shaping installs a per-tin token-bucket gate inside
`cake::TinShaperDispatcher` such that each tin's serve rate is hard-capped
at `share × totalRate`. This matches Linux `tc-cake bandwidth N
<profile>` — the production default for CAKE on home gateways and
operator equipment. Idle-tin capacity does **not** redistribute to
busy tins (unlike the work-conserving DRR-only mode of [The composition](#the-composition)):
each tin runs against its own ceiling.

```cpp
cake::Helper::SetAsCakeDiffserv4(edge, DataRate("10Mbps"),
                                  /*enableAckFilter*/ false,
                                  /*enableLlq*/ false,
                                  /*enableTinShaping*/ true);
```

In `diffserv4` profile terms, Bulk caps at 6.25% (625 kbps on a
10 Mbit/s link), Voice at 25% (2.5 Mbit/s), Video at 50%, BE at 100%
of the aggregate. A saturating Bulk flow can never starve Voice; an
idle Voice tin does not yield bandwidth back to Bulk. The mechanism
is the per-slot `cake::TinTokenBucket` (POD; `rateBps`, `burstBytes`,
`tokensBytes`, `lastUpdate`) gating the DRR loop's serve decision
before deficit credit.

Use this mode when reproducing Linux `tc-cake`'s default semantic,
when modelling a deployment with hard SLA per traffic class, or when
the application demands predictable per-class ceilings independent
of cross-tin load. Cross-implementation calibration against Linux
`tc-cake` holds to a ±15 pp envelope, matching the established
ns-2/ns-3 calibration tolerance.

### LLQ × tin-shaping (Cisco MQC pattern)

Composing LLQ-on-EF with per-tin rate-caps yields a fourth dispatcher
mode in which the latency-sensitive tin (Voice in `diffserv4`,
Latency-Sensitive in `diffserv3`, CS6/EF/VA in `diffserv8`) is served
both with **strict-priority fast-path** (sub-floor jitter as in pure
LLQ mode) **and** under a **hard ceiling** (the SP class can never
exceed its configured share). This mirrors Cisco MQC's `priority
percent N` template — the most-deployed enterprise QoS pattern,
canonised in IOS LLQ documentation since 1999 and reproduced across
Juniper, Arista, and most enterprise router vendors.

```cpp
cake::Helper::SetAsCakeDiffserv4(edge, DataRate("10Mbps"),
                                  /*enableAckFilter*/ false,
                                  /*enableLlq*/ true,
                                  /*enableTinShaping*/ true);
```

The composition is correct because the `Charge` step in
`HybridLlqDispatcher::OnDequeue` runs **before** the SP early-return
branch: an SP-marked Voice slot with a configured rate cap drains its
bucket on every dequeue, including
the SP fast-path serves, so a saturating EF flow cannot escape the
25% ceiling and starve Bulk/BE/Video. Without this ordering the
ceiling would silently no-op and the Cisco MQC correctness anchor
would not hold.

Use this mode for enterprise QoS replication (Cisco MQC LLQ +
bandwidth-cap) and for modelling SLAs on EF/voice classes that demand
both jitter protection AND a hard ceiling against EF starvation
attacks. An in-suite structural test pins the Cisco MQC composition,
paired with the cross-implementation latency calibration against
Linux `tc-cake`.

## Feature scope and deferred items

### Implemented in this release

- DRR-across-tins via `cake::TinShaperDispatcher` ✓
- Per-tin `FqCobaltQueueDisc` with 8-way set-associative hash ✓
- Per-flow COBALT AQM (inherited from FqCobalt) ✓
- DSCP-to-tin maps for `diffserv3`, `diffserv4`, `diffserv8` ✓
- Work-conserving bandwidth allocation (Linux `tc-cake` default mode) ✓
- ACK filter (Linux `tc-cake` `ack-filter` mode) — functional via
  the `EnableAckFilter` attribute on mainline `FqCobaltQueueDisc`,
  conservative + aggressive variants ✓
- LLQ-on-EF mode via `HybridLlqDispatcher` ✓
- Tin-shaping (path α) via `cake::TinTokenBucket` gate in
  `cake::TinShaperDispatcher` ✓
- Rate-based virtual-clock shaper (path β) via
  `cake::RateBasedShaperDispatcher` with per-tin and global virtual
  clocks; per-packet `adj_len` from `cake_calc_overhead` ✓
- Mainline `TbfQueueDisc` as per-tin inner (path γ) via the
  `useInnerTbfShaping` opt-in (alias for
  `ShaperMode::TbfInner`) ✓
- LLQ × tin-shaping (Cisco MQC pattern) via the same gate inside
  `HybridLlqDispatcher` ✓
- Host isolation (Linux `tc-cake` `triple-isolate` plus the
  named `srchost` / `dsthost` / `hosts` / `flowblind` / `flows`
  modes; `dual-srchost` / `dual-dsthost` alias their
  non-`dual` counterparts) via patched-mainline
  `FqCobaltQueueDisc` (`EnableHostIsolation=true`,
  `HostIsolationMode=Triple`, `EnableSetAssociativeHash=true`,
  `SetWays=8`) ✓
- **Egress DSCP wash** (Linux `tc-cake` `wash` mode) via the
  `EdgeQueueDisc::Wash` boolean attribute ✓
- **Per-tin `MemLimit` byte cap** (Linux `tc-cake` `memlimit
  BYTES` mode) — functional via the `MemLimit` attribute on
  mainline `FqCobaltQueueDisc` ✓
- **Per-packet `overhead` / `atm` / `mpu` framing** (Linux
  `tc-cake` link-layer framing options) via
  `cake::Helper::ConfigureLinkLayerOverhead` and the
  `cake::RateBasedTinClock::ComputeAdjLen` helper ✓
- **Named link-layer presets** (`cake::Helper::SetLinkLayer`) — 15
  Linux `tc-cake(8)` keyword presets resolved to their
  `(overhead, framing, MPU)` tuples ✓
- **Named RTT presets** (`cake::Helper::SetRttPreset`) — 8 Linux
  `tc-cake(8)` RTT-regime keywords mapped to their `(target, interval)`
  pairs ✓
- **Live bulk-flow counter** (`cake::LiveBulkCounter`) — opt-in
  wrapper providing a live `bulk_flow_count` matching Linux `tc -s`
  semantics ✓
- **Integrated shaped composition** (Linux `tc-cake bandwidth N`
  mode) via `cake::Helper::SetBandwidth` — the aggregate
  virtual-clock gate over per-tin demotion clocks, dispatched on the
  tin profile recorded at composition time
  (`diffserv3` / `diffserv4` / `diffserv8`) ✓
- **Autorate-ingress closed loop** (Linux `tc-cake`
  `autorate-ingress` mode) via `cake::Helper::SetAutorateImpl` plus
  the path-β `cake::LinuxAutorateHook` — a peak-bandwidth EWMA
  estimator, byte-exact to `sch_cake.c`'s `cake_enqueue`, that
  retargets the aggregate shaper to the inferred downstream
  bottleneck ✓

### Named link-layer presets

For common link technologies, `cake::Helper::SetLinkLayer(edge, preset)`
resolves a Linux `tc-cake(8)` keyword to the matching
`(overhead, framing, MPU)` tuple and applies it via the numeric setter.
The keywords match Linux:

| Preset           | Overhead | Framing |  MPU |
|------------------|---------:|---------|-----:|
| `Ethernet`       |       38 | —       |   84 |
| `EtherVlan`      |       42 | —       |   84 |
| `Docsis`         |       18 | —       |   64 |
| `PppoePtm`       |       30 | PTM     |    — |
| `PppoeVcmux`     |       32 | ATM     |    — |
| `PppoeLlcsnap`   |       40 | ATM     |    — |
| `PppoaVcmux`     |       10 | ATM     |    — |
| `PppoaLlc`       |       14 | ATM     |    — |
| `BridgedPtm`     |       22 | PTM     |    — |
| `BridgedVcmux`   |       24 | ATM     |    — |
| `BridgedLlcsnap` |       32 | ATM     |    — |
| `IpoaVcmux`      |        8 | ATM     |    — |
| `IpoaLlcsnap`    |       16 | ATM     |    — |
| `Conservative`   |       48 | —       |   64 |
| `Raw`            |        0 | —       |    — |

**Note on `Conservative`.** The substrate `Conservative` preset uses
overhead-only accounting (no ATM cell quantisation), matching the
long-standing substrate default. Linux `tc-cake conservative` uses
ATM cell quantisation in addition to the 48-byte overhead; if ATM
accounting is required, use `ConfigureLinkLayerOverhead` directly with
`atm=true`.

```cpp
cake::Helper::SetLinkLayer(edge, cake::Helper::LinkPreset::PppoePtm);
```

For values not covered by the table, call `ConfigureLinkLayerOverhead`
directly with explicit `overhead`, `atm`, `ptm`, and `mpu` arguments.

### RTT presets

CoDel's `target` (sojourn target) and `interval` (observation window)
scale with the path RTT. `cake::Helper::SetRttPreset(edge, preset)`
applies a Linux `tc-cake(8)` named regime:

| Preset           |    Target |  Interval |
|------------------|----------:|----------:|
| `Datacentre`     |      5 µs |   100 µs  |
| `Lan`            |     50 µs |     1 ms  |
| `Metro`          |    500 µs |    10 ms  |
| `Regional`       |   1.5 ms  |    30 ms  |
| `Internet`       |     5 ms  |   100 ms  |
| `Oceanic`        |    15 ms  |   300 ms  |
| `Satellite`      |    50 ms  |  1000 ms  |
| `Interplanetary` |     50 s  |  1000 s   |

`Internet` matches the RFC 8289 CoDel default and is the implicit
substrate default — applying it is a no-op. For arbitrary RTT regimes,
set `Target` and `Interval` on the inner `FqCobaltQueueDisc` directly
via `SetAttribute("Target", StringValue("..."))` (these are
string-typed time attributes in mainline ns-3).

```cpp
cake::Helper::SetRttPreset(edge, cake::Helper::RttPreset::Satellite);
```

Under host-isolation (`enableHostIsolation=true`), the preset is
applied to the patched-mainline `FqCobaltQueueDisc` for each tin
slot at composition time via `SetAttribute`.

### Egress DSCP wash

When the CAKE substrate sits at the edge of an administrative
boundary, the operator may want classification to drive scheduling
inside the qdisc but the egress packet to leave with a clean
DSCP byte so downstream forwarders see CS0/Default (and their own
classification policy applies, not ours).  Linux `tc-cake` exposes
this as the `wash` mode.

The substrate ships the same toggle as a `EdgeQueueDisc`
boolean attribute:

```cpp
edge->SetAttribute("Wash", BooleanValue(true));
```

Default is `false` (the rewrite stamps the classifier-assigned DSCP
into the IPv4 TOS byte's high six bits, mirroring the existing
classification-aware behaviour).  When set to `true`, `DoDequeue`
zeros those six bits while preserving the low two ECN bits, so an
ECT(0)/ECT(1)/CE marking from an inner AQM survives the wash and
remains observable downstream.

The attribute is orthogonal to every CAKE composition flag — it
sits on the edge disc, not inside the dispatcher or the per-tin
inners — and composes cleanly with any of the
`cake::Helper::SetAsCake*` presets, with or without LLQ,
tin-shaping, host-isolation, or ACK filtering.

### Per-tin `memlimit` (byte-based queue cap)

Linux `tc-cake` exposes a `memlimit BYTES` keyword that caps each
tin's total queue depth in bytes (rather than packets). The
substrate ships the same byte cap as a uinteger attribute on
mainline `FqCobaltQueueDisc` (the per-tin inner queue disc):

```cpp
cake::Helper::SetAsCakeDiffserv4(edge, DataRate("100Mbps"));
for (uint32_t s = 0; s < edge->GetNumInnerSlots(); ++s)
{
    edge->GetInnerDiscAt(s)->SetAttribute("MemLimit",
                                          UintegerValue(200000));
}
```

Default is `0` (disabled). When non-zero, an arriving packet is first
enqueued and run through the ACK-filter scan as usual; only then, if
the disc's byte total still exceeds the cap, an eviction loop drops
one packet per iteration from the head of the longest flow queue
until the disc is back under cap, crediting each eviction with drop
reason `MEMLIMIT_DROP`. This tail-eviction from the longest flow, which
runs after enqueue and ACK-filter scan, mirrors Linux `sch_cake.c`'s
`cake_drop()` semantics rather than
rejecting the arrival at the door. The packet-count `MaxSize`
attribute remains independent and unaffected.

The byte cap lives inside mainline `FqCobaltQueueDisc::DoEnqueue`
itself rather than in a DiffServ4NS subclass; the per-tin inner is
a stock `FqCobaltQueueDisc`. A local ns-3 patch carried in the
published artefact adds the attribute and the byte-counted eviction
loop (awaiting upstream review; auto-applied by
`scripts/fetch-ns3.sh`). Under host-isolation the byte cap applies
per-tin; finer-grained per-host cap accounting within one tin is
follow-up work.

### Rate-based virtual-clock shaper (path beta)

`cake::RateBasedShaperDispatcher` implements Linux's verbatim
rate-based virtual-clock shaper alongside path α
(`cake::TinTokenBucket`) and path γ (mainline `TbfQueueDisc` as inner).
The dispatcher is selected by setting
`cake::Helper::SetShaperMode(ShaperMode::RateBased)` before
`BuildAndInstall`. The default remains `ShaperMode::TokenBucket`.

Two virtual clocks, both lifted line-for-line from Linux
`sch_cake.c @ 67dc6c56b871`:

- **Per-tin clock** (`cake::RateBasedTinClock`) — three-branch advance
  in `Charge`, mirroring `cake_advance_shaper`. Idle-tin snap-to-
  now in `OnEnqueueIdleReset` mirrors `cake_enqueue`. Per-packet
  `adj_len` from `cake::RateBasedTinClock::ComputeAdjLen` mirrors
  `cake_calc_overhead` (signed overhead, MPU floor after overhead
  add, ATM `((adj+47)/48)*53`, PTM `adj+(adj+63)/64`).
- **Global clock pair** (`cake::RateBasedGlobalClock`) — the primary
  clock advances unconditionally on every dequeue and a failsafe
  companion advances at 1.5× the primary duration (skipped on
  ingress-mode drop charging), mirroring `q->time_next_packet` and
  `q->failsafe_next_packet`. The aggregate is gated only while both
  clocks are in the future. Binds total egress when
  `sum(tin demands) > global cap`; required for parity with Linux
  `tc-cake bandwidth N` mode.

Across-tin selection follows Linux's shaped mode: the global clock
pair is the only hard gate, and per-tin clocks rank rather than
block. Among backlogged tins, the highest-priority tin whose clock
meets its schedule is served; when none meets it, the
earliest-scheduled backlogged tin is served anyway — a lone
backlogged tin therefore receives the full configured bandwidth
(work conservation), and tin rates act as priority-demotion
thresholds, not ceilings. Tin priority is the layout's permutation
supplied by the helper (for `diffserv4`: Best-Effort < Bulk < Video
< Voice, matching Linux's tin scan order, which differs from the
slot order in the Bulk/Best-Effort pair). Hard per-tin ceilings
remain the job of paths α and γ. A single outstanding `SelfWake`
event arms at the global pair's effective gate, so the dispatcher
does not busy-poll during sustained backlog.

In-suite fixtures pin the shaper: the throughput-parity gate (path β
vs path γ within ±2 %), the global-cap gate (aggregate egress within
(95, 102) Mbps), a single-tin work-conservation gate (a lone
backlogged tin reaches the cap), and a schedule-meeting priority
gate (a Voice flow within its allowance is served in full while a
saturating Best-Effort flow takes the work-conserving remainder).

### Diagnostic output (`tc -s qdisc show cake` mirror)

`cake::Helper::PrintTcStats(os, edge)` writes a Linux-compatible
diagnostic dump for a CAKE-composed `EdgeQueueDisc`. The
section ordering and section-key vocabulary mirror Linux
`iproute2 tc/q_cake.c::cake_print_xstats`, which operators rely on
for at-a-glance CAKE health checks. The thin helper method
delegates to the standalone `cake::StatsFormatter::Print` so
scenario scripts can call either entry point.

```cpp
cake::Helper helper;
helper.SetShaperMode(cake::Helper::ShaperMode::TokenBucket);
Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
cake::Helper::SetAsCakeDiffserv4(edge, DataRate("100Mbps"));
edge->Initialize();
// ...drive traffic...
helper.PrintTcStats(std::cout, edge);
```

Sample output (abbreviated; whitespace canonical):

```
qdisc cake 0: dev (ns-3) tins 4
 Sent 6120 bytes 6 pkt (dropped 0, overlimits 0 requeues 0)
 backlog 5100b 5p requeues 0
 memory used: n/a of n/a
 capacity estimate: n/a
 tin 0 kind=fq_cobalt
  thresh 6Mbit
  bytes_enqueued 0 bytes_dequeued 0
  drops 0 marks 0
  ever_seen 0 (substrate-gap: stock FqCobaltQueueDisc class list is append-only; ...)
  backlog 0b 0p
 tin 1 kind=fq_cobalt
  thresh 100Mbit
  ...
 tin 3 kind=fq_cobalt
  thresh 25Mbit
  bytes_enqueued 4080 bytes_dequeued 1020
  drops 0 marks 0
  ever_seen 1 (substrate-gap: ...append-only...)
  backlog 3060b 3p
```

**Bulk-flow count — live counter opt-in.** By default the `ever_seen`
field reports `inner->GetNQueueDiscClasses()`, which reflects the
append-only per-flow class list that stock ns-3 `FqCobaltQueueDisc`
maintains. The field name `ever_seen` deliberately differs from
Linux's `bulk_flow_count` so a downstream parser cannot mistake it
for the live count Linux `tc -s` reports.

To obtain a live count matching Linux semantics, attach a
`cake::LiveBulkCounter` to the edge before traffic starts:

```cpp
cake::Helper::AttachLiveBulkCounter(edge);
// ... run the simulation ...
uint32_t live = cake::Helper::GetLiveBulkCount(edge, /*slot=*/1);
```

The wrapper hooks each per-tin inner `FqCobaltQueueDisc`'s `Enqueue`
trace source, maintaining a per-flow last-seen timestamp map. A flow
drops out of the live count once it has been idle for longer than
`8 × Interval` (matching Linux's `bulk_flow_threshold`). When the
live counter is attached, `PrintTcStats` substitutes its value for the
`ever_seen` approximation. Sparse and unresponsive flow counts remain
approximated against the mainline class list.

**Other substrate gaps.** The aggregate `memory used` and
`capacity estimate` fields are reported as `n/a` rather than
fabricated. The substrate has no aggregate memlimit counter
(per-tin `MemLimit` lives on each inner; an aggregate counter is
follow-up work — see [Per-tin `memlimit`](#per-tin-memlimit-byte-based-queue-cap)) and no learned link-capacity
estimator. The lines are
emitted regardless so a parser written against the section-key
vocabulary still finds them.

The output is structural, not byte-exact: future cosmetic iproute2
changes (whitespace, decimal precision, label re-ordering within a
section) shall not regress the paired in-suite fixture.

## Behavioural mechanisms

The following subsections cover behavioural additions to the CAKE
client that go beyond passive feature-parity: aggregate memory-limit
eviction, ingress shaping mode, and the Linux autorate-ingress closed loop.
Each targets a specific `sch_cake.c` behaviour that the feature-parity
additions left as a no-op or gap.

### Memory limit and host-isolation eviction

The patched-mainline `FqCobaltQueueDisc` accepts a `MemLimit` attribute
(bytes; 0 = unbounded) that caps the per-tin total queue depth in bytes.
When the per-tin byte total exceeds the cap after enqueueing, the
drop-from-heaviest logic mirrors Linux `sch_cake.c`'s eviction semantics. Set the attribute
before `Initialize()` on each per-tin inner:

```cpp
edge->GetInnerDiscAt(s)->SetAttribute("MemLimit",
                                      UintegerValue(4 * 1024 * 1024));
```

Finer-grained per-host cap accounting within one tin (for
host-isolated setups) is follow-up work (see [Per-tin `memlimit`](#per-tin-memlimit-byte-based-queue-cap)).

### Ingress shaping mode

The path-β rate-based shaper (`cake::RateBasedShaperDispatcher`) supports a
`cake-ingress` flag. When enabled, per-tin and global clocks advance on dropped
packets as well as forwarded ones — so the configured rate is the line
rate of incoming traffic, not the rate of forwarded traffic. Use this for
"shape the arriving downlink" home-gateway setups.

```cpp
cake::Helper helper;
helper.SetShaperMode(cake::Helper::ShaperMode::RateBased);
helper.SetEnableIngressMode(true);
helper.SetGlobalRateBps(100'000'000);  // 100 Mbps configured arrival rate
```

Ingress accounting in this version covers overflow drops at the dispatcher
boundary. Drops decided by the inner AQM (CoDel/COBALT) are not currently
visible to the dispatcher for ingress accounting.

### Autorate-ingress closed loop

Selecting the Linux autorate implementation activates a closed-loop rate
tracker on the path-β shaper. Rather than hold the configured rate, the shaper
infers the downstream bottleneck from the arrival stream and retargets itself
to track it. The hook reproduces `sch_cake.c`'s incoming-capacity estimator
(the `cake_enqueue` autorate branch): each accepted packet's wire length
accumulates into an open measurement window; an inter-arrival
exponentially-weighted moving average filters short-term bursts; and when an
inter-arrival exceeds that running average the window closes, folding its
bytes-per-second into a peak-bandwidth estimate. The reconfigure target is
15/16 (≈ 93.75 %) of that estimate, throttled by a 250 ms deadband.

The kernel filters the two directions asymmetrically. The peak estimate
*attacks upward* quickly — a one-quarter weight — when a faster window
appears, and *decays downward* slowly — a 1/256 weight — when traffic falls.
A link that suddenly offers more capacity is tracked within a second or two;
a transient dip cannot collapse the shaped rate.

```cpp
cake::Helper helper;
helper.SetShaperMode(cake::Helper::ShaperMode::RateBased);
helper.SetAutorateImpl(cake::Helper::AutorateImpl::Linux);
helper.SetEnableAutorateIngress(true);
```

**Convergence.** Driven by a single-packet arrival train whose spacing encodes
a 10 Mbit/s bottleneck, and starting from a 2 Mbit/s bootstrap guess, the
aggregate shaper climbs to **9.51 Mbit/s** within a fraction of a second and
holds. When the bottleneck steps up to 20 Mbit/s, the shaper re-adapts upward
to **19.02 Mbit/s**. Both sit a little above 15/16 of the nominal bottleneck
(9.375 and 18.75 Mbit/s) because the measured wire length includes the 20-byte
IPv4 header — the shaper tracks the *delivered* byte rate, header and all.

![CAKE autorate-ingress convergence. The aggregate path-β shaper starts at a 2 Mbit/s bootstrap, tracks upward to 9.51 Mbit/s against a 10 Mbit/s bottleneck — just above the dashed 15/16 set-point, the small excess being the counted 20-byte IPv4 header — and re-adapts upward to 19.02 Mbit/s after the bottleneck steps to 20 Mbit/s at the two-second mark. Deterministic single-packet workload, σ = 0.](figures/cake-autorate-converge/cake-autorate-converge.svg)

**Sticky downward.** The slow-decay direction is the counterpart. Seeded at
20 Mbit/s and then offered only 5 Mbit/s, the estimate is still **11.3 Mbit/s**
half a second later — more than twice the new offered rate — and reaches
**4.69 Mbit/s** (15/16 of 5 Mbit/s) only after a full thirty seconds. The slow
descent is what keeps a momentary lull from starving the shaper; upward
re-adaptation completes in a second or two, downward decay takes tens of
seconds.

The estimator is byte-exact to `cake_enqueue`: the running state evolves in
the kernel's native bytes-per-second unit, seeded from the configured aggregate
rate, with the same two-term moving-average rounding, the same one-second
inter-arrival cap, and the same window-accumulation order. The
bits-per-second conversion the dispatcher works in happens only at the seed
input and the target output, so the estimate evolves byte-for-byte with the
kernel on the same arrival stream.

Default selector is `AutorateImpl::NoOp` — a zero-delta hook that produces
byte-identical wire output to the autorate-disabled state. The Linux
implementation only attaches when both `SetAutorateImpl(Linux)` and
`SetEnableAutorateIngress(true)` are set.

This release reproduces the estimator and the reconfigure algorithm, not the
netfilter ingress redirect that Linux's `autorate-ingress` rides on. The rate
tracker runs on the path-β shaper directly: there is no IFB device and no
`tc`-ingress hook, so the closed loop reshapes the rate the substrate already
sees rather than redirecting a separate ingress qdisc. The CAKE paper motivates
the set-point — shape just below the physical capacity so the managed queue
stays in CAKE rather than building at the bottleneck (§III-A) — but leaves the
estimator unspecified; the algorithm is Linux's.

### Deferred items

- **Per-flow and per-host counters.** The current release ships the
  host-isolation modes but not the enumeration counters that Linux
  exposes via `tc-cake -s`. The counters extend the per-tin trace
  surface.
- **`memlimit` + host-isolation composition.** The byte-cap
  attribute is scoped per-tin. Finer-grained per-host cap accounting
  within one tin is follow-up work.
- **Additional CAKE-paper cross-validations.** Further
  cross-validation scenarios beyond the ones already replicated are
  gated on the Flent-application sibling project; see
  [CAKE validation](III-04-cake.md#validation) for the current anchors.
