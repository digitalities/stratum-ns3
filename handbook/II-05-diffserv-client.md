---
title: The DiffServ client
origin: inspired-by-thesis-§3, §3.3.3
status: filled
last-updated: 2026-06-07
---

# The DiffServ client

> **Hands-on**: see [DiffServ recipes](I-03-diffserv.md) for runnable recipes that exercise the meter, scheduler, and queueing architecture described here.

## From three axes to four slots

The DiffServ client is the substrate's canonical client and its
historical core. It absorbs the 2001 DiffServ4NS module — the IETF
Differentiated Services architecture (RFC 2475) built on top of the Nortel
Networks ns-2 diffserv module — as the substrate's first-class inhabitant.
The vocabulary here is shared across the ns-2.29 original, the ns-2.35 port,
and the substrate's DiffServ client.

The 2001 module factorises into three orthogonal axes. Each maps onto one
of the [four strategy slots](II-02-stratum-architecture.md) the substrate
exposes to every client:

- **Meters** (policy-side, per-flow) → fill the **Classify-and-Meter** slot.
  RFC 2697 srTCM, RFC 2698 trTCM, RFC 2859 TSW2CM/TSW3CM, token-bucket,
  FW policer, and the Dumb pass-through are the strategy choices available.
- **Policy entries and PHB table** → fill the **Mark-and-Route** slot.
  The edge classifier matches a packet against the policy table and writes
  the resulting DSCP tag; the PHB table routes that DSCP to an inner slot.
- **Per-DSCP DS-RED queues and drop managers** → form the **per-class slot
  array**. Up to eight inner queue discs, each a multi-precedence RED
  container.
- **Schedulers** → fill the **across-slot service policy** slot.
  PQ, RR, WRR, WIRR, WFQ, WF2Q+, SCFQ, SFQ, and LLQ are available;
  each is a strategy object owned by the outer queue disc. (The
  hybrid-LLQ across-tin dispatcher belongs to the CAKE client, not this
  scheduler family.)

The sections below describe each family in depth. For how this client is
built in ns-3 (class hierarchy, helper API, and attribute table), see the
[ns-3 module chapter](II-08-ns3-module.md). For the substrate's four-slot
vocabulary shared across all three clients, see the [Stratum architecture
chapter](II-02-stratum-architecture.md).

## 1. Edge/core split

The DiffServ client mirrors RFC 2475's two-tier model. Packets entering the DS domain
traverse a **DiffServ edge** node that conditions their traffic (classification,
metering, marking, policing, queueing). Packets inside the domain traverse
**DiffServ core** nodes that forward according to the DSCP but do no further
classification. All shaping and remarking decisions are made once, at the
edge.

```
   +------------------------------------------------------------+
   |                       DiffServEdge                         |
   |                                                            |
   |   classify  -->  meter  -->  mark  -->  police  -->  enq   |
   |   (policy     (TB/srTCM    (DSCP     (downgrade     (DS-RED
   |    entry)      trTCM/TSW    write)    on red)        queue)
   |                /FW/Dumb)                                    |
   +-----------------------------|------------------------------+
                                 |
                                 v
   +------------------------------------------------------------+
   |                       DiffServCore                         |
   |                                                            |
   |   DSCP-lookup  -->  enqueue to aggregate  -->  schedule    |
   |   (no reclass)      (DS-RED + WRED)            (PQ/WFQ/…)  |
   +------------------------------------------------------------+
```

