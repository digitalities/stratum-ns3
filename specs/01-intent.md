# Intent Specs — diffserv-ns3

**Tier:** Intent (capability assertions, not implementation tests)
**Source:** Direct mapping from `dsCore.h` author header in DiffServ4NS-0.1
**Audience:** Senior reviewers, downstream users, paper readers
**Format:** Each spec is a single declarative sentence describing observable behaviour. Specs are numbered for cross-reference from code comments and structural tests.

## I-1: Edge router classification and marking

**I-1.1** The edge router shall classify incoming packets into mark rules based on source node address.

**I-1.2** The edge router shall classify incoming packets into mark rules based on destination node address.

**I-1.3** The edge router shall classify incoming packets into mark rules based on transport protocol type (TCP, UDP).

**I-1.4** The edge router shall classify incoming packets into mark rules based on application type as carried in the packet header.

**I-1.5** The edge router shall write the resulting DSCP into the IPv4 header DS field of every packet that matches a mark rule.

**I-1.6** Packets that do not match any mark rule shall retain their original DSCP.

## I-2: Meters and policers

**I-2.1** The module shall provide a Single Rate Three Colour Marker conforming to RFC 2697 (srTCM).

**I-2.2** The module shall provide a Two Rate Three Colour Marker conforming to RFC 2698 (trTCM).

**I-2.3** The module shall provide a single-rate two-colour Token Bucket meter.

**I-2.4** The module shall provide a Time Sliding Window Two Colour Marker (TSW2CM) per RFC 2859.

**I-2.5** The module shall provide a Time Sliding Window Three Colour Marker (TSW3CM) per RFC 2859.

**I-2.6** The module shall provide an FW policer (ported from ns-2 SFDPolicy) that maintains per-flow state and penalises flows exceeding a byte threshold.

**I-2.7** Each meter shall support both colour-blind and colour-aware modes (colour-blind in v1; colour-aware deferred).

**I-2.8** Each policer shall support a configurable initial DSCP, downgrade-1 (yellow) DSCP, and downgrade-2 (red) DSCP.

## I-3: Schedulers

**I-3.1** The module shall provide a Round Robin scheduler.

**I-3.2** The module shall provide a Weighted Round Robin scheduler with configurable per-queue integer weights.

**I-3.3** The module shall provide a Weighted Interleaved Round Robin scheduler.

**I-3.4** The module shall provide a Priority Queue scheduler with optional per-queue rate cap.

**I-3.5** The module shall provide a Weighted Fair Queueing (WFQ) scheduler with configurable per-queue weights, providing throughput proportional to weights within an error bound of one maximum packet size per active flow.

**I-3.6** The module shall provide a Worst-case Fair Weighted Fair Queueing+ (WF2Q+) scheduler with the eligibility property: no packet is served before its virtual start time.

**I-3.7** The module shall provide a Self-Clocked Fair Queueing (SCFQ) scheduler.

**I-3.8** The module shall provide a Start-time Fair Queueing (SFQ) scheduler.

**I-3.9** The module shall provide a Low Latency Queueing (LLQ) scheduler that combines a strict-priority queue with a configurable fair-queueing scheduler for the remaining traffic.

## I-4: Per-Hop Behaviours and queue structure

**I-4.1** The core router shall support up to 9 physical queues per network device.

**I-4.2** Each physical queue shall support up to 3 virtual queues for drop precedence.

**I-4.3** Each virtual queue shall implement RED with In/Out (RIO) drop precedence.

**I-4.4** The module shall provide an Expedited Forwarding PHB (RFC 3246) using the Priority Queue scheduler and DSCP 46 by default.

**I-4.5** The module shall provide an Assured Forwarding PHB group (RFC 2597) supporting AF1x, AF2x, AF3x, AF4x classes with three drop precedence levels each.

**I-4.6** The module shall provide a Default PHB (best-effort) for unmarked traffic.

## I-5: Monitoring (per-DSCP statistics)

**I-5.1** For UDP flows, the module shall report One-Way Delay (OWD) per DSCP as instantaneous, average, minimum, and frequency distribution.

**I-5.2** For UDP flows, the module shall report IP Delay Variation (IPDV) per DSCP as instantaneous, average, minimum, and frequency distribution.

**I-5.3** For TCP flows, the module shall report goodput per DSCP.

**I-5.4** For TCP flows, the module shall report Round-Trip Time per DSCP as instantaneous and frequency distribution.

**I-5.5** For TCP flows, the module shall report Window Size per DSCP as instantaneous and frequency distribution.

**I-5.6** Per queue, the module shall report instantaneous and average queue length.

**I-5.7** Per (queue, drop precedence) pair, the module shall report instantaneous and average queue length.

**I-5.8** The module shall report maximum burstiness for queue 0 (EF queue by default).

**I-5.9** Per queue, the module shall report departure rate.

**I-5.10** Per (queue, drop precedence) pair, the module shall report departure rate.

**I-5.11** Per DSCP, the module shall report received, transmitted, and dropped packet counts as both absolute values and percentages.

**I-5.12** Drop counts shall distinguish between drops caused by RIO/RED droppers and drops caused by physical buffer overflow.

