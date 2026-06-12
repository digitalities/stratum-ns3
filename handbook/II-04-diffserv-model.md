---
title: The DiffServ model
origin: inspired-by-thesis-§2
status: filled
last-updated: 2026-06-06
---

# The DiffServ model

Architecture and behaviour of the IETF Differentiated Services (DiffServ)
framework. This chapter mirrors the structural arc of thesis Chapter 2 —
architecture, DS field, PHBs, traffic conditioning — and the RFC chronology
that shaped the modern DSCP-marking landscape the 2026 port runs in. The [DiffServ client chapter](II-05-diffserv-client.md) maps these concepts onto the
DiffServ4NS object model; Appendix A lists the RFC conformance vectors used
by the ns-3 test suite.

## 1. The DiffServ architecture (RFC 2475)

DiffServ (RFC 2475, 1998) is a scalable reaction to the flow-state explosion
of IntServ. Instead of per-flow reservations across every hop, DiffServ
partitions the network into **domains** whose boundary is patrolled by
**edge routers** and whose interior is populated by **core routers**. The
two roles have asymmetric responsibilities:

| Role | Where | Responsibilities |
|---|---|---|
| Edge router | Domain boundary | Classify, meter, mark (DSCP), shape, police |
| Core router | Domain interior | Apply Per-Hop Behaviour (PHB) based on DSCP; forward |

All per-flow complexity is pushed to the edge; the core sees only aggregate
**Behaviour Aggregates** (BA) identified by DSCP and applies a small fixed
set of forwarding treatments. This is the scalability argument behind
DiffServ: the forwarding path is stateless with respect to flows.

**Traffic conditioning** at the edge comprises four stages (RFC 2475 §2.3):

1. **Classification** — map arriving packets to a class, either by the DS
   field alone (BA classifier) or by a multi-field tuple (MF classifier:
   source/destination address, protocol, ports, application type).
2. **Metering** — measure the temporal properties of a classified stream
   against a traffic profile (CIR, CBS, PIR, PBS).
3. **Marking** — write a DSCP into the DS field of each packet based on the
   meter outcome.
4. **Shaping / policing** — delay (shape) or drop (police) packets that
   exceed the profile.

The domain boundary is also where **Traffic Conditioning Agreements** (TCA,
the technical instantiation of an SLA) are enforced. A packet that enters a
DiffServ domain without being conditioned at an edge is untrusted and may be
re-marked or dropped.

## 2. DSCP marking (RFC 2474)

RFC 2474 (1998) redefines the IPv4 TOS octet and the IPv6 Traffic Class
octet as the **Differentiated Services (DS) field**:

```
 0   1   2   3   4   5   6   7
+---+---+---+---+---+---+---+---+
|         DSCP          |  CU   |
+---+---+---+---+---+---+---+---+
```

The top six bits encode the **Differentiated Services Code Point (DSCP)**,
giving 64 code points. The bottom two bits are Currently Unused (CU) and
have since been reclaimed for ECN (RFC 3168).

RFC 2474 §6 partitions the 64 DSCPs into three pools by the low-order bits:

| Pool | Pattern | Assignment authority | Intended use |
|---|---|---|---|
| 1 | `xxxxx0` (32 points) | IETF standards | Standardised PHBs |
| 2 | `xxxx11` (16 points) | Local / experimental | Site-specific policies |
| 3 | `xxxx01` (16 points) | Experimental, later standards | Initially experimental; IETF may migrate from pool 1 |

Pool 1 is where the Class Selector (CS), EF, and AF code points live.
Pool 3 later hosted the Voice-Admit (RFC 5865) and Lower-Effort (RFC 8622)
additions.

<!-- added:2026 -->
In modern practice (2020s) the DSCP field is often partially trusted across
administrative boundaries: ISPs routinely bleach the DS field at peering
points to prevent customers from stealing priority. Enterprise networks,
data centres, and private WANs are the main venues where DSCP marking has
end-to-end meaning. The [DiffServ client chapter](II-05-diffserv-client.md) takes the per-domain view:
DiffServ4NS simulates a single domain with a well-defined edge, matching
the RFC 2475 abstraction without needing inter-domain trust.
<!-- end added -->

## 3. Defined Per-Hop Behaviours

### Expedited Forwarding (EF)

