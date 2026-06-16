---
title: Extending the substrate to wireless (demo)
origin: 2026-written
status: demo
last-updated: 2026-06-06
---

# Extending the substrate to wireless

> **Hands-on**: see [Wireless recipes](I-07-wireless.md) for runnable recipes that demonstrate the Stratum + Wi-Fi composition described here.

This chapter documents a **demo-grade extension** of the DiffServ4NS
substrate to an ns-3 Wi-Fi link. Unlike [L4S](II-06-l4s-client.md) and
[CAKE](III-04-cake.md), this chapter is not a calibrated client of the substrate —
there is no Q-tier scenario, no validation oracle, no spec entry.
What it demonstrates is that the substrate is **wireless-agnostic
by construction**: the ns-3 module's queue discs talk to the
network stack through `Ipv4QueueDiscItem` and the standard
`TrafficControlLayer`, exactly the abstractions every ns-3
`NetDevice` honours, including `WifiNetDevice` and the LTE/5G
device families.

> **Reading order.** Stand-alone. Read after [The ns-3 module](II-08-ns3-module.md).
> [L4S](II-06-l4s-client.md) and [CAKE](III-04-cake.md) are independent of this one.

## Why a demo, not a scenario

The 2001 DiffServ4NS architecture and all three Q-tier validation
scenarios (Q-1, Q-2, Q-3; defined in `specs/03-quality.md`) target a wired bottleneck. Reproducing the
2001 results on a wireless link would mean **changing the experiment**,
not validating the port. So the wireless extension is reported as a
*capability* (the substrate composes above any `NetDevice`) rather
than as a numerically calibrated result.

A second reason is the absence of a clean external oracle. Wired
results have three calibration anchors (the 2001 ns-2.29 baseline,
the 2026 ns-2.35 port, and the TF-TANT real-network measurements
from Ferrari et al.). For wireless DiffServ-over-802.11 the closest
analogues are Linux `tc-cake` running behind a Wi-Fi AP and the
RFC 8325 DSCP-to-UP mapping vectors, neither of which has a
published reference timeseries comparable to TF-TANT. A calibrated
wireless scenario therefore deserves its own paper, not a chapter
section.

## What the demo shows

`examples/diffserv-wifi-demo.cc` builds a minimal topology:

```
  server ──P2P 100 Mb/s, 1 ms──▶ AP ══ 802.11ax 5 GHz ══▶ sta0  (EF, DSCP 46)
                                                       \═▶ sta1  (BE, DSCP 0)
```

The AP runs the **standard DiffServ edge queue disc** on its Wi-Fi
`NetDevice` — created and installed via the same
`TrafficControlLayer::SetRootQueueDiscOnDevice()` call used in every
other Stratum example. Two flows leave the server:

- **EF**: 300 kbps OnOff to `sta0`, classified to DSCP 46 by the
  edge mark rule, policed by a `TokenBucketPolicy` (CIR 300 kbps,
  CBS 4687 B = Cisco MQC default `Bc = CIR · 125 ms`).
- **BE**: 50 Mbps OnOff to `sta1`, classified to DSCP 0, no policer.

Once the edge stamps the DSCP, the packet leaves the queue disc and
enters `WifiNetDevice`. The mainline 802.11e mapper
(`src/wifi/model/qos-utils.cc`,
`QosUtilsMapTidToAc`) is RFC 8325-shaped: DSCP 46 → AC_VO, DSCP 0 →
AC_BE. **No additional wiring is required between Stratum and Wi-Fi.**

## Caveats

The demo deliberately exposes three concessions that any wireless
substrate user must make explicit:

1. **`LinkBandwidth` is a representative airtime budget, not a
   physical line rate.** The scheduler attribute is set to
   `phyRateMbps × airtimeFraction` (defaults: 60 Mb/s × 0.45). On a
   point-to-point link this attribute equals the wire rate; on Wi-Fi
   it must encode the user's expectation of sustained throughput
   under contention. This is the same approximation Linux
   `tc-cake` operators make when running CAKE behind a Wi-Fi AP.
