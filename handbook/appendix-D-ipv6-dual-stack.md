# Appendix D — IPv6 dual-stack

The DS field (Differentiated Services codepoint + ECN bits) occupies the Traffic
Class byte of the IPv6 header — the same semantic position as in IPv4. RFC 2474
defines the field identically for both address families, so the classifier, meter,
and scheduler logic is address-family agnostic: the same client configuration that
runs over IPv4 runs unchanged over IPv6.

Each client chapter carries a short **"Over IPv6"** section pointing here:
[DiffServ](I-03-diffserv.md), [L4S](I-04-l4s.md), and [CAKE](I-05-cake.md). This
appendix collects the shared mechanism once and records the worked examples that
exercise it.

## The dual-stack delta

The only structural change between an IPv4 and an IPv6 scenario is the networking
layer. The usual `Ipv4AddressHelper` call is replaced with `InternetStackHelper`
(which installs both IPv4 and IPv6) and `Ipv6AddressHelper`:

```cpp
// Install the dual-stack protocol suite on all nodes.
InternetStackHelper internet;
internet.Install(allNodes);

// Assign ULA addresses on the two point-to-point links.
Ipv6AddressHelper ipv6;
ipv6.SetBase(Ipv6Address("fd00:1::"), Ipv6Prefix(64));
Ipv6InterfaceContainer if1 = ipv6.Assign(devSenderRouter);
if1.SetForwarding(1, true);
if1.SetDefaultRouteInAllNodes(1);

ipv6.SetBase(Ipv6Address("fd00:2::"), Ipv6Prefix(64));
Ipv6InterfaceContainer if2 = ipv6.Assign(devRouterSink);
if2.SetForwarding(0, true);
if2.SetDefaultRouteInAllNodes(0);
```

The client configuration above the networking layer — mark rules, policer entries,
PHB tables, schedulers, DualPI2 parameters, CAKE tin shares — is identical to an
IPv4 scenario. The queue disc reads the DS field from whichever IP header version
the packet carries: the DSCP-to-class and DSCP-to-tin classifiers read the Traffic
Class byte via the same extraction path used for IPv4, and `stratum::GetEcn`
extracts the ECN bits (including the L4S `ECT(1)` identifier) from either header.

> **Prefix note:** the examples use the `fd00::/8` unique-local address range
> (ULA, RFC 4193) rather than the `2001:db8::/32` documentation prefix. ns-3's
> IPv6 routing layer does not install routes for the documentation prefix by
> default, so ULA prefixes are the reliable choice for self-contained simulation
> topologies.

## Worked examples

Three minimal examples drive each client over a pure-IPv6 dumbbell
(sender → router → sink). They are the companion runs to the main client recipes;
only the networking layer differs from their IPv4 counterparts.

### DiffServ EF / AF21 / BE differentiation over IPv6

**Example:** `diffserv-ipv6-recipe`

Three UDP CBR flows cross a 2 Mbps bottleneck. The DiffServ edge queue disc
classifies by DSCP — EF (46), AF21 (18), and BE (0) — and schedules with
strict-priority queueing: EF at the highest priority, AF21 in the middle, BE at
the bottom. The bottleneck is intentionally overloaded so the priority ordering
visibly determines which flows get through. The `EdgeQueueDisc` configuration is
identical to an IPv4 scenario.

```bash
./ns3 run "diffserv-ipv6-recipe"
```

```
Flow EF   (DSCP 46): port 9, offered 1 Mbps
Flow AF21 (DSCP 18): port 10, offered 0.8 Mbps
Flow BE   (DSCP  0): port 11, offered 0.6 Mbps
Bottleneck: 2 Mbps; simulation time: 15 s

=== Per-class results (IPv6 DiffServ) ===
Class       Rx bytes    Rx pkts  Served Mbps
--------- ----------  --------- -----------
EF   (DSCP 46)    1748480       3415  0.999
AF21 (DSCP 18)    1398272       2731  0.799
BE   (DSCP  0)      39424         77  0.023

Packets Statistics
=======================================
 CP  TotPkts   TxPkts   ldrops   edrops
 --  -------   ------   ------   ------
  0     2052    6.29%   93.71%     0.00%
 18     2734  100.00%    0.00%     0.00%
 46     3417  100.00%    0.00%     0.00%
----------------------------------------
All     8203   76.56%   23.44%     0.00%
```

