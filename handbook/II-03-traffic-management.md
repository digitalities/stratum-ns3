---
title: Traffic management background
origin: inspired-by-thesis-§1
status: filled
last-updated: 2026-06-06
---

# Traffic management background

Background motivation for differentiated traffic handling in IP networks.
Covers Active Queue Management, the IntServ/DiffServ debate, and the
architectural trajectory that made DiffServ the de-facto QoS default by 2026.

## 1. Why traffic management matters

IP was designed as a best-effort datagram service. Every packet is forwarded
independently, queues are FIFO, and the only overload response built into the
base architecture is tail-drop. That design carried the Internet through its
first two decades because the dominant application was elastic bulk transfer
(FTP, SMTP, early HTTP) and the dominant transport was TCP, whose congestion
control (Van Jacobson 1988) adapts sending rate to observed loss. Best-effort
plus TCP was sufficient while every flow had the same service expectation.

That assumption broke in the early 1990s. Packet voice (RTP, RFC 1889, 1996)
and later packet video introduced flows whose value collapses under even
modest loss, delay, or jitter, and which, being UDP-based, do not react to
congestion signals at all. A voice call and a bulk FTP transfer sharing the
same FIFO queue at a congested link receive the same treatment, but their
service requirements are incompatible: TCP tolerates seconds of queueing
delay, interactive voice does not. TCP's own congestion control, however
well-tuned, cannot provide the flow-level service differentiation that
real-time traffic requires, because it operates end-to-end and has no
mechanism for a router to treat one flow differently from another.

Traffic management is the set of router-side mechanisms — classification,
metering, queueing, scheduling, dropping — that closes this gap. It is the
machinery by which a network operator converts a raw FIFO link into a set
of service classes, each with its own loss, delay, and throughput
guarantees.

## 2. The two architectural camps of the late 1990s

Through the mid- and late 1990s the IETF converged on two quite different
answers to the question of how IP should support service differentiation.

**Integrated Services (IntServ)**, specified in RFC 1633 (1994) with
signalling in RFC 2205 (RSVP, 1997), takes a telephony-network stance.
Every flow that wants a non-best-effort service requests it explicitly
through RSVP; the signalling traverses every router on the path; each
router installs per-flow state describing the reservation; and admission
control at each hop guarantees that the sum of reservations does not
exceed link capacity. The two service classes defined for IntServ —
Guaranteed Service (RFC 2212) and Controlled Load (RFC 2211) — provide
per-flow quantitative and qualitative guarantees respectively. IntServ
is powerful: it can deliver the kind of hard end-to-end bounds that
telephony operators were accustomed to.

IntServ's Achilles heel is scalability. Per-flow state in core routers
grows with the number of concurrent flows, which in a backbone is
O(10^5) to O(10^6). RSVP signalling must be refreshed periodically per
flow. Admission decisions must be made per reservation. None of this
fits the forwarding budget of a carrier-class core router, whose
hardware is optimised for stateless longest-prefix-match lookups at
wire speed. By 1998 it was widely accepted within the IETF that IntServ
could not be deployed end-to-end across the public Internet; its use
was relegated to enterprise edges and controlled domains.

**Differentiated Services (DiffServ)**, specified in RFC 2474 and the
architectural framework RFC 2475 (both 1998), takes the opposite stance.
Packets carry a six-bit Differentiated Services Code Point (DSCP) in the
IPv4 header that encodes a Per-Hop Behaviour (PHB). A small, fixed number
of PHBs are defined: Expedited Forwarding (EF, RFC 2598, later RFC 3246)
for low-loss low-latency traffic; Assured Forwarding (AF, RFC 2597) with
four classes and three drop-precedence levels each; and the Default PHB
for best-effort. Edge routers classify flows into PHBs and mark the DSCP;
core routers forward purely on DSCP, with no per-flow state, no signalling,
and no admission control. Service differentiation is delivered as
aggregate behaviour: the AF3 aggregate receives better loss than the AF4
aggregate, but no individual AF3 flow is guaranteed anything specific.