**RFC 2598 (1999)** defined EF as "a PHB providing low loss, low
latency, low jitter, assured bandwidth, end-to-end service", with the
mental model of a **virtual leased line**. DSCP `101110` (46 decimal) is
the recommended code point.

RFC 2598 specified EF in terms of departure rate: packets marked EF should
depart from any node at a configured rate regardless of other traffic. This
definition proved ambiguous under bursty arrivals and was superseded.

**RFC 3246 (2002)** reformulated EF as a **strict bound on per-hop delay**:
for any time interval, the EF aggregate departs such that the queueing
delay never exceeds a configured bound `F`. The companion RFC 3247 gives
the supporting analysis. RFC 3246 obsoletes RFC 2598 but keeps the DSCP
value 46.

The canonical implementation is a strict-priority queue served ahead of
all other BAs, optionally rate-capped to protect lower-priority traffic
from EF starvation. DiffServ4NS implements this as `dsPQ` (priority queue
with per-queue rate cap) and uses it both standalone (for pure EF) and as
the EF slot of `dsLLQ` (Low Latency Queueing).

### Assured Forwarding (AF)

**RFC 2597 (1999)** defines the AF PHB **group**: four independent classes
(AF1, AF2, AF3, AF4), each with three **drop precedences** (low, medium,
high). The grid of 12 code points is:

| Class | Low drop | Medium drop | High drop |
|---|---|---|---|
| AF1 | AF11 = `001010` (10) | AF12 = `001100` (12) | AF13 = `001110` (14) |
| AF2 | AF21 = `010010` (18) | AF22 = `010100` (20) | AF23 = `010110` (22) |
| AF3 | AF31 = `011010` (26) | AF32 = `011100` (28) | AF33 = `011110` (30) |
| AF4 | AF41 = `100010` (34) | AF42 = `100100` (36) | AF43 = `100110` (38) |

The four classes are forwarded on independent queues (no bandwidth sharing
across classes); within a class, drop precedence controls which packets
are discarded first under congestion. The canonical implementation is a
multi-queue scheduler (one queue per AF class) with **RIO** (RED with In/Out)
on each queue to separate drop precedences.

AF is the mechanism behind the **Olympic service model** (Gold/Silver/Bronze)
introduced in the RFC 2597 examples and widely used in enterprise networks.

### Class Selector (CS)

RFC 2474 §4.2.2 defines eight **Class Selector** code points `xxx000` to
preserve backward compatibility with the IP precedence field of the legacy
TOS byte (RFC 791/1349). CS0–CS7 correspond to IP precedence 0–7. CS0 is
identical to the Default PHB; CS1 is traditionally "scavenger" / lower
priority; CS6 and CS7 are reserved for network-control traffic. The
Class Selector PHBs require only that higher-numbered CS aggregates
receive at least as much forwarding preference as lower-numbered ones.

### Default (Best Effort)

DSCP `000000` (0) is the Default PHB (RFC 2474 §4.1): ordinary best-effort
forwarding with no assurances. Traffic that enters the domain unmarked, or
that is explicitly marked Default, is served here. In the canonical
DiffServ4NS configuration it shares the last queue of the scheduler with
whatever residual capacity EF and AF leave behind.

## 4. Traffic conditioning components

### Meters

A meter measures the arrival pattern of a stream against a **traffic
profile** and emits a colour (green / yellow / red) per packet. DiffServ4NS
implements six meter families:

| Meter | Model | Reference | Colours |
|---|---|---|---|
| Token Bucket | Single bucket (CIR, CBS) | pre-RFC (textbook) | green / red |
| srTCM | Two coupled buckets (CIR, CBS, EBS) | RFC 2697 | green / yellow / red |
| trTCM | Two independent buckets (CIR/PIR, CBS/PBS) | RFC 2698 | green / yellow / red |
| TSW2CM | EWMA rate estimator | RFC 2859 | green / red (probabilistic) |
| TSW3CM | EWMA rate estimator | RFC 2859 | green / yellow / red (probabilistic) |
| FW (fair weighted) | Per-flow byte accounting | DiffServ4NS original | policy-defined |

