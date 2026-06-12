# Migration Guide: DiffServ4NS (ns-2) to diffserv (ns-3)

This guide is for users familiar with the ns-2 DiffServ4NS module (Andreozzi, 2001)
who want to use the ns-3 port. It maps ns-2 Tcl commands to their ns-3 C++ equivalents
and explains the key architectural differences.

## Scheduler construction (ADR-0037, 2026-04-20)

All 10 `Scheduler` subclasses take configuration via ns-3 attributes,
not C++ constructor arguments. Any ns-2 porting recipe that previously
emitted

```cpp
auto s = CreateObject<FooScheduler>(numQueues, linkBandwidthBps[, extra]);
```

should now emit

```cpp
auto s = CreateObjectWithAttributes<FooScheduler>(
    "NumQueues",     UintegerValue(numQueues),
    "LinkBandwidth", DoubleValue(linkBandwidthBps),
    /* ...per-class attrs (see below)... */);
```

Full attribute list is in the module `CHANGELOG.md` under the 2026-04-20
entry. Key per-class attributes:

- `LlqScheduler::FqVariant` — `EnumValue<FqVariant>` selects
  `WFQ` / `WF2Qp` / `SCFQ` / `SFQ` for queues 1..N-1.
- `PriorityScheduler::WinLen` — TSW estimator window in seconds
  (default 1.0).
- `l4s::CoupledScheduler::L4sQueueIdx` + `BurstCap` — queue index
  reserved for L4S traffic, and the RFC 9332 §2.5.1 bounded-priority
  safeguard.

Per-queue weights stay behind the imperative `scheduler->SetParam(i, w)`
setter; they are not attribute-driven.

## 1. Quick reference table