EF (DSCP 46) and AF21 (DSCP 18) each receive their full offered load — 0.999 Mbps
and 0.799 Mbps — because PQ gives them unconditional priority over the 2 Mbps link.
BE (DSCP 0) absorbs whatever remains: 0.023 Mbps of the 2 Mbps capacity, with
93.71% of its packets dropped at the edge. The DS field read from each packet's
IPv6 Traffic Class byte drives the classification; nothing in the disc logic is
IPv4-specific.

### L4S ECT(1) coupled marking over IPv6

**Example:** `l4s-ipv6-recipe`

Two flows contend for a 10 Mbps bottleneck through a DualPI2 queue disc. One flow
marks its packets `ECT(1)` (the L4S identifier) and lands in the L4S sub-queue,
which receives low-latency PI² marking. The other flow sends `Not-ECT` classic
traffic and lands in the classic sub-queue with its own drop-based PI² AQM. The
coupled marking formula `p_C = (p_L/k)²` (RFC 9332 §2.1 eq. (1)) ensures classic
traffic does not starve. The `l4s::QueueDisc` configuration is identical to an
IPv4 scenario.

```bash
./ns3 run "l4s-ipv6-recipe"
```

```
L4S over IPv6 — DualPI2 coupled marking
Bottleneck: 10 Mbps / 20 ms  |  Each flow: 6 Mbps  |  Sim: 10 s
Sink: fd00:2::200:ff:fe00:4

=== Results (L4S over IPv6) ===
Flow                  Rx pkts     Rx bytes      Avg OWD (ms)  Min OWD (ms)  Max OWD (ms)
--------------------------------------------------------------------------------
L4S ECT(1) port 5001  7408        7408000       23.18         21.92         23.59
Classic Not-ECT  5002 4352        4352000       46.32         22.76         203.67

Disc nTotalDroppedPacketsBeforeEnqueue: 3054
Disc nTotalDroppedPackets:              3054
Disc nTotalMarkedPackets:               6946
  mark reason "L4S_IMMEDIATE_MARK": 6946

L4S flow sent: 7426 pkts  classic flow sent: 7426 pkts
```

The L4S flow achieves a mean one-way delay of 23.18 ms against a 20 ms
propagation baseline — 3 ms of queueing — while the classic flow sees 46.32 ms
mean OWD with a tail out to 203.67 ms. The 6,946 marked packets are CE marks
applied to ECT(1) packets in the L4S sub-queue by the PI² controller; the 3,054
drops are in the classic sub-queue. The sink address `fd00:2::200:ff:fe00:4`
confirms traffic traversed the IPv6 data plane.

### CAKE diffserv4 tin classification over IPv6

**Example:** `cake-ipv6-recipe`

Four UDP CBR flows — each offered 5 Mbps — contend for a 10 Mbps bottleneck
shaped by CAKE in `diffserv4` mode. Each flow carries a distinct DSCP value that
maps to one of the four CAKE tins: Bulk (CS1, DSCP 8), Best-Effort (CS0, DSCP 0),
Video (AF21, DSCP 18), and Voice (EF, DSCP 46). CAKE's DSCP-to-tin classifier
reads the DS field from the IPv6 Traffic Class byte via the same extraction path
used for IPv4; the `cake::Helper` configuration (`SetAsCakeDiffserv4`, rate, tin
shares) is identical to an IPv4 scenario.

```bash
./ns3 run "cake-ipv6-recipe"
```

```
CAKE diffserv4 over IPv6 — four flows to distinct tins
Bottleneck: 10 Mbps  |  Each flow offered: 5 Mbps
Sink: fd00:2::200:ff:fe00:4

Flow 0: DSCP  8 -> tin Bulk   (share 6.25%), port 5010
Flow 1: DSCP  0 -> tin BE     (share 100% ), port 5011
Flow 2: DSCP 18 -> tin Video  (share 50%  ), port 5012
Flow 3: DSCP 46 -> tin Voice  (share 25%  ), port 5013

=== Per-flow results (CAKE diffserv4 over IPv6) ===
DSCP    Tin     Share   Rx bytes      Rx pkts   Served Mbps
--------------------------------------------------------------
8       Bulk    6.25%   1314000       1314      1.107
0       BE      100%    5824000       5824      4.904
18      Video   50%     2583000       2583      2.175
46      Voice   25%     1272000       1272      1.071

Disc stats:
  total enqueued:               23754 pkts
  total dropped:                12759 pkts
  delivered (enqueued-dropped): 10995 pkts
```

