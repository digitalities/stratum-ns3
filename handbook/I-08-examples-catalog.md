# Examples catalogue

The substrate ships runnable examples under [`examples/`](../examples/). Each one is annotated below — one paragraph per example, grouped by client. If you're looking for the cookbook entries that walk through a specific example end-to-end, follow the **Recipe** link.

## DiffServ (6 examples)

### `diffserv-example-1.cc`

**Purpose**: ns-3 port of DiffServ4NS example-1 (`simulation-1.tcl`); reproduces the topology, traffic mix, and DiffServ configuration from the original 2001 scenario using PQ scheduling with an sr-TCM/TokenBucket policer. Topology: 5 sources → edge e1 → core → edge e2 → 5 destinations; bottleneck e1→core 2 Mbps / 5 ms.

**Client**: DiffServ. **Recipe**: [diffserv.md → "Edge node with sr-TCM marking and PQ scheduling"](I-03-diffserv.md).

### `diffserv-example-2.cc`

**Purpose**: ns-3 port of DiffServ4NS example-2 (`example-2.tcl`); reproduces the three-class scenario from the original 2001 example with Premium (EF/TokenBucket), Gold (AF/RIO-C/TSW2CM), and Best Effort services carrying TCP and UDP traffic. The binary offers two scales selected via `--scale`: the default `quick` runs the 5-source / 5-destination 13-node topology shared with example-1; `full` runs the 469-node thesis-Scenario-2 reconstruction (AF PHB importance differentiation via a 6-way WRED parameter sweep, mirroring the ns-2.35 reconstruction in `ns2/diffserv4ns/examples/example-2-fullscale/scenario-2.tcl`).

**Client**: DiffServ. **Recipe**: [diffserv.md → "Three-class edge: EF + AF + BE with tr-TCM"](I-03-diffserv.md).

### `diffserv-example-3.cc`

**Purpose**: Reconstruction of thesis Scenario 3 (Section 4.3): a complete DiffServ service model with Premium (EF), Olympic (Gold/Silver/Bronze AF tiers), and Best Effort. The binary offers two scales selected via `--scale`: the default `quick` uses the 13-node topology shared with examples 1 and 2 to exercise the full 5-class service model from Table 4.5 of the thesis; `full` runs the original 771-node scale — 40 web servers, 420 web clients, 300 VoIP/RealAudio senders, plus background traffic and routers — mirroring `scenario-3.tcl` from the original ns-2 work. LLQ scheduler (PQ for Premium, SFQ weights 3:3:3:1 for Olympic + BE).

**Client**: DiffServ. **Recipe**: [diffserv.md → "Hierarchical multi-edge topology"](I-03-diffserv.md).

### `diffserv-example-le.cc`

**Purpose**: Lower-Effort (LE) PHB demonstration, per RFC 8622 (2019). LE is a post-2001 PHB not covered by the original DiffServ4NS module; this example shows the substrate ports it trivially because LE is just "a PHB with strict priority below Best-Effort" — no new code is needed beyond a 2-queue PQ router with BE at priority 0 and LE at priority 1.

**Client**: DiffServ. **Recipe**: [diffserv.md → "Lower-Effort (LE) PHB per RFC 8622"](I-03-diffserv.md).

### `retx-calibration.cc`

**Purpose**: Retransmission-counter calibration harness. A single TCP bulk-transfer flow crosses a 2-node point-to-point link with exactly one deterministic loss injected on the receiver NIC (segment 10). The sender-side DiffServ edge queue disc is monitored by `MonitorHelper`, which split-counts per-DSCP bytes by `TcpRetransmitTag` presence.

**Client**: DiffServ (diagnostic / calibration). **Recipe**: not v1; specialist tool.

### `gold-policer-replay.cc`

**Purpose**: Policer equivalence calibration harness. Replays a byte-identical 1.06M-packet ingress trace through both the ns-3 DiffServ4NS TSW2CM policer and the ns-2.35 reference implementation; a match within a narrow tolerance demonstrates the two implementations are equivalent for byte-identical input, so any cross-simulator throughput residual is attributable to the traffic generator, not to the metering logic.

**Client**: DiffServ (diagnostic / calibration). **Recipe**: not v1; specialist tool.

## L4S (7 examples)

### `diffserv-l4s-s1-latency.cc`

**Purpose**: EF (ECT(1)) vs classic under priority, latency probe. A sender → router → receiver topology where the bottleneck egress carries the queue disc under test. Two modes selected via `--mode` exercise the L4S DualPI2 separation of scalable-CC marked flows from classic traffic.

**Client**: L4S. **Recipe**: [l4s.md → "ECT(1) vs DSCP — same latency, different mechanism"](I-04-l4s.md).

### `diffserv-l4s-s2-equivalence.cc`

**Purpose**: Mixed-flow coupling-formula sanity check. RFC 9332 §2.1 promises throughput equivalence between classic and L4S flows when the L4S flow runs Scalable CC. Because ns-3 mainline lacks an ECT(1)-sending Scalable-CC TCP stack, this scenario frames the result as a coupling-formula sanity check rather than a throughput-equivalence claim — verifying the DualPI2 coupling machinery responds to sustained two-flow contention per RFC 9332 §2.1 eq. (1) / App. A.1.