DiffServ won the core-router space because it aligned with how core
routers were actually built. Stateless DSCP-based queueing maps directly
onto hardware packet classification tables; PHBs map onto the fixed
number of priority queues a line card provides; the per-hop model
decouples operator domains, so each AS can deploy DiffServ
independently. By the time the IETF published RFC 3246 in 2002, DiffServ
had become the assumed baseline for IP QoS in production networks.

## 3. Active Queue Management — the other half of the picture

Classification and scheduling decide *which* queue a packet enters and
*when* a queue gets serviced. Active Queue Management (AQM) decides
*which* packets to drop, and when, in order to signal congestion to
responsive transports before the buffer overflows.

The seminal AQM algorithm is Random Early Detection (Floyd & Jacobson
1993). RED maintains an exponentially-weighted average queue length and
drops arriving packets with a probability that ramps linearly from zero
at a minimum threshold to a maximum drop probability at a maximum
threshold. The effect is to signal incipient congestion to TCP senders
before tail-drop sets in, spreading the back-off across flows and
damping the global synchronisation that tail-drop induces.

RED is a *single*-class algorithm. Weighted RED (WRED) extends it to
multiple drop-precedence levels by maintaining per-precedence (min,
max, p_max) triples against a shared average queue length, so
higher-precedence packets are dropped less aggressively than
lower-precedence ones from the same physical queue. WRED is the AQM
algorithm DiffServ4NS uses: the `dsred` queue in the 2001 module
implements RIO (RED with In/Out), the direct ancestor of WRED, with up
to three virtual drop-precedence queues per physical queue.

<!-- added:2026 -->
Two AQM families emerged after the module's 2001 baseline that DiffServ4NS does not implement: CoDel/FQ-CoDel and PIE.
Operational experience with RED showed that its effectiveness depends
heavily on parameter tuning (min/max thresholds, weight, p_max), and
that good parameters at one load are poor at another. Two replacements
emerged. CoDel (Controlled Delay; Nichols & Jacobson 2012,
standardised as RFC 8289 in 2018) abandons queue-length tracking and
instead measures sojourn time — the interval a packet spends in the
queue — dropping packets only when sojourn exceeds a target (typically
5 ms) for longer than an interval (typically 100 ms). FQ-CoDel (RFC
8290, 2018) couples CoDel with per-flow stochastic fair queueing,
providing both bufferbloat control and flow isolation without any
per-flow configuration. PIE (Proportional Integral controller Enhanced;
RFC 8033, 2017) reaches similar goals via a classical control-theoretic
feedback loop on queueing delay, and is widely deployed in DOCSIS cable
modems.
<!-- end added -->

<!-- added:2026 -->
DiffServ4NS predates these developments by fifteen-plus years. It uses
RED/RIO/WRED, which was state-of-the-art when the module was written in
2001 and remained the default AQM in carrier-class routers well into
the 2010s. A modern reconstruction could, in principle, substitute
CoDel or PIE for RED under each RIO virtual queue — ns-3 mainline ships
CoDel and FQ-CoDel queue discs — but the port deliberately does not:
the point of the reconstruction is to reproduce the 2001 module's
behaviour, not to modernise it. AQM modernisation is a separate axis of
future work.
<!-- end added -->

## 4. Where DiffServ4NS fits

DiffServ4NS is a simulation platform for DiffServ behaviour modelling,
written by Sergio Andreozzi in 2001 against the then-current
ns-2.1b8 / ns-2.29 releases and the IETF DiffServ RFCs of 1997–2001. The
module implements the full DiffServ edge/core split: edge routers
classify on source, destination, transport protocol, and application
type (see `specs/01-intent.md`), assign a DSCP, and meter flows
through one of six metering algorithms (srTCM RFC 2697, trTCM RFC 2698,
TSW2CM/TSW3CM RFC 2859, Token Bucket, and the FW per-flow policer).
Core routers forward on DSCP alone, feeding nine configurable per-hop
queues through one of nine schedulers (RR, WRR, WIRR, PQ, WFQ, WF2Q+,
SCFQ, SFQ, LLQ), each queue protected by RIO drop precedence.