| ns-2 Tcl | ns-3 C++ | Notes |
|---|---|---|
| `$ns simplex-link $e1 $core 2Mb 5ms dsRED/edge` | `CreateObject<EdgeQueueDisc>()` | Edge disc is created separately, installed on the TrafficControlLayer |
| `$ns simplex-link $core $e1 2Mb 5ms dsRED/core` | `CreateObject<CoreQueueDisc>()` | Same pattern as edge |
| `set qE1C [[$ns link $e1 $core] queue]` | `Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>()` | In ns-3, you hold the pointer directly |
| `$qE1C set numQueues_ 2` | `disc->SetNumQueues(2)` | |
| `$qE1C setNumPrec 0 2` | `disc->SetNumPrec(0, 2)` | |
| `$qE1C setQSize 0 30` | `disc->SetQueueLimit(0, 30)` | |
| `$qE1C setMREDMode DROP` | `disc->SetMredMode(MredMode::DROP_TAIL)` | Enum instead of string |
| `$qE1C addMarkRule 46 -1 [$dest(0) id] any any` | `helper.AddMarkRule(disc, 46, kAnyHost, dstAddr, kAnyProtocol, 0)` | Addresses are `int32_t` (raw IPv4 bits), not node IDs |
| `$qE1C addPolicyEntry 46 TokenBucket $cir $cbs` | `helper.AddTokenBucketPolicy(disc, 46, cirBps, cbsBytes)` | Rates in bits/s; one method per meter type |
| `$qE1C addPolicyEntry 46 srTCM $cir $cbs $ebs` | `helper.AddSrTcmPolicy(disc, 46, cirBps, cbsBytes, ebsBytes)` | |
| `$qE1C addPolicyEntry 46 trTCM $cir $cbs $pir $pbs` | `helper.AddTrTcmPolicy(disc, 46, cirBps, cbsBytes, pirBps, pbsBytes)` | |
| `$qE1C addPolicyEntry 46 TSW2CM $cir` | `helper.AddTsw2cmPolicy(disc, 46, cirBps)` | |
| `$qE1C addPolicyEntry 46 TSW3CM $cir $pir` | `helper.AddTsw3cmPolicy(disc, 46, cirBps, pirBps)` | |
| `$qE1C addPolicyEntry 0 Dumb` | `helper.AddDumbPolicy(disc, 0)` | |
| `$qE1C addPolicerEntry TokenBucket 46 48` | `helper.AddPolicerEntry(disc, PolicerType::TOKEN_BUCKET, 46, 48, 48)` | Explicit 3-colour args (initial, downgrade1, downgrade2) |
| `$qE1C addPolicerEntry Dumb 0 0` | `helper.AddPolicerEntry(disc, PolicerType::DUMB, 0, 0, 0)` | |
| `$qE1C addPHBEntry 46 0 0` | `helper.AddPhbEntry(disc, 46, 0, 0)` | Works on edge or core disc |
| `$qE1C configQ 0 0 30` | `helper.ConfigQueue(disc, 0, 0, 30.0, 30.0, 1.0)` | Explicit (thMin, thMax, maxP); tail-drop = thMin==thMax, maxP=1.0 |
| `$qE1C configQ 0 1 -1` | `helper.ConfigQueue(disc, 0, 1, 0.0, 0.0, 0.0)` | -1 becomes (0, 0, 0) = always drop out-of-profile |
| `$qE1C setSchedularMode PRI` | `disc->SetScheduler(CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(n)))` | Scheduler is a separate object (see below); WinLen defaults to 1.0 |
| `$qE1C setSchedularMode WFQ` | `disc->SetScheduler(CreateObjectWithAttributes<WfqScheduler>("NumQueues", UintegerValue(n), "LinkBandwidth", DoubleValue(bw)))` | |
| `$qE1C addQueueWeight 0 3` | `scheduler->SetParam(0, 3.0)` | Call on the scheduler before installing |
| `$qE1C printStats` | `disc->PrintStats()` | |
| `$qE1C printPolicyTable` | `disc->PrintPolicyTable()` (edge only) | |
| `$qE1C setMREDMode RIO-C 1` | `disc->SetMredMode(MredMode::RIO_C, 1)` | Per-queue mode; omit queue index to set all |
| `$qE1C meanPktSize 1300` | `disc->SetMeanPacketSize(1300)` | Applies to all sub-queues |
| `$qE1C setQueueBW 1 1000000` | `disc->SetQueueBandwidth(1, 1000000.0)` | Per-queue link bandwidth for RED ptc |
| `$qE1C getVirtQueueLen 1 0` | `disc->GetVirtualQueueLen(1, 0)` | (queue, prec) → virtual queue packet count |
| `$qE1C getDepartureRate 0` | `disc->GetScheduler()->GetDepartureRate(0, -1)` | Returns bits/s |
| `$qE1C getQueueLen 0` | `disc->GetQueueDiscClass(0)->GetQueueDisc()->GetNPackets()` | Via ns-3 QueueDisc API |
| `$qE1C addMarkRule 10 -1 -1 any telnet` | `helper.AddMarkRuleWithPorts(disc, 10, kAnyHost, kAnyHost, kAnyProtocol, kAnyAppType, kAnyPort, 23)` | App-type → port-based classification |

## 2. Key differences

### No Tcl: all configuration is C++

The ns-2 module was configured entirely through OTcl `command()` dispatch, parsing
string arguments from Tcl scripts. The ns-3 port replaces this with typed C++ methods
on `diffserv::Helper` and the queue disc classes. There is no Tcl layer.

### Namespace

Substrate classes live inside `ns3::stratum`; client-specific classes live in
the nested namespaces `ns3::stratum::diffserv` (policy classifiers + helper),
`ns3::stratum::l4s`, and `ns3::stratum::cake`. Avoid blanket
`using namespace` directives — several class names (e.g. `RedQueueDisc`,
`OnOffApplication`) deliberately mirror mainline ns-3 names and are
disambiguated by the namespace:

```cpp
using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
```

### Meters are standalone objects