**Client**: L4S. **Recipe**: [l4s.md → "DualPI2 coupling-formula sanity check"](I-04-l4s.md).

### `diffserv-l4s-fqcodel-comparison.cc`

**Purpose**: Compare DiffServ4NS L4S behaviour against a pure ns-3 mainline `FqCoDelQueueDisc` baseline on the same bottleneck scenario. Disc choice selectable via `--disc={l4s-wred, l4s-coupled-only, l4s-fqcodel-inner, fqcodel}`.

**Client**: L4S. **Recipe**: [l4s.md → "L4S DualPI2 vs FqCoDel head-to-head"](I-04-l4s.md).

### `diffserv-l4s-s1-advantage.cc`

**Purpose**: L4S latency-advantage demonstration. A UDP CBR probe (ECT(1), ~500 kbps) competes with a TcpCubic bulk flow on a 10 Mbps bottleneck under three qdisc modes — `l4s` (DualPI2), `fqcodel` (FqCoDel, no L4S coupling), and `fifo` (drop-tail floor) — selected via `--mode`. The probe one-way delay across modes quantifies the ECT(1) latency advantage. Companion to `diffserv-l4s-s1-latency.cc` (which uses a DSCP-marked probe rather than ECT(1)).

**Client**: L4S. **Recipe**: [l4s.md → "ECT(1) vs DSCP — same latency, different mechanism"](I-04-l4s.md).

### `diffserv-l4s-s2-coexistence.cc`

**Purpose**: L4S / classic coexistence check — demonstrates that responsive L4S and classic flows under the RFC 9332 coupling formula converge to throughput equivalence within ~25 %. Complements `diffserv-l4s-s2-equivalence.cc` (coupling-formula sanity check); this scenario runs a longer steady-state window and surfaces the coexistence regime rather than just the peak coupling response.

**Client**: L4S. **Recipe**: [l4s.md → "DualPI2 coupling-formula sanity check"](I-04-l4s.md).

### `diffserv-hierarchical-l4s.cc`

**Purpose**: Hierarchical composition of two heterogeneous inner queueing disciplines on a single DiffServ edge, dispatched by DSCP. Implements the `draft-briscoe-tsvwg-l4s-diffserv-02` Figure 1 ("gap 1") pattern: slot 0 = `l4s::QueueDisc` for EF/DSCP-marked traffic, slot 1 = mainline queue disc for everything else.

**Client**: L4S (advanced / hierarchical composition). **Recipe**: not v1; advanced topology.

### `diffserv-l4s-dualpi2-gprt-parity.cc`

**Purpose**: Cross-validation of two RFC 9332 DualPI2 implementations under identical conditions: the in-tree `l4s::QueueDisc` and the upstream-shaped `ns3::DualPi2QueueDisc` from the Veras et al. reference port. Pass `--rootQdisc=l4s`, `--rootQdisc=cake`, or `--rootQdisc=gprt` to select the bottleneck qdisc; all other parameters (topology, TCP variants, access links) are held constant to permit a parity comparison of per-flow goodput and Jain's Fairness Index.

**Client**: L4S (cross-validation). **Recipe**: not v1; specialist tool.

## CAKE (6 examples)

### `diffserv-cake.cc`

**Purpose**: CAKE substrate demonstrator. Drives one UDP CBR per CAKE tin through a `EdgeQueueDisc` configured via `cake::Helper::SetAsCakeDiffserv4`. Each tin carries a saturating CBR (above its own rate-cap) so the per-tin TBF + DRR-across-tin dispatcher determine bottleneck sharing.

**Client**: CAKE. **Recipe**: [cake.md → "CAKE substrate demo — one UDP CBR per tin"](I-05-cake.md).

### `cake-rrul.cc`

**Purpose**: RRUL-style workload (4 saturating TCP downloads + 4 saturating TCP uploads + 4 UDP probes + 1 ICMP ping) over a dumbbell with a single bottleneck shaped by the rate-based CAKE dispatcher (`cake::Helper` RateBased mode). The `FlentCsvSink` emits a per-flow CSV bundle (schema in `scripts/flent-export/SCHEMA.md`).

**Client**: CAKE. **Recipe**: covered indirectly — [cake.md → "RRUL benchmark with DSCP marking"](I-05-cake.md) names this example as the no-DSCP baseline.

### `cake-rrul-diffserv.cc`

**Purpose**: RRUL with DSCP marking on the 4 TCP downloads + 4 TCP uploads. Maps flow 0 → CS0 (BE), flow 1 → CS1 (BK), flow 2 → CS5, flow 3 → EF. Mirrors the CAKE 2018 paper Fig 5 substrate.

**Client**: CAKE. **Recipe**: [cake.md → "RRUL benchmark with DSCP marking"](I-05-cake.md).

### `cake-tcp-4up-squarewave.cc`