The edge performs the full pipeline; the core is a subset of the edge that
skips classification, meters, and marking, and reuses the edge's queueing and
scheduling components unchanged. In all three source variants
(the ns-2.29 original, the ns-2.35 port, and the DiffServ client's `model/`) the core
class inherits from the same DS-RED base as the edge and overrides only the
enqueue front-end.

## 2. The three orthogonal axes

The lead mapped each axis to a substrate slot; the subsections below give
the full family listings and algorithms.

The architecture factorises into three orthogonal subsystems. Each subsystem
is independently pluggable — any meter can combine with any scheduler and any
queue discipline. This orthogonality is what lets the module cover every
Scenario 1/2/3 combination of the thesis with a single codebase.

### Meters (policy-side, per-flow)

Meters classify packets as in-profile or out-of-profile relative to a
per-flow token-bucket state (or equivalent rate estimator). Seven meter
classes cover every IETF-standardised marker and one legacy per-flow
policer inherited from the Nortel base:

| ns-2 class | ns-3 class | Acronym | Standard | Behaviour |
|---|---|---|---|---|
| `DumbPolicy` | `DumbMeter` | Dumb | — | Pass-through; DSCP unchanged |
| `TBPolicy` | `TokenBucketMeter` | TB | — | Single-rate two-colour token bucket |
| `SRTCMPolicy` | `SrTcmMeter` | srTCM | RFC 2697 | Single-rate three-colour (CIR, CBS, EBS) |
| `TRTCMPolicy` | `TrTcmMeter` | trTCM | RFC 2698 | Two-rate three-colour (CIR, PIR, CBS, PBS) |
| `TSW2CMPolicy` | `Tsw2cmMeter` | TSW2CM | RFC 2859 | Time-sliding-window two-colour (EWMA) |
| `TSW3CMPolicy` | `Tsw3cmMeter` | TSW3CM | RFC 2859 | Time-sliding-window three-colour |
| `FWPolicy` | `FWMeter` | FW | — | FirstWinner: per-flow byte-threshold policer |

Each meter maintains a `policyTableEntry` holding the bucket levels
(`cBucket`, `eBucket`, `pBucket`), the rate estimator state (`avgRate`,
`winLen`), and the last update timestamp. The meter is invoked on every
packet via `applyMeter()` (advance state) + `applyPolicer()` (decide
colour and target DSCP). The ns-3 port splits the same state into a typed
`PolicyEntry` struct and a `Meter` subclass hierarchy (see §6 of
`docs/PORTING_MAP.md`); the algorithmic core is line-for-line identical to
the 2001 reference. See `specs/01-intent.md` and `specs/02-structural.md`
for the testable contracts.

### Schedulers (aggregate-side, between queues)

Schedulers decide, at each dequeue opportunity, which of up to
`MAX_QUEUES = 9` physical queues is served next. They operate on the
aggregates already classified by DSCP — they do not look at per-flow state.
Nine classes:

| Class | Acronym | Family | Per-queue parameter |
|---|---|---|---|
| `dsRR` | RR | Round-robin | — |
| `dsWRR` | WRR | Round-robin | integer weight |
| `dsWIRR` | WIRR | Round-robin | integer weight, interleaved |
| `dsPQ` | PQ | Priority | priority index + optional rate cap |
| `dsWFQ` | WFQ | GPS-approx | weight; uses GPS reference clock |
| `dsWF2Qp` | WF2Q+ | GPS-approx | weight; eligibility constraint |
| `dsSCFQ` | SCFQ | GPS-approx | weight; self-clocked virtual time |
| `dsSFQ` | SFQ | GPS-approx | weight; start-time tags |
| `dsLLQ` | LLQ | Composite | PQ + one fair-queueing scheduler |

All schedulers share the same dispatch contract: `enqueEvent(queueIndex,
packet)` on arrival, `dequeEvent() -> queueIndex` on departure. The ns-3
port formalises this as the `Scheduler` strategy-object interface
(see §5 below). See `specs/01-intent.md` and `specs/02-structural.md` for the testable contracts.

### Queues and drop managers (per-DSCP aggregate)

Each physical queue is a DS-RED sub-queue partitioned into up to
`MAX_PREC = 3` virtual queues, one per drop precedence. All three virtual
queues share a single physical buffer but compute independent RED drop
probabilities using distinct `(minTh, maxTh, maxP)` parameter triples. This
is the RIO (RED with In/Out) construction of Clark and Fang, extended to
three precedences to match RFC 2597 AF.

| Class | Role |
|---|---|
| `dsREDQueue` | Multi-queue RED container (up to 9 queues × 3 precedences) |
| `dsREDSubQueue` | Per-queue RED logic with virtual queues for DP0/DP1/DP2 |

Constants: `MAX_QUEUES = 9`, `MAX_PREC = 3`, `MAX_CP = 64` (code points),
`MEAN_PKT_SIZE = 1000` (RED EWMA default). See `dsconsts.h` in the 2001
source and `model/stratum-constants.h` in the ns-3 port.

## 3. Policy entries and rule matching

<!-- added:2026 -->
Each edge node holds a **policy table** — an ordered list of `MarkRule`
entries. When a packet enters the edge, the classifier scans the list and
picks the first rule whose predicates all match. Match fields are:

| Field | Semantics |
|---|---|
| `srcIP`, `dstIP` | IP address predicates (or `ANY_HOST`) |
| `srcPort`, `dstPort` | L4 port predicates (added in the 2026 port) |
| `srcNode`, `dstNode` | Node-address predicates (ns-2 legacy) |
| `appType` | Application-type tag (FTP, HTTP, CBR, RealAudio, …) |
| `protocol` | TCP/UDP discriminator |

The matching rule is first match wins, and wildcards use the sentinel
value `ANY_HOST` (or the port sentinel `PORT_ANY`). The 2026 ports
clarify two points that the 2001 source left implicit:

1. Rule order matters — later entries cannot override earlier matches.
2. Wildcards match any value, but an explicit rule with a concrete value
   still takes precedence when it is listed first.

A packet with no matching rule retains its original DSCP. When a
rule matches, the classifier dispatches to the meter/policer pair named in
the corresponding `policyTableEntry` / `policerTableEntry`, and the meter's
colour decision selects one of three target DSCPs (`initialCodePt`,
`downgrade1`, `downgrade2`). That target DSCP is written to the IPv4 DS
field before the packet reaches the queue.
<!-- /added:2026 -->

## 4. Drop precedence in the AF aggregate

RFC 2597 defines four AF classes (AF1x..AF4x), each with three drop
precedence levels (DP0 = low, DP1 = medium, DP2 = high). DiffServ4NS models
each AF class as one physical queue with three WRED parameter triples,
indexed by drop precedence:

```
   physical queue i  (one AF class, e.g. AF1)
     |
     +--- virtual queue 0  (DP0 = AF11)  -->  minTh0, maxTh0, maxP0
     +--- virtual queue 1  (DP1 = AF12)  -->  minTh1, maxTh1, maxP1
     +--- virtual queue 2  (DP2 = AF13)  -->  minTh2, maxTh2, maxP2
     \
      shared physical buffer (tail-drop when full)
```

A critical architectural point: marking and dropping are separate
stages. The meter output DSCP decides which drop precedence the packet
carries into the queue; the WRED thresholds on that drop precedence decide
whether the packet survives. A packet marked yellow by the meter is *not*
dropped at the edge: it is admitted with a higher drop precedence, and
drops only if congestion at a core queue's WRED instance fires. This
decoupling is what makes the AF PHB express "reduced assurance" rather than
"hard admission control". See `specs/02-structural.md` for the testable
assertions.

## 5. The fair-queueing family

<!-- added:2026 -->
DiffServ4NS exposes four schedulers from the Generalised-Processor-Sharing
(GPS) approximation family. The scheduler is a pluggable component owned by
the DS-RED queue disc and installed by a single call, in keeping with the
ns-3 classful `QueueDisc` idiom; the queue disc holds no scheduling state
of its own.

The family forms a spectrum from stateless to virtual-time:

| Scheduler | Virtual time | Reads real time | Tag basis | Complexity |
|---|---|---|---|---|
| SFQ | No | No | Start tag per packet | Stateless, tag-only |
| SCFQ | Implicit | No | Finish tag of in-service packet | Self-clocked |
| WF2Q+ | Yes | Yes (dequeue) | Per-flow S/F + system V | Eligible-set semantics |
| WFQ | Yes | Yes (enqueue) | GPS reference clock + events | Full GPS approximation |

The `Scheduler` base-class interface accommodates this spectrum
through two optional hooks: `OnEnqueueWithTime(queueIndex,
packetSizeBytes, nowSeconds)` lets WFQ observe enqueue time without
breaking the simpler stateless schedulers (default delegation), and
`SetLinkBandwidth(double)` exposes the link rate required by every
fair-queueing scheduler's finish-time computation. The end result is that
SCFQ and SFQ remain pure-unit-testable (no `Simulator` dependency), while
WFQ and WF2Q+ opt into event-driven validation.

LLQ (next section) is the composite consumer of this family.
<!-- /added:2026 -->

## 6. LLQ composition

Low-Latency Queueing is a composite scheduler: a strict-priority queue
(`dsPQ` instance) for the low-latency aggregate sits above a fair-queueing
scheduler (typically `dsSFQ` or `dsWFQ`) that serves the remaining queues.
The PQ half enforces the latency bound; the fair-queueing half shares the
residual capacity fairly among the non-priority aggregates.

DiffServ4NS composes this at the edge-queue-disc level rather than as a
monolithic class — `dsLLQ` holds a `dsPQ*` and a `dsScheduler*` and
delegates. This keeps every fair-queueing scheduler reusable both
standalone and as the LLQ sub-component. Thesis §3.3.3 documents LLQ as the
default Scenario 3 scheduler for the Premium PHB aggregate.

## 7. Monitoring infrastructure

DiffServ4NS defines six canonical per-DSCP-aggregate trace outputs, each
emitted at simulation tick granularity:

| Trace file | Quantity | Aggregation |
|---|---|---|
| `ServiceRate.tr` | Departure rate (bytes/s) | per queue, per (queue, DP) |
| `QueueLen.tr` | Queue length (packets, bytes) | per queue, per (queue, DP) |
| `OWD.tr` | One-way delay | per DSCP, UDP flows |
| `IPDV.tr` | IP delay variation | per DSCP, UDP flows |
| `ClassRate.tr` | Received/transmitted rate | per DSCP |
| `PktLoss.tr` | Drops (RIO vs buffer) | per DSCP |

All six traces are line-oriented ASCII with `<timestamp> <value>` records.
The tooling pipeline (`scripts/parse-traces.py`) ingests these files into
pandas DataFrames and produces the CSVs that drive the plot scripts.
Goodput for TCP flows and frequency distributions for RTT/OWD/IPDV are
derived from the same traces after the run; see `specs/01-intent.md` for the full intent list.

## 8. Cross-simulator architectural mapping

<!-- added:2026 -->
The two ports that coexist in the release repo diverge in one structural
dimension that is worth surfacing here. The ns-2 port (in the DiffServ4NS
heritage repository) exposes a single-level "service-model" API: a packet
enters `dsREDQueue::enque()`, which runs the mark rules, the policer, and
the RED admission test in one monolithic call, then hands the marked
packet to the scheduler. The ns-3 port decomposes this across the native
`TrafficControlLayer` + `QueueDisc` boundary:

| ns-2 concept | ns-3 concept |
|---|---|
| `dsREDQueue::enque()` | `stratum::RedQueueDisc::DoEnqueue()` + classifier |
| `dsREDQueue::deque()` | `stratum::RedQueueDisc::DoDequeue()` + `Scheduler` strategy |
| `dsEdge::classify()` | `diffserv::PolicyClassifier::ApplyPolicy()` |
| Tcl `addPolicyEntry` | `diffserv::Helper::AddPolicy(...)` typed method |
| `dsScheduler*` on the queue | `Ptr<Scheduler>` strategy object owned by the queue disc |

This decomposition is what makes the ns-3 port testable at fine
granularity: each concern lives in its own class, each with its own
isolated test suite. The price is a deeper call stack; the gain is a
cleaner feature surface that tracks ns-3 mainline conventions. The [ns-3 module chapter](II-08-ns3-module.md) covers the ns-3 port in detail.
<!-- /added:2026 -->

## See also

- `specs/01-intent.md` — capability assertions per subsystem.
- `specs/02-structural.md` — testable per-component assertions.
- `docs/INVENTORY.md` — file-level analysis of the 2001 source.
- `docs/PORTING_MAP.md` — class-by-class ns-2 → ns-3 mapping.
- `model/stratum-scheduler.h` — base interface and strategy contract.