## I-6: Configuration

**I-6.1** The module shall expose configuration via a `stratum::Helper` class that requires no Tcl scripting.

**I-6.2** All configurable parameters shall be exposed as ns-3 attributes via `TypeId::AddAttribute`, allowing inspection and modification through the standard `Config::Set` mechanism.

**I-6.3** The module shall provide example scenarios reproducing example-1 and example-2 from DiffServ4NS-0.1, with topologies, traffic generators, and output traces equivalent to the originals.

## I-7: Compatibility and licensing

**I-7.1** The module shall be released under GPLv2, compatible with both ns-3 mainline and the DiffServ4NS-0.1 ancestor.

**I-7.2** The module shall preserve attribution to the original DiffServ4NS author (Sergio Andreozzi), to the Nortel Networks ns-2 diffserv module from which DiffServ4NS derived, and to all intermediate contributors.

**I-7.3** The module shall build cleanly against ns-3 mainline (currently the latest release at port time).

## I-8: Meter colour observability

The substrate shall expose a uniform `MeterColour` trace source on the
`Meter` base class. The trace fires on every colour decision (every
ingress packet that passes through any concrete meter in the registry).
The trace signature is `(Colour colour, uint32_t classId, Time when)`,
carrying enough information for external consumers (recipes, audit
tools, future telemetry) to reconstruct the meter's marking sequence
without inspecting internal state.

All concrete meters in the registry (sr-TCM, tr-TCM, TSW2CM, TSW3CM,
byte-acct, FW, token-bucket, dumb) fire the trace uniformly. Meters
without per-class state emit `classId=0`; the dumb-meter additionally
emits `Colour::GREEN` unconditionally.

S-specs derived: S-meter-base-trace-registered, S-meter-trace-srtcm,
S-meter-trace-trtcm, S-meter-trace-tsw2cm, S-meter-trace-tsw3cm,
S-meter-trace-byteacct, S-meter-trace-fw, S-meter-trace-tokenbucket,
S-meter-trace-dumb.

## I-9: Per-class diagnostic instrumentation in diffserv-example-1

The `diffserv-example-1` binary shall emit, in addition to its existing
aggregate trace files, three new per-class artefacts in the run directory:

  * `OWD-ef.tr` and `OWD-be.tr` — per-packet OWD time-series for the
    EF and BE classes (whitespace-separated, headerless,
    `time owd_seconds` rows; mirrors the existing `OWD.tr` convention).
  * `FlowRate.csv` — per-window per-class throughput in kbps with
    header `time,classId,rate_kbps`; one row per (sample-window,
    classId) pair at the configured sample window (default 100 ms).
  * `MeterColour.csv` — per-window per-class colour-count aggregate
    with header `time,classId,green,yellow,red`; one row per
    (sample-window, classId) pair, consumed via the substrate
    `MeterColour` trace source (I-8).

These artefacts are read by the `diffserv-example-1`,
`diffserv-srtcm`, and `diffserv-wfq-true-pgps` recipes under the
augment-all-three multi-plot policy.

S-specs derived: S-example-1-perclass-owd, S-example-1-perclass-flowrate,
S-example-1-metercolour-aggregate.

## I-10: Per-flow CSV host attribution

The substrate helper `FlentCsvSink` shall, when a per-flow registration
supplies a non-empty host identifier, emit that identifier as a `host`
column in the corresponding per-flow CSV. When no host identifier is
supplied (default for legacy callers), the `host` column is still
present but holds an empty value per row. The CSV schema is therefore
invariant across all callers of the helper.

S-specs derived: S-flent-sink-host-column-emitted,
S-flent-sink-host-attribution-correct,
S-flent-sink-backwards-compat-no-hostid.

## I-11: cake-host-isolation convergence characterisation

The `cake-host-isolation` recipe shall publish per-host steady-state
goodput shares for both isolation modes (triple, flowblind) at a
simulation length sufficient for TCP convergence. The chosen length
and the observed per-host shares shall be documented in the recipe's
calibration notes and rendered into three figures (per-arm time-series
plus a steady-state grouped bar).

S-specs derived: `S-cake-host-asymmetric-capture-bounded` (asymmetric
capture-share invariant pinned in CI). The recipe artefacts themselves
remain deliverables verified by visual inspection + plot-recipe
--validate against locked calibration.

## I-12: L4S scenarios drive P.I.² controller into active region under nominal load

The L4S demonstration scenarios (`diffserv-l4s-s2-equivalence`, `diffserv-l4s-s1-latency`)
shall configure their default parameters so that the DualPI2 P.I.² controller engages
under the binary's default load — `pPrime` non-zero across a sustained fraction of
samples (≥5%), with the RFC 9332 coupling cascade `pC = p'²` and `pL = min(k·p', 1)`
(§2.1 eq. (1); Appendix A.1 Figure 6 lines 4–5) holding approximately. The criterion
is formula verification across whatever operating region the load produces — under
the defaults' non-responsive overload the controller legitimately rides high (mean
`pPrime` ≈ 0.75 with clamp episodes at 1.0) — not strong throughput differentiation
(which requires responsive flows, deferred to v1.8+). *(An earlier revision required
"weak controller engagement (`pPrime` in `[1e-5, 1e-2]`)"; that band was measured
while the classic lane sat in its constructor trap state with the controller asleep
— see the 2026-06-10 L4S validation audit.)*