**Purpose**: Square-wave 4-flow TCP fairness — four `BulkSendApplication`s start at t=0/5/10/15 s and stop at t=45/50/55/60 s, traversing a single bottleneck shaped by `cake::Helper` rate-based mode. The `FlentCsvSink` emits a per-flow CSV bundle compatible with the Flent `tcp_4up_squarewave` schema.

**Client**: CAKE. **Recipe**: [cake.md → "Square-wave 4-flow TCP fairness"](I-05-cake.md).

### `cake-l4s-composition.cc`

**Purpose**: Demonstrates the substrate's compositional value — configures CAKE diffserv4 with a `l4s::QueueDisc` (DualPI2) as the per-tin inner. No mainline Linux or BSD AQM expresses this composition; the four-slot composer makes it a configuration choice. Two host pairs cross a shared bottleneck with CAKE+L4S on the egress; CLI controls flow count and TCP variant per host pair for an apples-to-apples comparison against pure-CAKE bridge results.

**Client**: CAKE + L4S (advanced composition). **Recipe**: [cake.md](I-05-cake.md); see also [l4s.md](I-04-l4s.md).

### `cake-stratum-bridge-router.cc`

**Purpose**: Closed-loop bridging example. A single ns-3 node with two `EmuFdNetDevice`s acts as an L3 router between two Linux network namespaces, with Stratum CAKE (DiffServ4 + host-isolation) on the forward egress. Used as the cross-validation bridge for Stratum ↔ Linux comparisons; requires a live network setup and is not runnable in a plain ns-3 build.

**Client**: CAKE (specialist / bridge). **Recipe**: not v1; specialist tool.

## Wireless (3 examples)

### `diffserv-wifi-demo.cc`

**Purpose**: Wireless demo — DiffServ4NS edge disc attached to a Wi-Fi 802.11ax AP's downlink NetDevice. Demonstrates the substrate is wireless-agnostic (the queue disc plugs into any `TrafficControlLayer`) and exercises the DSCP → 802.11e UP mapping that ns-3 mainline provides via `qos-utils.cc::QosUtilsMapTidToAc` (RFC 8325-shaped).

**Client**: Wireless. **Recipe**: [wireless.md → "Stratum edge on an 802.11ax AP downlink"](I-07-wireless.md).

### `diffserv-wifi-scheduler-comparison.cc`

**Purpose**: Eight DiffServ4NS schedulers (PQ, RR, WRR, WIRR, SCFQ, WFQ, WF2Q+, LLQ) on the AP downlink over 802.11a 6 Mb/s, plus a single-AC saturation mode for Bianchi 2000 / Magrin et al. WNS3 2021 Figure 3 calibration. Demonstrates that scheduler choice composes with `WifiNetDevice` the same way it composes with `PointToPointNetDevice`.

**Client**: Wireless. **Recipe**: [wireless.md → "Eight schedulers compared on a Wi-Fi AP"](I-07-wireless.md).

### `diffserv-hybrid-wired-wireless.cc`

**Purpose**: Hybrid wired/wireless example — a wired backbone feeds an 802.11ax AP that acts as the DS edge router. The AP classifies traffic by destination STA on its downlink, marks DSCP, and runs an LLQ scheduler (PQ + WFQ hybrid) on the qdisc. Classification and marking happen at the AP, NOT at the wired servers — the realistic deployment shape for residential / SOHO QoS-on-Wi-Fi.

**Client**: Wireless (advanced / realistic deployment). **Recipe**: not v1; the simpler `diffserv-wifi-demo.cc` covers the substrate-agnostic principle.

## AQM evaluation (2 examples)

### `aqm-eval-runner.cc`

**Purpose**: RFC-7928-aligned multi-AQM characterisation runner. Sweeps the full vanilla-AQM matrix (n=13 cells in the `Registry<AqmEntry>` template) across nine congestion-level scenarios. Demonstrates the Stratum substrate composes equivalently with mainline AQMs and DiffServ-aware queue discs through a single binary.

**Client**: AQM evaluation. **Recipe**: [aqm-eval.md → "Characterise the in-tree AQM catalogue in 30 seconds (ellipse diagram)"](I-06-aqm-eval.md).

### `chang-comparison.cc`

**Purpose**: Reproduce R. Chang, M. Rahimi, V. Pournaghshband, "Differentiated Service Queuing Disciplines in NS-3," SIMULTECH 2015 (Split-Dubrovnik) — an external validation scenario for ns-3 DiffServ scheduling modules (SPQ, WFQ, etc.). Used by the substrate as an independent oracle on top of the RFC vectors and the three-way ns-2 / ns-2.35 / ns-3 cross-port.

**Client**: AQM evaluation / external benchmark. **Recipe**: [aqm-eval.md → "Reproduce Chang et al. 2015"](I-06-aqm-eval.md).

## How to add a new example

If you've written a new scenario you think belongs here, the pattern is documented in [extending.md](I-09-extending.md). In short: drop your `.cc` under `examples/`, add an entry to `examples/CMakeLists.txt`, and — if the example introduces a new AQM or scheduler — register the cell in the appropriate `Registry<EntryT>`. The catalogue grows from there.
