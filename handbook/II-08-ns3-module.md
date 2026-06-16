---
title: The 2026 ns-3 port
origin: 2026-written
status: paper-MVP
last-updated: 2026-06-06
---

# The 2026 ns-3 port

A 2026 port of DiffServ4NS to ns-3 mainline, built from scratch using
Evaluation-Driven Development (EDD) against a three-tier spec suite
(`specs/01-intent.md`, `specs/02-structural.md`, `specs/03-quality.md`).
The port ships as Stratum, the QoS substrate whose composer and three
clients this part documents; this chapter covers how that module is
laid out, built, and configured. Source at the repository root.
Validated with 17 test suites covering RFC conformance vectors,
structural assertions, and end-to-end scenario reproductions.

## Module composition

```
stratum-ns3/
├── CMakeLists.txt        ← ns-3 build_lib() declaration
├── model/                ← meters + schedulers + queue discs + L4S + CAKE + registries
│   ├── stratum-meter (base), stratum-{dumb,token-bucket,sr-tcm,tr-tcm,
│   │   tsw2cm,tsw3cm,fw}-meter
│   ├── stratum-scheduler (base),
│   │   stratum-{rr,wrr,wirr,pq,wfq,wf2qp,scfq,sfq,llq}-scheduler
│   ├── stratum-red-{queue-disc,sub-queue}, stratum-{slot,hybrid-llq}-dispatcher
│   ├── stratum-{edge,core}-queue-disc
│   ├── stratum-{policy,per-flow-policy}-classifier
│   ├── stratum-{mark-rule,policy-entry,phb-table}
│   ├── stratum-{dscp,app-type,send-time}-tag, stratum-statistics
│   ├── stratum-{onoff,trace-replay}-application
│   ├── stratum-l4s-{queue-disc,coupled-scheduler,timestamp-tag}  ← L4S client
│   ├── stratum-cake-{linux-autorate-hook,live-bulk-counter}      ← CAKE client
│   ├── stratum-rate-based-{global-clock,tin-clock,shaper-dispatcher},
│   │   stratum-{tin-shaper,shaped-tin}-dispatcher, stratum-tin-token-bucket
│   ├── stratum-{aqm,scheduler}-registry, stratum-registry.h
│   └── stratum-{edge-meter,queue-stats}-provider,
│       stratum-empirical-cdf-loader, stratum-constants.h
├── helper/
│   ├── stratum-helper        ← stratum::Helper (edge/core config, policy, auto-L2)
│   ├── stratum-cake-helper   ← cake::Helper (tin presets, RTT presets, host isolation)
│   ├── stratum-cake-stats-formatter
│   ├── stratum-monitor-helper, stratum-onoff-helper
│   └── stratum-flent-csv-sink
├── test/
│   ├── diffserv-test-suite.cc        ← primary suite (DiffServ)
│   ├── diffserv-cake-q15-test-suite.cc
│   ├── l4s-scenario-validation-test.cc, l4s-routing-test.cc
│   ├── diffserv-q16-chang-convergence-test.cc
│   ├── diffserv-q17-parekh-theorem1-test.cc
│   ├── diffserv-wf2qp-regression-test.cc
│   ├── per-flow-classifier-test.cc
│   ├── empirical-cdf-loader-test.cc
│   └── rfc-test-vectors-runner.cc + rfc-test-vectors.h
│       (5 TB + 10 srTCM + 10 trTCM deterministic RFC vectors)
├── examples/
│   ├── diffserv-example-1.cc        ← thesis Scenario 1
│   ├── diffserv-example-2.cc        ← thesis Scenario 2 (quick/full scale)
│   ├── diffserv-example-3.cc        ← thesis Scenario 3 (quick/full scale)
│   └── chang-comparison.cc          ← Chang et al. WRR ratio reproduction
├── doc/stratum.rst
├── CHANGELOG.md
└── MIGRATION-from-ns2.md
```

## EDD methodology

Every component was specified before being implemented:

1. **I-tier (intent)** — `specs/01-intent.md` declares what the module *shall do*,
   derived from thesis §3.3.3 and the `dsCore.h` author header.