In ns-2, the meter type was a string (`"TokenBucket"`, `"srTCM"`, ...) stored in
a policy table entry. In ns-3, each meter is a separate `Meter` subclass
(`TokenBucketMeter`, `SrTcmMeter`, `TrTcmMeter`, `Tsw2cmMeter`, `Tsw3cmMeter`,
`FWMeter`) that inherits from `ns3::stratum::Meter`. The helper methods
(`AddTokenBucketPolicy`, `AddSrTcmPolicy`, etc.) create the correct meter internally.

### Scheduler is a separate strategy object (ADR-0012)

In ns-2, scheduling state (`queueWeight[]`, `wirrTemp[]`, etc.) lived directly on
`dsREDQueue`, and the scheduler mode was set via `setSchedularMode <STRING>`.

In ns-3, each scheduler is a standalone class inheriting from `Scheduler`:

| ns-2 mode string | ns-3 class |
|---|---|
| `PRI` | `PriorityScheduler` |
| `RR` | `RoundRobinScheduler` |
| `WRR` | `WeightedRoundRobinScheduler` |
| `WIRR` | `WeightedInterleavedRoundRobinScheduler` |
| `WFQ` | `WfqScheduler` |
| `SCFQ` | `ScfqScheduler` |
| `SFQ` | `SfqScheduler` |
| `WF2Qp` | `Wf2qPlusScheduler` |
| (composite) | `LlqScheduler` |

Create the scheduler, configure weights, then install:

```cpp
auto wfq = CreateObjectWithAttributes<WfqScheduler>( // 2 queues, 2 Mbps link
    "NumQueues",     UintegerValue(2),
    "LinkBandwidth", DoubleValue(2000000.0));
wfq->SetParam(0, 3.0);   // queue 0 weight
wfq->SetParam(1, 17.0);  // queue 1 weight
disc->SetScheduler(wfq);
```

### Per-packet metadata uses ns-3 Tags

ns-2 used `hdr_flags` and custom packet header fields for application type and
send-time metadata. ns-3 uses packet tags:

| ns-2 | ns-3 |
|---|---|
| `hdr_flags::fid_` / `class_` | `AppTypeTag` (for app-type classification) |
| (no equivalent) | `SendTimeTag` (for OWD measurement) |
| `hdr_ip::flowid()` | `DscpTag` (carries DSCP through the disc pipeline) |

### Addresses are IPv4, not node IDs

ns-2 mark rules used integer node IDs for address matching (`[$dest(0) id]`).
ns-3 uses raw IPv4 address bits (`destAddr.Get()` returns `uint32_t`):

```cpp
// ns-2: $qE1C addMarkRule 46 -1 [$dest(0) id] any any
// ns-3:
Ipv4Address destAddr = dstIfs[0].GetAddress(1);
helper.AddMarkRule(edgeDisc, 46, kAnyHost,
                   static_cast<int32_t>(destAddr.Get()),
                   kAnyProtocol, 0);
```

### Packet size includes headers (ADR-0019)

In ns-2.29, `hdr_cmn::size()` returned the application payload size only --
protocol headers did not consume link bandwidth. In ns-3, `Packet::GetSize()`
returns the full IP packet (payload + UDP/TCP header + IPv4 header). This means:

- A 512-byte CBR payload becomes a 540-byte packet on the wire (512 + 8 UDP + 20 IPv4).
- Fair-queueing schedulers (WFQ, SCFQ, SFQ, WF2Q+) compute finish times using the
  larger size, which changes bandwidth allocation at tight margins.

The ns-2.35 port in this repository adds the missing 28 bytes at UDP send time
(see BUG-5 in `docs/HISTORICAL_BUGS.md`), so both simulators now agree on the
same wire-byte semantics. ns-3's meters and schedulers therefore meter on
`QueueDiscItem::GetSize()` directly -- no compatibility shim is needed.

Priority scheduling (PQ) is unaffected regardless because it does not use packet
sizes for scheduling decisions.