The key distinction is **token-bucket** (srTCM, trTCM) vs **time-sliding-
window** (TSW2CM, TSW3CM). Token buckets are deterministic given a packet
trace; TSW estimators update an EWMA rate and make probabilistic decisions
when the estimate exceeds CIR. Both families accept both colour-blind and
colour-aware configuration in the RFC; DiffServ4NS v1 implements the
colour-blind variant only (spec I-2.7).

### Markers

The marker writes the DSCP corresponding to the meter's colour. DiffServ4NS
parameterises each policer with `initialCodePt` (green), `downgrade1`
(yellow), and `downgrade2` (red); the marker is therefore policy-agnostic
and can be configured to produce AF1x, AF2x, EF, or any custom combination.

### Droppers and shapers

RFC 2475 lists shapers (delay-based) and droppers (discard-based) as
separate conditioners. DiffServ4NS folds the dropper into the RIO queue
structure: once a packet has been marked, the drop precedence is enforced
by the per-queue RED/RIO thresholds rather than by a standalone dropper
block. Shapers are not implemented in the 2001 release; the FW policer
provides the closest equivalent (byte-counting per-flow penalty).

## 5. RFC chronology after 2001

<!-- added:2026 -->
The DiffServ model as the 2001 thesis described it was already complete in
its major components, but the intervening two decades added clarifications,
operational guidance, and two new PHBs. A 2026 port has to be aware of this
landscape even though the DiffServ4NS code base does not yet implement the
newer code points.

| Year | RFC | Title | Impact on the model |
|---|---|---|---|
| 2002 | 3246 | An Expedited Forwarding PHB | Redefined EF in delay terms; obsoleted RFC 2598; kept DSCP 46 |
| 2002 | 3247 | Supplemental Information for the New Definition of the EF PHB | Analytical backing for RFC 3246 |
| 2002 | 3260 | New Terminology and Clarifications for Diffserv | Formalised "BA", "Ordered Aggregate", "PHB Scheduling Class" vocabulary |
| 2006 | 4594 | Configuration Guidelines for DiffServ Service Classes | The practical cookbook: 12 recommended service classes (Voice, Broadcast Video, Multimedia Conferencing, Real-Time Interactive, Multimedia Streaming, Signalling, Network Control, OAM, High-Throughput Data, Low-Priority Data, Standard, Telephony) mapped to specific DSCPs |
| 2010 | 5865 | A Differentiated Services Code Point (DSCP) for Capacity-Admitted Traffic | Added **Voice-Admit** PHB at DSCP 44 (`101100`) for admission-controlled voice distinct from uncontrolled EF |
| 2019 | 8622 | A Lower-Effort Per-Hop Behavior (LE PHB) | Added **LE PHB** at DSCP 1 (`000001`) for "scavenger" traffic that yields to everything else; replaces CS1 in modern deployments |

Three of these are operationally significant for anyone reading a 2026
DSCP trace:

- **RFC 4594** is the single document most modern DiffServ deployments cite
  for their marking plan. If an enterprise says "we mark VoIP as EF and
  video conferencing as AF41", they are following RFC 4594.
- **RFC 5865 (Voice-Admit)** matters because real VoIP deployments now
  distinguish admitted from non-admitted voice. A simulator that models
  only EF conflates the two.
- **RFC 8622 (LE PHB)** matters because the "scavenger" idea — traffic
  that actively yields — is now a standardised PHB with its own DSCP,
  not a local convention. Background backups, software updates, and
  opportunistic traffic are expected to mark LE.

RFC 3260 is the one most developers accidentally need: it is where the
phrase **Behaviour Aggregate** (BA), ubiquitous in the diffserv
literature, is actually defined. RFC 2475 used the term informally; RFC
3260 gave it a precise scope.
<!-- end added -->

## 6. How DiffServ4NS maps to the model

<!-- added:2026 -->
The 2026 ns-3 port implements a strict subset of the model above, with
RFC-conformance-tested meters and the complete DiffServ4NS scheduler family.
The mapping table below is indexed by §1–§4 of this chapter and cites the
corresponding Intent specs (`specs/01-intent.md`).

### Architecture coverage