2. **`L2OverheadBytes` is a representative scalar.**
   `diffserv::Helper::DetectL2OverheadBytes()` returns 0 for
   `WifiNetDevice` on purpose — 802.11 framing is variable per packet
   (QoS-data + LLC/SNAP, with or without AMPDU aggregation, A-MSDU,
   block-ack overheads). The demo passes a representative 36 B
   (QoS-data + LLC/SNAP, no aggregation). A production scenario must
   either (a) pick a representative value for the regime under study
   or (b) wait for ns-3 mainline to grow a virtual
   `NetDevice::GetL2OverheadBytes()` API.
3. **There is no validation target.** The output is two `EF rx` /
   `BE rx` byte counters, not a calibrated metric.

## What the run shows

A representative run on the pinned ns-3 revision:

| Mode | EF rx (B) | BE rx (B) |
|---|---|---|
| `--diffserv=true`  | 190 400 | 37 497 600 |
| `--diffserv=false` | 262 400 | 37 497 600 |

Two notes on this surprising-looking direction:

- BE arrives at the same byte count in both modes because the BE
  flow is air-saturating and does not interact with the Stratum edge in
  any class-isolating way under PQ-on-AP-downlink (the bottleneck is
  on the wireless air, not in the queue disc).
- **EF rx is *lower* with DiffServ on** because the token-bucket
  policer downgrades over-rate EF packets to DSCP 48 (the standard
  RFC-3246 violate-action), and ns-3's mainline DSCP→UP mapper does
  not give DSCP 48 a privileged AC. The interesting metric in a
  proper wireless scenario is therefore **EF latency under BE
  saturation**, not EF byte count, and the demo as shipped does not
  measure latency.

The point is the *shape* of the result, not its magnitude: the Stratum
edge composed cleanly above an 802.11ax `WifiNetDevice` with
**zero module changes**. A future paper can take this attachment
pattern and add a calibration target.

## What changed in the module

Nothing. The grep across `model/` and `helper/`
finds no hard coupling to `PointToPointNetDevice` or `CsmaNetDevice`.
The single soft coupling — `diffserv::Helper::DetectL2OverheadBytes()`
— already documents the wireless override path in its source comment
(`helper/stratum-helper.cc:38`).

## Scheduler comparison demo

`examples/diffserv-wifi-scheduler-comparison.cc` extends the
demo into a side-by-side comparison of the Stratum scheduler catalogue
over the **same** 802.11a 6 Mb/s topology (4 STAs, one per traffic
class: EF, AF41, BE, BK). Offered load is set to ~128 Mb/s aggregate
so the AP queue disc backs up and the scheduler choice is the only
mechanism that determines per-class share and latency.

The catalogue below is regenerated from the scheduler registry by
`scripts/regen-handbook-tables.sh`; do not edit between the markers.

<!-- BEGIN registry-table: scheduler-catalogue -->
| Tag | Name | Family | Parameter shape | Description |
|---|---|---|---|---|
| pq | PQ | priority | priority-winlen | Strict priority — queue 0 served first; WinLen is the rate-estimator window for the optional per-queue rate cap |
| rr | RR | round-robin | none | Plain round-robin across all queues; no weights |
| wrr | WRR | round-robin | rr-weights | Weighted round-robin; per-queue integer-style weights via SetParam |
| wirr | WIRR | round-robin | rr-weights | Weighted interleaved round-robin; per-queue integer weights |
| scfq | SCFQ | fair-queue | fq-shares | Self-clocked fair queueing; per-queue fractional weights summing to 1 |
| sfq | SFQ | fair-queue | fq-shares | Start-time fair queueing; per-queue fractional weights summing to 1 |
| wfq | WFQ | fair-queue | fq-shares | Parekh-Gallager PGPS — true V(t) snapshot; per-queue fractional weights |
| wf2qp | WF2Q+ | fair-queue | fq-shares | Worst-case fair WFQ+ (Bennett-Zhang 1997, time-discrete); per-queue weights |
| llq | LLQ | hybrid | hybrid-llq | Cisco LLQ: queue 0 is strict-priority slot (weight=0 sentinel); queues 1..N share residual via WFQ-style weights summing to 1 |
<!-- END registry-table: scheduler-catalogue -->