### Structural OWD offset from NetDevice queue (ADR-0016)

ns-3's PointToPoint model has a mandatory 1-packet NetDevice queue between the
queue disc and the link. ns-2 has no equivalent -- the queue output goes directly
to the link. This adds roughly one packet's serialisation time per hop (~2 ms at
2 Mbps with 512-byte packets), producing a ~14% OWD divergence under PQ scheduling.
This is a structural cross-simulator difference, not a port defect.

Set the device queue to 1 packet (the minimum) to minimise the effect:

```cpp
p2pHelper.SetQueue("ns3::DropTailQueue<Packet>",
                   "MaxSize", StringValue("1p"));
```

### Installation order matters

The edge/core disc must be fully configured **before** `Initialize()` is called.
In ns-3, `Initialize()` creates child sub-queues using `m_numQueues`, so setting
`SetNumQueues()` after initialisation has no effect. The recommended pattern:

1. Create the disc: `CreateObject<EdgeQueueDisc>()`
2. Configure: `SetNumQueues`, `SetNumPrec`, `SetQueueLimit`, `SetMredMode`, `SetScheduler`, mark rules, policies, policers, PHB entries
3. Install on the TrafficControlLayer
4. Call `Initialize()`
5. Configure RED thresholds (sub-queues now exist): `ConfigQueue`

## 3. Configuration walkthrough: side-by-side

### ns-2 Tcl (simulation-1.tcl excerpt)

```tcl
# Create edge link with DiffServ queue
$ns simplex-link $e1 $core 2Mb 5ms dsRED/edge
set qE1C [[$ns link $e1 $core] queue]

# 2 queues: EF (2 precedence levels) and BE (1 precedence level)
$qE1C set numQueues_ 2
$qE1C setNumPrec 0 2
$qE1C setNumPrec 1 1
$qE1C setSchedularMode PRI
$qE1C setQSize 0 30
$qE1C setQSize 1 50
$qE1C setMREDMode DROP

# Classification
$qE1C addMarkRule 46 -1 [$dest(0) id] any any
$qE1C addMarkRule  0 -1 [$dest(1) id] any any

# Metering and policing
$qE1C addPolicyEntry  46 TokenBucket 300000 513
$qE1C addPolicyEntry  48 Dumb
$qE1C addPolicyEntry   0 Dumb
$qE1C addPolicerEntry TokenBucket 46 48
$qE1C addPolicerEntry Dumb 0 0

# PHB table
$qE1C addPHBEntry 46 0 0
$qE1C addPHBEntry 48 0 1
$qE1C addPHBEntry  0 1 0

# RED thresholds (tail-drop mode)
$qE1C configQ 0 0 30
$qE1C configQ 0 1 -1
$qE1C configQ 1 0 50
```

### ns-3 C++ equivalent