The module is an artifact of a specific design era. It was written at
the moment the DiffServ architecture had crystallised but before the
operational community had accumulated the decade of field experience
that would refine it. It took the RFCs at face value, implemented them
faithfully, and instrumented the result with per-DSCP monitoring
(`specs/01-intent.md`) rich enough to answer the kinds of
research questions that motivated the thesis: how do the different
schedulers behave under mixed-class load; how does PHB-level
differentiation translate into flow-level quality; how do the metering
algorithms of RFC 2697/2698/2859 compare under bursty traffic. These
are the questions the 2001 validation scenarios are built to answer,
and they remain the questions this handbook's three-way
results revisit twenty-five years later.

## 5. Historical note — the 2001 to 2026 shift

<!-- added:2026 -->
A quarter century of production deployment has settled the IntServ/DiffServ
debate that shaped the module's design context. IntServ did not
disappear; it fossilised into niches. RSVP survives in MPLS traffic
engineering (RSVP-TE, RFC 3209) where the per-flow state is per-LSP
rather than per-microflow and therefore tractable, and in a few
controlled-network guaranteed-service deployments (some professional
audio-over-IP systems, some military tactical networks). But on the
public Internet, IntServ-style end-to-end reservations never
materialised.
<!-- end added -->

<!-- added:2026 -->
DiffServ, by contrast, became the implicit QoS default. Carrier
Ethernet and MPLS networks map DSCP to EXP/TC bits and run DiffServ-style
aggregate behaviours inside the transport. LTE and 5G bearer QoS
(QCI/5QI classes) is DiffServ in all but name: a small fixed number of
service classes, stateless per-packet treatment at intermediate nodes,
edge-based admission and policing. Data-centre fabrics use DSCP-marked
priority classes to separate RDMA, storage, and general traffic. Most
operator peering and transit contracts assume and depend on DSCP
preservation at interconnects. The design decisions DiffServ4NS
embodies — aggregate-class PHBs, edge marking, stateless core, a small
catalogue of scheduler primitives — are the decisions the industry
standardised on.
<!-- end added -->

<!-- added:2026 -->
What has changed, and what DiffServ4NS does not reflect, is the AQM
layer beneath the scheduler. The bufferbloat discourse of the early
2010s (Gettys & Nichols 2011) identified oversized tail-drop buffers
in home routers and cable modems as a cause of severe interactive-flow
latency, and catalysed the shift from RED to CoDel/PIE and from flat
queues to FQ-CoDel in edge hardware. Modern cloud and data-centre
workloads — microservice RPC, container orchestration heartbeats,
storage replication — are also driving renewed interest in
low-latency AQM, but the shift has been toward sojourn-time-based
algorithms rather than back to WRED. DiffServ4NS's use of RIO/WRED is
historically faithful; it is also, by 2026 standards, dated. The
module is therefore best understood as a snapshot of how IP QoS was
conceived in 2001: the moment DiffServ had won the architectural
argument but before the AQM layer caught up with what traffic was
actually going to look like on the other side of the broadband
revolution.
<!-- end added -->

## See also

- `specs/01-intent.md` — what DiffServ4NS shall do; the scoped
  intent of the 2001 module against which the 2026 ports are validated.
- [The DiffServ model](II-04-diffserv-model.md) — edge/core
  architecture, DSCP, PHB groups, the RFC families in detail.
- [Three-way comparative results](III-02-three-way-validation.md) —
  how the 2001 ns-2.29 module, the 2026 ns-2.35 port, and the 2026
  ns-3 port compare on the thesis validation scenarios.

### Relevant RFCs

- RFC 1633 (1994) — Integrated Services architecture
- RFC 2205 (1997) — RSVP signalling
- RFC 2474 (1998) — DSCP definition
- RFC 2475 (1998) — DiffServ architecture
- RFC 2597 (1999) — Assured Forwarding PHB group
- RFC 2598 (1999) / RFC 3246 (2002) — Expedited Forwarding PHB
- RFC 2697 (1999) — Single Rate Three Colour Marker
- RFC 2698 (1999) — Two Rate Three Colour Marker
- RFC 2859 (2000) — Time Sliding Window Three Colour Marker
- RFC 8033 (2017) — PIE AQM
- RFC 8289 (2018) — CoDel AQM
- RFC 8290 (2018) — FQ-CoDel