Two design choices distinguish this example from the [What the demo shows](#what-the-demo-shows) section:

- **QoS support is disabled on the Wi-Fi MAC** (`QosSupported=false`).
  With QoS enabled, ns-3's `WifiMac` splits packets into per-AC queues
  based on the DSCP→AC mapping and EDCA differentiates classes at L2,
  which makes the qdisc-level scheduler choice nearly invisible. To
  isolate the queue-disc scheduler as the only differentiating mechanism,
  this example turns QoS off. The [What the demo shows](#what-the-demo-shows) demo (which exercises the
  full Stratum + WMM stack) remains the canonical example for the
  WMM-aware case.
- **A constant-rate Wi-Fi link** at `OfdmRate6Mbps` ensures the link
  capacity is well below the offered load. With `IdealWifiManager`
  the rate adapts upward and the AP queue disc never builds up, so per-class outcomes are identical across schedulers.

A representative single run (8 s, default CLI):

| Scheduler | EF kb/s | EF p99 ms | AF kb/s | AF p99 ms | BE kb/s | BE p99 ms | BK kb/s | BK p99 ms |
|---|---|---|---|---|---|---|---|---|
| pq    | 326 | 503 | 4672 | 661 |  354 |  976 |    0 |    0 |
| rr    | 237 | 658 | 1670 | 967 | 1970 |  985 | 1598 |  986 |
| wrr   | 272 | 510 | 2351 | 888 | 2068 |  992 |  744 | 1452 |
| wirr  | 301 | 510 | 2265 | 889 | 1988 |  991 |  863 | 1450 |
| scfq  | 306 | 506 | 2328 | 847 | 1896 |  988 |  881 | 1483 |
| wfq   | 297 | 503 | 2543 | 843 | 1691 | 1959 |  883 | 1719 |
| wf2qp | 308 | 503 | 2383 | 847 | 1867 |  992 |  850 | 1482 |
| llq   | 304 | 503 | 2533 | 841 | 1673 | 1885 |  899 | 1658 |

![Stratum scheduler comparison over 802.11a 6 Mb/s with the AP
queue disc backed up. Top panel: per-class throughput. Bottom panel: per-
class p99 OWD.](figures/12-wireless/scheduler-comparison.png)

Three observations match textbook expectations:

- **PQ** strict-priority correctly starves BK (0 kb/s) and serves EF
  and AF first; BE gets only the residual after AF leaves the queue.
- **RR** distributes airtime equally across the four classes
  (per-packet round-robin), so EF gets about a quarter of capacity
  even though it has far less.
- **WRR / WIRR / SCFQ / WFQ / WF2Q+ / LLQ** all give EF and AF a
  proportionally larger share than BE/BK without starving the lower
  classes — the difference between them is timing fairness, not
  first-order share allocation. WIRR smooths the WRR bursts (it
  reaches BK earlier in the cycle) and therefore reaches a less
  uneven share.

The latencies (~500 ms p99 across all classes) are an artefact of
the deeply over-saturated queue disc; the comparison is about *relative*
share allocation under saturation, not absolute latency targets. As
with [What the demo shows](#what-the-demo-shows) this is a demo, not a Q-tier scenario.

### Engaging WMM at L2: a four-mode matrix

The example exposes a `--wmmMode` flag with four values that exhaust
the meaningful combinations of qdisc-level and L2 differentiation:

- **`off`** (default) — `QosSupported=false` on AP and STAs. All four
  classes share the AC_BE_NQOS L2 queue; only the DiffServ queue disc
  differentiates. This is the canonical "queue disc is the sole
  differentiator" demo (the table above).
- **`hybrid`** — `QosSupported=true`. ns-3's mainline DSCP-to-AC
  mapping (`QosUtilsMapTidToAc`, `src/wifi/model/qos-utils.cc`)
  routes EF (DSCP 46) to AC_VO, AF41 (DSCP 34) to AC_VI, BE (DSCP 0)
  to AC_BE, CS1 (DSCP 8) to AC_BK at L2; the DiffServ queue disc above
  continues to gate which class dequeues next. This is the realistic
  WMM-router shape — the same composition Linux qdiscs (cake,
  fq_codel, htb) ride above the WMM-aware Wi-Fi driver in
  production.
- **`qdisc-only`** — `QosSupported=true` for infrastructure
  (Block-Ack, A-MPDU when applicable), but a custom
  `WifiHelper::SetSelectQueueCallback` returns `AC_BE` for every
  packet so EDCA at L2 sees a single AC and does not differentiate.
  The queue disc above is the only differentiator; QoS infrastructure is
  available without QoS routing.
- **`edca-only`** — `QosSupported=true`, but the inner DiffServ queue disc
  collapses to a single shared queue (`NumQueues=1`, every DSCP
  maps to queue 0). Packets are still classified and stamped with
  the right DSCP for L2 routing, but the queue disc cannot reorder them.
  Only EDCA at L2 differentiates. Useful as a pure-WMM baseline
  column.

A `--lowLoad` flag scales the offered load from the default
~128 Mb/s aggregate (~25× over-saturation) down to ~7 Mb/s
(~1.2× link cap), so the queue disc only briefly queues and L2 effects
have a chance to surface.

#### Mode × load matrix: per-class throughput

![Stratum schedulers over 802.11a 6 Mb/s — qdisc-only (off) vs WMM-on
(hybrid) at high (~25×) and low (~1.2×) load. 8 schedulers × 4 classes
per panel; bars are per-class delivered rate. The visual near-equality
between off and hybrid (at both loads) is the headline finding of this
section.](figures/12-wireless/scheduler-comparison-matrix.png)

The figure shows mode `off` and mode `hybrid` (the qdisc-comparison
demo's two main modes) side-by-side at both loads. Mode `qdisc-only` is
omitted because it is bit-for-bit identical to `hybrid` in both regimes
(see analysis below). Mode `edca-only` is shown separately further down
because its single-row output does not fit the per-scheduler matrix.

Two findings from the high-load row of the matrix:

- **`hybrid` and `qdisc-only` are bit-for-bit identical at high
  load.** Both run the full 8-scheduler sweep; in both, the AP queue-disc
  backlog is so deep (~25× saturation, p99 ~500-2000 ms) that EDCA's
  short-timescale ordering at L2 is dwarfed by qdisc-induced
  latency. Whether L2 distinguishes ACs or treats everything as
  AC_BE is invisible at this metric.
- **`edca-only` at high load is degenerate** (plotted separately
  below). With one shared queue, tail-drop happens uniformly
  in arrival order: BE (60 Mb/s offered) and BK (60 Mb/s offered)
  crowd the FIFO and crush EF (300 kb/s offered → 27 kb/s
  delivered, ~9 % survival). EDCA at L2 cannot recover packets that
  were already dropped at the queue disc. The lesson is that pure WMM
  without per-class queueing somewhere upstream — queue disc,
  application-layer admission control, or a separate VoIP path —
  provides no protection against bulk-traffic starvation at
  saturation.

![WMM mode = edca-only: single-queue queue disc + EDCA at L2. At high load
the FIFO crushes EF; at low load EDCA in a single-AP DL-only scenario
behaves the same as hybrid. The plot reflects single-row CSV output
(scheduler choice does not apply when the inner queue disc has only one
queue).](figures/12-wireless/scheduler-comparison-edca-only.png)

#### Low load (~1.2× link cap)

In the low-load row of the matrix, **`hybrid`, `qdisc-only`, and `edca-only`
are bit-for-bit identical**, and the `off` row differs only in the
fourth significant digit. Most schedulers (PQ, RR, WRR, SCFQ, WFQ,
WF2Q+, LLQ) produce identical numbers within a mode; only WIRR
diverges, and only by 30-70 kb/s on EF/AF. Why?

This is a single-transmitter scenario: only the AP sends data
(server → AP → STAs is the data path; STAs are receivers). EDCA
differentiates between *competing contenders* — multiple devices
contending for the same channel — and provides no internal
ordering benefit when there is only one contender. With one AP and
no UL traffic, AC_VO does not "win" against AC_BE because there is
nothing to win against; the AP simply transmits whichever packet
its NIC pulls next, and the WifiMacQueue ahead of the radio absorbs
the 1.2× excess into a ~500 ms FIFO buildup that affects every AC
identically.

The bit-equality across modes at low load is therefore not a bug;
it is the expected behaviour for a DL-only single-AP scenario. To
surface real EDCA differentiation, the experiment needs *multiple
contenders* — STA-uplink traffic, hidden-node configurations, or
multi-AP/multi-BSS scenarios — none of which the current [Scheduler comparison demo](#scheduler-comparison-demo)
exercises.

#### What this implies for the `WMM` question

For the qdisc-comparison demo as it stands:

- **Default `off` is the right setting** for the [Scheduler comparison demo](#scheduler-comparison-demo) table. It
  isolates the queue disc as the sole differentiator and produces the
  cleanest cross-scheduler comparison.
- **`hybrid` is the right setting for a realistic WMM-router shape**
  but produces near-identical results to `off` here, because queue-disc
  backlog dominates p99 at high load and there is no inter-contender
  EDCA action at low load. The mode is documented for users who
  want to verify the composition works (and it does).
- **`qdisc-only` and `edca-only` are diagnostic instruments** for
  separating the layers' contributions in regimes where they would
  matter. In this single-AP DL-only scenario they don't surface a
  visible EDCA effect; in a multi-contender Wi-Fi scenario (the
  Q-tier wireless paper future-work item) they would.

CSVs at `handbook/figures/12-wireless/scheduler-comparison{,-wmm,-low,-wmm-low,-edca-only}.csv`.

### Running the example: flag reference and recipes

`diffserv-wifi-scheduler-comparison.cc` exposes the following CLI
flags. Run from the ns-3 build directory as
`./ns3 run "diffserv-wifi-scheduler-comparison [flags]"`.

| Flag | Default | What it does |
|---|---|---|
| `--scheduler` | `pq` | Stratum queue-disc scheduler. One of `pq`, `rr`, `wrr`, `wirr`, `scfq`, `wfq`, `wf2qp`, `llq`. Ignored when `--wmmMode=edca-only`. |
| `--wmmMode` | `off` | L2 differentiation mode. `off`: QoS off, queue disc only. `hybrid`: QoS on, queue disc + EDCA. `qdisc-only`: QoS on, EDCA forced to AC_BE only. `edca-only`: QoS on, queue disc collapsed to single queue. See [Engaging WMM at L2](#engaging-wmm-at-l2-a-four-mode-matrix). |
| `--lowLoad` | `false` | If true, scale offered load down from ~128 Mb/s aggregate (~25× link cap) to ~7 Mb/s (~1.2× link cap). Surfaces L2-side dynamics when the queue disc is not permanently backlogged. |
| `--singleAcSaturation` | `false` | Switch to single-AC saturation mode (no Stratum, single STA pair, bidirectional UDP). Used for Bianchi 802.11a sanity and Magrin 802.11ax Figure 3 calibration. See [Saturation-throughput sanity checks](#saturation-throughput-sanity-checks). |
| `--standard` | `80211a` | Wi-Fi standard. `80211a` (6 Mb/s OFDM) for the queue-disc demos, `80211ax` for Magrin calibration. |
| `--heMcs` | `5` | HE MCS index 0-11 when `--standard=80211ax`. |
| `--numStas` | `1` | Number of STAs in `--singleAcSaturation` mode. The queue-disc demo always uses 4 STAs (one per class). |
| `--simTime` | `10.0` | Simulation duration in seconds (1 s warm-up, rest measured). |
| `--airtimeFraction` | `0.65` | Fraction of PHY rate the scheduler treats as the link bandwidth attribute. Wi-Fi airtime budget approximation. |
| `--phyRateMbps` | `6.0` | PHY rate fed to the scheduler's `LinkBandwidth` attribute. |
| `--l2OverheadBytes` | `36` | Per-packet 802.11 framing overhead the scheduler attributes to each packet (LLC/SNAP + QoS-data MAC header). |

#### Recipes

**1. Headline scheduler comparison (default).** Compares 8 Stratum
schedulers over 802.11a 6 Mb/s with QoS off; the queue disc is the only
differentiator. Produces the row in `scheduler-comparison.csv`.

```
./ns3 run "diffserv-wifi-scheduler-comparison --scheduler=pq"
```

Replace `pq` with `rr`, `wrr`, `wirr`, `scfq`, `wfq`, `wf2qp`, `llq`
to sweep all eight. Each invocation prints one CSV-formatted row.

**2. Realistic WMM-router shape.** Same comparison, with WMM enabled
at L2. The Stratum queue disc continues to gate per-class share; ns-3 routes
DSCP→AC at L2 and EDCA contends per AC. Use this when comparing
against Linux `tc-cake`-over-Wi-Fi behaviour.

```
./ns3 run "diffserv-wifi-scheduler-comparison --scheduler=pq --wmmMode=hybrid"
```

**3. Low-load regime (briefly congested queue disc).** Drops offered load
to ~1.2× link cap so the queue disc is only briefly backlogged. In a
multi-contender topology this regime would surface EDCA's
short-timescale ordering; in this single-AP DL-only demo it
produces near-identical results across modes (see [Engaging WMM at L2](#engaging-wmm-at-l2-a-four-mode-matrix)).

```
./ns3 run "diffserv-wifi-scheduler-comparison --scheduler=pq --wmmMode=hybrid --lowLoad=true"
```

**4. Bianchi 2000 single-station sanity.** 802.11a 6 Mb/s OFDM,
single STA, bidirectional UDP saturation. Single-row output to
verify the airtime-budget approximation against the published
~93 % efficiency.

```
./ns3 run "diffserv-wifi-scheduler-comparison --singleAcSaturation"
```

Expected: `aggregate_mbps ≈ 5.5` (within ±0.5 noise across runs).

**5. Magrin et al. WNS3 2021 Figure 3 sweep.** 802.11ax HE-MCS5,
20 MHz, 5 GHz, bidirectional UDP at 100 Mb/s per direction per flow,
sweep N=1..19 STAs. Each invocation produces one point on the curve.

```
for N in 1 3 5 7 9 11 13 15 17 19; do
  ./ns3 run "diffserv-wifi-scheduler-comparison \
    --singleAcSaturation --standard=80211ax --heMcs=5 --numStas=$N"
done
```

Expected: `aggregate_mbps` decreasing from ~54 at N=1 to ~47 at
N≥13 (with the high-N plateau caveat in [Saturation-throughput sanity checks](#saturation-throughput-sanity-checks)). Pipe to a CSV and
re-render `handbook/figures/12-wireless/magrin-validation.png` via
`scripts/plot-wireless-figures.py`.

## Saturation-throughput sanity checks

The same example exposes a `--singleAcSaturation` flag that runs a
single STA, single AC, saturating UDP, no DiffServ — producing a
single-station throughput number that can be compared against
analytical or published targets for the chosen PHY/MCS. Two
calibration anchors are exercised:

**Bianchi 2000 / 802.11a OfdmRate6Mbps (default mode).**

```
single_ac_saturation,standard=80211a,heMcs=5,phy_mbps=6,measured_mbps=5.59
```

5.59 Mb/s out of a 6 Mb/s nominal PHY is ~93 % efficiency, well
within the 10 % tolerance Bianchi 2000 predicts for OFDM-6 saturation
throughput once 802.11 framing and ACK overhead are accounted for.
This is a sanity-grade check that the airtime-budget approximation in
the [Caveats](#caveats) caveat 1 is calibrated against a real number, not a guess.

**Magrin et al. WNS3 2021 Figure 3 sweep / 802.11ax HE-MCS 5
(`--standard=80211ax --heMcs=5 --numStas=N`).**

The Magrin / Avallone / Roy / Zorzi paper validates ns-3's 802.11ax
OFDMA implementation against an extended Bianchi-Bellalta analytical
model. Their Figure 3 sweep (Table 2 + Table 3: MCS 5, 20 MHz, 5 GHz,
N=1..19 STAs, BE, 100 Mb/s app-layer UDP per direction per flow,
1000 B payload, Friis propagation, single-user transmissions only,
bidirectional UL+DL traffic) reports aggregate throughput
**Su + Sd** that decreases with N at small CWmin due to UL/DL channel
contention. Reproducing that sweep in this example with the matching
parameters:

![Stratum aggregate UL+DL throughput vs Magrin et al. Figure 3
CWmin=15 curve, MCS 5, 20 MHz, 5 GHz, N=1..19 STAs.](figures/12-wireless/magrin-validation.png)

The two curves track within ~3 Mb/s for N ≤ 11 (where the analytical
prediction is most reliable), then diverge at higher N — the Stratum
curve plateaus around 47-49 Mb/s while the published curve continues
descending toward 44 Mb/s. The diagnostic feature is the per-direction
breakdown: at N=19 the Stratum DL falls to ~1.7 Mb/s while UL holds at
~43 Mb/s, the signature of single-MPDU AP transmissions losing to
N contending STAs at AC_BE EDCA parity. With proper A-MPDU
aggregation the AP would amortise contention overhead across many
MPDUs per TXOP and the curve would continue to descend smoothly.

The plateau therefore reflects a structural difference between the
pinned ns-3.48 revision (`d2add90b4`) and the
`signetlabdei/ofdma-validation` fork that drove the published
results. The following candidates were each ruled out by direct
experiment in this example (each held the other knobs at their
default and re-ran N=1, 11, 19):

- **QoS-enabled MAC.** The default mode runs `QosSupported=false` to
  isolate the qdisc-comparison demo. Forcing `QosSupported=true` only
  in the `--singleAcSaturation` branch produced bit-identical
  throughput at all three sweep points, confirmed by reading back
  `WifiMac::GetQosSupported()` post-install.
- **A-MPDU cap.** Setting `BE_MaxAmpduSize=6500631` (HE max, 6.5 MB)
  via `--ns3::WifiMac::BE_MaxAmpduSize=...` produced bit-identical
  throughput. Default value is 65535, the HT cap, but raising it
  changed nothing in this setup.
- **Block-Ack reorder buffer.** Setting `MpduBufferSize=256` (HE
  extended Block-Ack) had no effect; default is 64.

The bit-for-bit equality across these three configurations indicates
that A-MPDU aggregation is not being engaged in this single-user
setup at all, regardless of QoS state — most likely because Magrin
et al.'s fork drives traffic through OFDMA-specific paths
(`MultiUserScheduler`, `EnableUlOfdma`, `EnableBsrp`, see
`examples/wireless/wifi-he-network.cc` for the mainline analogue)
which the example's basic single-user `ConstantRateWifiManager` setup
does not exercise. Reproducing the descending portion of the published
curve requires porting the relevant author setup from
`https://github.com/signetlabdei/ofdma-validation` rather than
patching this example. The qualitative shape — saturated
single-STA throughput around 54 Mb/s, monotonic decrease through
N=11 — matches the published behaviour over the regime where
single-user aggregation is not the dominant effect.

The CWmin=127 and CWmin=1023 curves from Figure 3 are not reproduced
in this example; doing so would require setting EDCA AC_BE
contention-window overrides, which the example does not currently
expose.

Both PDFs live under `paper/related-papers/` (`wns3_2021_magrin.pdf`
slides; `wns3_2021_magrin_paper.pdf` proceedings paper); the full
author code is at `https://github.com/signetlabdei/ofdma-validation`
(a separate ns-3 fork driven by a Jupyter notebook, useful as a
multi-STA OFDMA reference if the [Scheduler comparison demo](#scheduler-comparison-demo) ever
extends in that direction).

Three parameters in the example are load-bearing for matching Magrin's
Figure 3 setup:

- **20 MHz channel pin** via `phy.Set("ChannelSettings", "{0, 20,
  BAND_5GHZ, 0}")`. Without it ns-3's default channel for
  `WIFI_STANDARD_80211ax` is wider than 20 MHz, the link ceases to
  be the bottleneck at 100 Mb/s offered, and the comparison loses
  meaning (measured throughput tracks the offered 100 Mb/s instead
  of the link's saturation rate).
- **1000 B UDP payload** (Magrin Table 3 `L_D = 1000`). Using a
  larger payload shifts measured throughput slightly upward.
- **Bidirectional UL+DL traffic** at saturation. Without UL the
  channel has no contention and DL throughput is N-invariant near
  the SU PHY ceiling; the published Figure 3 curve cannot be
  reproduced unidirectionally. The metric reported is aggregate
  Su + Sd, matching paper §6.

## Hybrid wired/wireless edge

`examples/diffserv-hybrid-wired-wireless.cc` puts the Stratum
edge at the AP downlink in a more realistic deployment shape: two
wired servers (a low-rate VoIP-like source and a high-rate bulk
source) feed a core router, which feeds the AP over a 100 Mb/s P2P
backhaul, which feeds four STAs over the same 802.11a 6 Mb/s link as
the [Scheduler comparison demo](#scheduler-comparison-demo). The AP runs the LLQ scheduler (PQ slot for EF, WFQ for the
remaining classes). The classifier and marking live **only** on the
AP downlink — the wired servers send unmarked packets, and the AP
stamps the DSCP based on destination STA. This is the residential /
SOHO QoS-on-Wi-Fi pattern: the bottleneck is on the wireless air,
so that is where the edge sits.

`--diffserv={true,false}` toggles the LLQ queue disc on or off. A
representative 8 s run:

| Class | rx kb/s (Stratum on) | p99 ms (Stratum on) | rx kb/s (Stratum off) | p99 ms (Stratum off) |
|---|---|---|---|---|
| EF   |  258 |  506 |  273 |  509 |
| AF41 | 2459 |  532 | 1599 |  660 |
| BE   | 1792 | 1423 | 2020 | 2423 |
| BK   |  885 | 1315 | 1506 | 2418 |

Two observations:

- **AF41 protection is the sharpest signal.** With Stratum on, AF41
  receives ~2.5 Mb/s at ~530 ms p99; with Stratum off it falls to
  ~1.6 Mb/s and starts to compete with BE/BK on equal footing. The
  WFQ-residual portion of LLQ is doing what it should.
- **EF is similarly low-rate in both modes** because at 300 kb/s it
  is far below any reasonable congestion threshold. The deep
  queue-disc backlog dominates its p99 in both modes; what changes is
  the *aggregate* shape of the residual classes around it.

The DSCP marking is observed indirectly through the per-class
differentiation: if the EF/AF41 marks were not surviving the wired
ingress → AP queue disc → Wi-Fi egress path, the LLQ scheduler at the AP
would see four indistinguishable flows and the table above would
show all four classes converging.

## Open follow-ups

- **Q-tier wireless scenario.** The [Saturation-throughput sanity checks](#saturation-throughput-sanity-checks) section establishes a single-point
  PHY/MAC calibration against Magrin et al. WNS3 2021 (within 3.3 %).
  A full Q-tier wireless spec with EF latency under BE saturation as
  the metric remains future work. Candidates beyond what is now
  exercised: multi-STA OFDMA scaling against Magrin's full N=1..19
  curve (slide 7); the contention-window sweep in Qayyum et al. 2024
  (PeerJ CS, Zenodo `10.5281/zenodo.14046465`); Banchs-Vollero 2006
  per-AC EDCA targets; Linux `tc-cake` over a real Wi-Fi 6 AP for
  qdisc-level validation. Each requires either a multi-point
  calibration sweep or a hardware testbed/trace dataset.
- **Magrin Figure 3 high-N descent.** The plateau divergence in [Saturation-throughput sanity checks](#saturation-throughput-sanity-checks) is
  structural (single-user A-MPDU isn't engaging in this setup at
  this ns-3 commit, regardless of QoS / BE_MaxAmpduSize /
  MpduBufferSize — confirmed by direct rule-out in that section).
  Closing the gap means porting the relevant single-user-aggregation
  knobs from `signetlabdei/ofdma-validation`, or accepting the
  divergence and using the fork directly for any extended N>11
  reproduction. Either path is a multi-day investigation that
  belongs in the dedicated wireless paper, not this chapter.
- **5G L4S.** ns-3 5G-LENA has experimental L4S support patches.
  An obvious extension is to point the L4S coupled-AQM (see [L4S](II-06-l4s-client.md))
  at a 5G-LENA bearer instead of a P2P link. Substrate-side: zero
  changes expected.
- **`NetDevice::GetL2OverheadBytes()` upstream MR.** A virtual
  accessor on the `NetDevice` base class would let
  `diffserv::Helper::DetectL2OverheadBytes()` return the correct value
  on Wi-Fi / LTE / 5G NR without per-device casting.
- **Companion paper (Andreozzi-Stratum-wireless v2).** Future work; the
  attachment pattern shown here is the kernel of that paper.

## References

- The demos: `examples/diffserv-wifi-demo.cc`,
  `examples/diffserv-wifi-scheduler-comparison.cc`,
  `examples/diffserv-hybrid-wired-wireless.cc`.
- ns-3 mainline 802.11e mapper:
  `ns-3.48/src/wifi/model/qos-utils.cc:116-119`.
- RFC 8325 — *Mapping Diffserv to IEEE 802.11* (Szigeti, Henry, Baker,
  2018).
- Bianchi, G. (2000). *Performance analysis of the IEEE 802.11
  distributed coordination function.* IEEE JSAC 18(3):535-547.
- The scheduler `L2OverheadBytes` attribute and the wire-byte basis
  it records are documented inline in
  `model/stratum-scheduler.h`.