```cpp
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-pq-scheduler.h"

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::PriorityScheduler;

// Step 1: Create edge disc
Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
diffserv::Helper helper;

// Step 2: Configure BEFORE Initialize()
edgeDisc->SetNumQueues(2);
edgeDisc->SetNumPrec(0, 2);       // EF: 2 precedence levels
edgeDisc->SetNumPrec(1, 1);       // BE: 1 precedence level
edgeDisc->SetQueueLimit(0, 30);   // EF queue: 30 packets
edgeDisc->SetQueueLimit(1, 50);   // BE queue: 50 packets
edgeDisc->SetMredMode(MredMode::DROP_TAIL);

// Scheduler
Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>(
    "NumQueues", UintegerValue(2),
    "WinLen",    DoubleValue(1.0));
edgeDisc->SetScheduler(pq);

// Mark rules (note: IPv4 address bits, not node IDs)
Ipv4Address destAddr0 = dstIfs[0].GetAddress(1);
Ipv4Address destAddr1 = dstIfs[1].GetAddress(1);
helper.AddMarkRule(edgeDisc, 46, kAnyHost,
                   static_cast<int32_t>(destAddr0.Get()), kAnyProtocol, 0);
helper.AddMarkRule(edgeDisc, 0, kAnyHost,
                   static_cast<int32_t>(destAddr1.Get()), kAnyProtocol, 0);

// Policy entries (rates in bits/s)
helper.AddTokenBucketPolicy(edgeDisc, 46, 300000.0, 513.0);
helper.AddDumbPolicy(edgeDisc, 48);
helper.AddDumbPolicy(edgeDisc, 0);

// Policer entries
helper.AddPolicerEntry(edgeDisc, PolicerType::TOKEN_BUCKET, 46, 48, 48);
helper.AddPolicerEntry(edgeDisc, PolicerType::DUMB, 0, 0, 0);

// PHB table
helper.AddPhbEntry(edgeDisc, 46, 0, 0);   // EF in-profile  -> queue 0, prec 0
helper.AddPhbEntry(edgeDisc, 48, 0, 1);   // EF out-of-profile -> queue 0, prec 1
helper.AddPhbEntry(edgeDisc, 0, 1, 0);    // BE -> queue 1, prec 0

// Step 3: Install on TrafficControlLayer and initialise
Ptr<TrafficControlLayer> tc = device->GetNode()->GetObject<TrafficControlLayer>();
tc->SetRootQueueDiscOnDevice(device, edgeDisc);
edgeDisc->Initialize();

// Step 4: RED thresholds (after Initialize -- sub-queues now exist)
helper.ConfigQueue(edgeDisc, 0, 0, 30.0, 30.0, 1.0);  // EF: tail-drop at 30
helper.ConfigQueue(edgeDisc, 0, 1, 0.0, 0.0, 0.0);    // EF out: always drop
helper.ConfigQueue(edgeDisc, 1, 0, 50.0, 50.0, 1.0);   // BE: tail-drop at 50
```

## 4. What is NOT ported (and why)

### `dsAgent` / `Agent/LossMonitor`

The ns-2 `dsAgent` (UDP sending agent) and `Agent/LossMonitor` (loss/delay monitor)
are replaced by standard ns-3 applications:

| ns-2 | ns-3 replacement |
|---|---|
| `Agent/UDP` + `Application/Traffic/CBR` | `OnOffApplication` (constant on-time, zero off-time) or the module's tag-stamping `stratum::OnOffApplication` |
| `Agent/LossMonitor` | `PacketSink` + Rx trace callback with `SendTimeTag` |
| `$Sink_(0) set sumOwd_` / `sumIpdv_` | Compute in Rx callback: `Simulator::Now() - tag.GetSendTime()` |
| `$Sink_(0) FrequencyDistribution ...` | Use ns-3 data collection framework or custom file output |

### `printCoreStats`

The ns-2 `printCoreStats` method is replaced by `Statistics` objects with
traced callbacks. Use `disc->PrintStats()` for console output matching the ns-2 format,
or connect to the TracedCallbacks for programmatic access.

### Tcl `command()` dispatcher

The ns-2 OTcl `command()` dispatcher that parsed string arguments like
`"addPolicyEntry"`, `"setSchedularMode"`, etc. is completely eliminated. All
configuration is through typed C++ methods on `diffserv::Helper` and the queue disc
classes. There is no runtime string parsing.

### `gnuplot.tcl` / `gnuplot-x.tcl`

The ns-2 inline gnuplot generation scripts are not ported. The ns-3 example writes
trace files in the same two-column format (`time value`), which can be plotted with
any tool (gnuplot, matplotlib, etc.).

## 5. Further reading

- `docs/PORTING_MAP.md` -- class-by-class port mapping
- Architectural decision records (ADR-0012 through ADR-0020)
- `specs/` -- tiered spec suite (intent, structural, quality)
- `examples/diffserv-example-1.cc` -- complete working example
- `provenance/Andreozzi-2001-thesis.pdf` -- original 2001 thesis (Chapter 3.3.3)