2. **S-tier (structural)** — `specs/02-structural.md` derives testable per-component
   assertions, each linked to one or more intent specs.
3. **Q-tier (quality)** — `specs/03-quality.md` defines end-to-end scenario
   acceptance gates.

Each test case carries a descriptive class name (e.g.
`SrTcmIdleAccumulationTest`) and a Doxygen `@see` line that points to its
structural spec. Tests are written before implementation; implementation targets the
spec, not the reference code.

## Validation summary

- **RFC conformance**: 25 srTCM + trTCM + TSW vectors pass exactly (no tolerance).
- **Test gate**: 17 suites, all pass at EXTENSIVE fullness.
- **Scenario reproduction**: 5 schedulers × 7 packet sizes within 6 % rate
  agreement vs the ns-2.35 cross-port.
- **TF-TANT real-network heritage**: PQ 512B EF mean OWD ~17 ms (real) vs
  ~12.4 ms (ns-2.35) vs 14.0 ms (ns-3); ns-3 closes about a third of the
  simulation-to-hardware gap.
- **Full-scale Scenario 2 (n=469, simTime=5000 s, numHttp=400)**: 29/36 cells
  within tolerance (caPL ≤ 2 pp, boPL ≤ 0.5 pp), mean cross-sim |ΔcaPL| 0.70 pp.
- **Full-scale Scenario 3**: Premium/Gold/Silver/Bronze/BE within 2 % of ns-2.35
  (commit `0c660ec`).

## Wire-byte vs IP-byte accounting at the FQ scheduler

A subtle byte-basis question becomes load-bearing whenever a fair-queueing
scheduler is asked to allocate exactly its nominal share to a constrained
flow on a link with non-zero L2 framing.

### The question

A scheduler computes its rate / deficit / token-bucket math against a
per-packet byte count. Two interpretations:

- **IP bytes:** `item->GetSize()` — IP header + payload + transport header.
  This is the only count the queue-disc layer sees natively.
- **Wire bytes:** IP bytes plus whatever L2 framing the netdev's
  `AddHeader()` step prepends per packet. PointToPoint adds 2 bytes
  (PPP); Csma adds 14 bytes (Ethernet); SimpleLink adds 0; Wifi/LTE
  varies per packet and can't be summarised as a single scalar.

The L2 framing is invisible at the queue-disc layer because it's added
*downstream* of the queue-disc, inside the netdev's `Send()`:

```
TC layer (queue-disc, scheduler, meter — all see IP bytes)
   ↓
NetDevice::Send → AddHeader (L2 framing happens here, after dequeue)
   ↓
Channel serialises wire bytes onto the link
```

### Why it matters under marginal load

The link wire-byte rate is fixed by the netdev's `DataRate` (e.g. 2 Mbps
= 250000 wire B/s). The effective IP-layer rate available is
`wire_rate × pkt_IP / (pkt_IP + L2)` — slightly less. For a 540-byte EF
packet on a PPP link, that's `2 Mbps × 540/542 ≈ 1.99 Mbps`.

If the scheduler reasons in IP bytes, it allocates a queue's share as
`weight × LinkBandwidth` against the *nominal* 2 Mbps and over-promises
by ~0.4 %. At unsaturated load this is invisible — the over-promise is
absorbed by the slack between offered and link rate. But at marginal
load, where the constrained flow is offered exactly its nominal share,
0.4 % is exactly the difference between an empty queue and a saturated
one. The queue grows monotonically until it hits the queue-limit and
drops every excess packet; cumulative-mean OWD trajectories rise into
hundreds of milliseconds even though the steady-state queue depth
should be one or two packets.

This is exactly the pathology the Scenario 1 cross-simulator validation exposed: ns-3 SCFQ/SFQ/WF2Q+ EF queues saturated at 30
packets / OWD 245 ms cumulative, while ns-2.35 stayed at mean 1.5
packets / OWD 33 ms. Same algorithms, same offered load, drastically
different outcomes — because ns-2.35's `SimpleLink` adds zero L2
framing and ns-3's `PointToPointNetDevice` adds 2 bytes. The
algorithmic implementations were faithful; the byte basis was the
hidden mismatch.

### How real router schedulers handle it