*(Erratum 2026-06-10: an earlier revision of this intent stated the cascade as
`pC = (k·p')²` / `pL = min(2·p', 1)` and attributed it to "RFC 9332 §4.1". That was a
misreading of eq. (1) — the RFC divides by k, `p_C = (p_CL / k)²`, equivalently
`p_C = p'²` with `p_CL = k·p'` — and §4.1 is a Security Considerations subsection.
The implementation encoded the same misreading; see S-L4S.13 and the superseding ADR.)*

S-specs derived: S-l4s-piControl-fires-at-nominal-load, S-l4s-s1-latency-arm-differentiation.

## I-15: L4S client conforms to RFC 9331/9332 core semantics

The L4S client (`stratum::l4s::QueueDisc` + `CoupledScheduler`) shall implement the
DualQ Coupled PI2 semantics of RFC 9332 and the ECN discipline of RFC 9331:
ECT(1)/CE classification into the L queue; a PI² base-probability controller whose
arithmetic reproduces Appendix A.1 Figures 2 and 6 (gains `alpha = 0.16 Hz`,
`beta = 3.2 Hz` at the default `Tupdate = 16 ms`, classic-queue sojourn input,
15 ms classic target); the coupling cascade `p_CL = k·p'`, `p_C = p'²` honouring the
configured coupling factor `k`; CE-only signalling on the L4S lane (never demoting
ECT(1), never re-marking codepoints otherwise); drop-not-mark on the classic coupled
path; and DSCP preservation through both lanes (the queue disc MUST NOT alter the
DSCP of any packet it forwards).

S-specs derived: S-L4S.1 – S-L4S.15 (see the S-L4S section of `02-structural.md`).

## I-13: aqm-eval-runner plot has correct axis semantics (Mbps, not ECDF [0,1])

The `aqm-eval-runner` recipe's `aqm-envelope` plot shall render its y-axis in
Mbps (not bps with scientific-notation tick labels that read as [0, 1] × 10⁷).
The calibration range shall be expressed in Mbps with explicit per-flow vs
aggregate semantics documented in `notes:`.

S-specs derived: S-aqm-envelope-axis-in-mbps.

## I-14: aqm-eval-runner measurement and registry factory correctness

The AQM characterisation harness (`aqm-eval-runner`) shall measure per-flow
goodput over the full `--simTime` window (no warm-up trim applied to the
denominator without the matching numerator trim), shall deliver unsaturated
UDP-CBR traffic across the rate-limited bottleneck at the offered rate to
within the IP+UDP header-overhead band, and shall construct its `StratumRed`
cell through the four-step factory sequence that a naive
`CreateObject<stratum::RedQueueDisc>()` instantiation lacks (the naive
instantiation drops every packet). Every Stratum composite cell the registry
fabricates shall leave the factory with a functional AQM on each traffic
path it advertises; in particular the `StratumL4sWred` cell's classic lane
shall run WRED early drop, not the constructor trap defaults.

S-specs derived: S-aqm-runner-goodput-window-full-simtime,
S-aqm-runner-udp-cbr-rate-band, S-aqm-stratumred-factory-four-step,
S-aqm-stratuml4swred-factory-classic-lane-functional.

## I-16: CAKE autorate-ingress closed loop tracks the observed bottleneck

When autorate-ingress is enabled on the CAKE rate-based shaper, the aggregate
shaper rate shall be inferred from the observed packet-arrival stream rather
than configured statically: the module shall estimate the downstream
bottleneck's delivered rate by a peak-bandwidth exponentially-weighted moving
average over arrival windows and reshape the aggregate clock to ≈15/16
(93.75 %) of the inferred peak, tracking the bottleneck upward as it changes.
The estimator and the 15/16 set-point reproduce the Linux `sch_cake.c`
autorate-ingress mechanism (`cake_enqueue`); the CAKE paper motivates the
set-point — shape just below the physical capacity so the managed queue stays
in CAKE rather than at the bottleneck (§III-A) — but does not specify the
algorithm. The estimate is raised quickly when a higher bottleneck is observed
and lowered only slowly (the kernel's asymmetric filter); the feature is
default-off and, when disabled, perturbs nothing on the wire.

S-specs derived: S-17.63 (upward convergence), S-17.64 (upward re-adaptation),
S-17.65 (peak-bandwidth estimator byte-exact to `cake_enqueue`), S-17.67
(downward adaptation characterization).

## Out of scope for v1

- IPv6 DSCP handling (the original was IPv4-only; deferred but architecturally compatible)
- ECN interaction with RIO (deferred — ns-3 RED already handles ECN, integration is straightforward but not on the critical path)
- Hierarchical token bucket meters
- Colour-aware mode for srTCM/trTCM (the original is colour-blind; spec I-2.7 makes this explicit)
- Real-time emulation (FdNetDevice integration)