| RFC 2475 concept | DiffServ4NS implementation | Spec |
|---|---|---|
| Edge router | `EdgeQueueDisc` (model/stratum-edge-queue-disc) | I-1 |
| Core router | `CoreQueueDisc` (model/stratum-core-queue-disc) | I-4 |
| BA classifier | DSCP read path in core edge disc | I-4 |
| MF classifier | `diffserv::PolicyClassifier` (source, dest, proto, app-type) | I-1.1–I-1.4 |
| Marking | `MarkRule` writes DSCP into IPv4 header | I-1.5 |
| Metering | Meter class hierarchy (see [Meter coverage](#meter-coverage)) | I-2 |
| Shaping | Not implemented (v1) | — |
| Policing | Integrated into meter's applyPolicer step | I-2.8 |

### Meter coverage

All six DiffServ4NS meters are implemented in `model/`:

| Meter | Class | RFC test vectors | Count |
|---|---|---|---|
| Token Bucket | `TokenBucketMeter` | pre-RFC, hand-derived | 5 |
| srTCM | `SrTcmMeter` | RFC 2697 §3–§4 | 10 |
| trTCM | `TrTcmMeter` | RFC 2698 §2 | 10 |
| TSW2CM | `Tsw2cmMeter` | RFC 2859 | — (probabilistic, validated via scenarios) |
| TSW3CM | `Tsw3cmMeter` | RFC 2859 | — (probabilistic, validated via scenarios) |
| FW | `FWMeter` | DiffServ4NS original | scenario-validated |

The 25 deterministic vectors (5 TB + 10 srTCM + 10 trTCM) are catalogued in
`test/rfc-test-vectors.h` and documented in
[Appendix A](appendix-A-rfc-conformance.md). They pass with no tolerance
(deterministic arithmetic, exact match).

### PHB coverage

| PHB | DiffServ4NS mapping | Spec |
|---|---|---|
| EF (RFC 3246) | `PriorityScheduler` (optionally wrapped by `LlqScheduler`) | I-4.4 |
| AF (RFC 2597) | Multi-queue scheduler + RIO per queue | I-4.5 |
| Default (RFC 2474) | Unmarked or DSCP=0 traffic on the default queue | I-4.6 |
| CS0–CS7 | Implicit via DSCP read-through; no dedicated queue | partial |
| Voice-Admit (RFC 5865) | Not implemented | — |
| LE PHB (RFC 8622) | Not implemented | — |

The missing pieces (Voice-Admit, LE) are straightforward to add as
additional mark rules — the DSCP infrastructure already supports the full
0–63 range; only the default topology wiring does not allocate a dedicated
queue for them.

### What the port does NOT claim

To be explicit about scope:

- **No colour-aware mode.** srTCM and trTCM are colour-blind only (I-2.7);
  adding colour-aware is a bounded extension but deferred.
- **No IPv6.** The DS field is read/written on IPv4 only; the ns-3 IPv6
  stack has compatible machinery and the port is architecturally ready
  but untested on it.
- **No inter-domain trust model.** The simulator assumes a single
  administrative domain; no bleaching or re-marking at simulated peering.
- **No ECN interaction.** RFC 3168 ECN is left to the mainline ns-3 RED
  infrastructure; RIO does not currently honour the CE codepoint.

These are listed in `specs/01-intent.md` under "Out of scope for v1".
<!-- end added -->

## See also

- [The DiffServ client](II-05-diffserv-client.md) — maps the model onto the DiffServ4NS object hierarchy.
- [Appendix A — RFC conformance vectors](appendix-A-rfc-conformance.md) — the 25 deterministic meter vectors.
- `specs/01-intent.md` — capability assertions (I-1 through I-7) underlying §6.
- RFC 2474 — DS field (1998).
- RFC 2475 — DiffServ architecture (1998).
- RFC 2597 — Assured Forwarding PHB group (1999).
- RFC 2598 — original EF PHB (1999, obsoleted by RFC 3246).
- RFC 2697 — A Single Rate Three Color Marker (1999).
- RFC 2698 — A Two Rate Three Color Marker (1999).
- RFC 2859 — Time Sliding Window Three Colour Marker (2000).
- RFC 3246 — An Expedited Forwarding PHB (2002).
- RFC 3260 — New Terminology and Clarifications for DiffServ (2002).
- RFC 4594 — Configuration Guidelines for DiffServ Service Classes (2006).
- RFC 5865 — A DSCP for Capacity-Admitted Traffic (2010).
- RFC 8622 — A Lower-Effort Per-Hop Behavior (2019).