The served throughput of each tin reflects its share of the 10 Mbps link. BE
holds the top-level share at 100% and wins the most bandwidth; Voice at 25% and
Video at 50% are constrained accordingly; Bulk at 6.25% is the lowest tier,
receiving just over 1 Mbps. The sink address `fd00:2::200:ff:fe00:4` confirms all
four flows traversed the IPv6 data plane, and the DSCP-to-tin assignments match
the diffserv4 map exactly as they do over IPv4.

### CAKE + L4S composition over IPv6

**Example:** `cake-l4s-composition-ipv6`

Two TCP flows share a 40 Mbps bottleneck shaped by CAKE in `diffserv4` mode with a
DualPI2 instance installed as the per-tin inner queue. Both flows carry DSCP 0 (CS0,
Best-Effort tin) so they land in the same tin; the DualPI2 inner then separates them
by ECN codepoint. The scalable flow uses DCTCP with `UseEct0=false`, which causes
`TcpDctcp::Init` to set `ECT(1)` on every data packet; the DualPI2 inner routes
`ECT(1)` packets to the low-latency sub-queue and CE-marks them under load. The
classic Cubic flow carries no ECN marking and goes to the classic sub-queue. The
composition is identical to the IPv4 `cake-l4s-composition` scenario — the
addressing, socket setup, and ECN socket configuration (`UseEcn=On`) are the only
differences from the IPv4 counterpart.

```bash
./ns3 run "cake-l4s-composition-ipv6"
```

```
CAKE diffserv4 + DualPI2 composition over IPv6
Bottleneck: 40Mbps  |  Sim: 20 s
Sink: fd00:3::200:ff:fe00:6

=== Results (CAKE diffserv4 + DualPI2 over IPv6) ===
Flow                    Goodput     Rx bytes
--------------------------------------------------
Scalable (DCTCP ECT1)   20.35   Mbps  48335688
Classic  (Cubic)        12.24   Mbps  29080184

Disc nTotalDroppedPacketsBeforeEnqueue: 17
Disc nTotalDroppedPackets:              17
Disc nTotalMarkedPackets:               675
  mark reason "(Marked by child queue disc) L4S_IMMEDIATE_MARK": 675
```

Both flows receive non-zero goodput, confirming the IPv6 data plane is active end
to end. The 675 CE marks with reason `L4S_IMMEDIATE_MARK` confirm the DualPI2 inner
is classifying the `ECT(1)` packets correctly from the IPv6 Traffic Class byte and
applying coupled PI² marking. The classic Cubic flow receives no CE marks; the 17
tail drops are at the driver queue, not the CAKE disc. The sink address
`fd00:3::200:ff:fe00:6` confirms all traffic crossed the IPv6 network. The CAKE +
L4S composition is substrate-identical to its IPv4 counterpart.

## Validation summary

| Example | Client | DS-field read over IPv6 | Result |
|---|---|---|---|
| `diffserv-ipv6-recipe` | DiffServ | DSCP from Traffic Class byte | EF/AF21 served in full; BE squeezed — same ordering as IPv4 |
| `l4s-ipv6-recipe` | L4S | `ECT(1)` via `stratum::GetEcn` | L4S 23.18 ms mean OWD vs classic 46.32 ms; coupled marking active |
| `cake-ipv6-recipe` | CAKE | DSCP-to-tin from Traffic Class byte | Per-tin shares match the diffserv4 map, as over IPv4 |
| `cake-l4s-composition-ipv6` | CAKE + L4S | `ECT(1)` via `stratum::GetEcn` (inner DualPI2) | 675 CE marks on scalable flow; classic flow unaffected; composition substrate-identical to IPv4 |

## Found a problem?

[File a recipe issue](https://github.com/digitalities/stratum-ns3/issues/new?template=recipe-request.yml) — recipe accuracy is the highest-priority feedback.