Linux solved it in 2009 via `tc-stab(8)` — the `stab` (size-table)
attribute on each qdisc that adds per-packet L2 overhead before
charging tokens or quantum. Cisco MQC's `bandwidth` policy is L2 line
rate by default (you have to opt in to L3 reasoning via
`bandwidth percent`). BSD ALTQ and Juniper class-of-service follow the
same pattern. The convention in production routers is that schedulers
charge wire bytes, not IP bytes. RFCs (2474, 2475, 2597, 2598/3246,
2697, 2698, 2859, 7141) are deliberately layer-agnostic, leaving the
byte basis to implementations; every real implementation lands on
wire bytes.

### How Stratum handles it

The FQ schedulers (`ScfqScheduler`, `SfqScheduler`, `WfqScheduler`,
`Wf2qPlusScheduler`, `LlqScheduler`) and the byte-consuming meters
(`TokenBucketMeter`, `SrTcmMeter`, `TrTcmMeter`, `Tsw2cmMeter`,
`Tsw3cmMeter`) carry an `L2OverheadBytes` attribute (default `0`).
Subclasses add it to the IP byte count before computing finish-time
increments / charging tokens, so meter and scheduler reason in the
same wire-byte basis the link physically consumes. The strict-priority
scheduler does *not* consume the attribute — it doesn't compute
against `LinkBandwidth` and is unaffected by the deficit.

The `diffserv::Helper::DetectL2OverheadBytes(Ptr<NetDevice>)` static
method auto-detects from the bottleneck netdev (`PointToPointNetDevice`
→ 2, `CsmaNetDevice` → 14, others → 0), so the typical user does not
have to set the attribute manually. `examples/diffserv-example-1.cc`
defaults to auto-detect and exposes `--l2OverheadBytes` as an explicit
override for what-if scenarios (LLC/SNAP/VLAN/FCS) or for forcing
pure IP-byte accounting (`=0`).

ns-2.35's `SimpleLink` has zero L2 framing, so its scheduler
implementation reasons in IP bytes and reaches the right answer
trivially because IP bytes ≡ wire bytes there. Each port matches
its host simulator's link reality.

### This is not unique to Stratum

The same blind spot exists in ns-3 mainline `FqCoDelQueueDisc`,
`FqCobaltQueueDisc`, `FqPieQueueDisc`, `TbfQueueDisc`, `RedQueueDisc`,
and `PieQueueDisc` — all charge `item->GetSize()` IP bytes and silently
lose the L2 framing percentage to the underlying netdev. The drift is
typically masked because most ns-3 FQ studies over-offer load by
50–200 % (saturation drops drown out a 0.4 % allocation drift) or use
Wifi/LTE channels whose other per-packet costs (preambles, MAC, ACK,
contention) swamp the issue. Scenario 1's marginal-load FQ stress test
forced the question.

The cleanest upstream resolution is a virtual
`NetDevice::GetL2OverheadBytes()` accessor with overrides on each
fixed-framing netdev plus generic `QueueDisc::ChargeBytes(item)`
consumption — a small structural addition mirroring `tc-stab`, drafted
for future contribution filed upstream against ns-3-dev.

The rationale, design alternatives, and consequences are documented
in the project's design records.

## Ns-3 mainline patches carried locally

Several ns-3 mainline gaps are patched locally.
The patch stack lives under `patches/ns3/` and is auto-applied by
`scripts/fetch-ns3.sh` against the pinned release (`--print-pin`). Patches cover TCP edge-case
fixes, TBF inner-mode, FQ-Cobalt ACK filter and host isolation, traffic-control
slot dispatcher, L2-overhead accounting, TCP jitter realism knobs, and DualPI2
GPRT. Upstream artefacts (issue text, proposed patches) are filed under
`docs/upstream/`. Bugs are catalogued in `docs/HISTORICAL_BUGS.md`.

## See also

- Repository root — ns-3 port source.
- `../MIGRATION-from-ns2.md` — ns-2 Tcl → ns-3 C++ translation guide.
- `doc/stratum.rst` — Sphinx documentation.
- `docs/PORTING_MAP.md` — class-by-class port plan.
- `specs/0[123]-*.md` — the EDD spec suite.
