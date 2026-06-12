/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * diffserv-l4s-fqcodel-comparison.cc
 *
 * Compare DiffServ4NS L4S behaviour against mainline queue disc baselines
 * on the same bottleneck scenario. Selectable via
 * --mode={l4s-wred, l4s-coupled-only, l4s-fqcodel-classic, fqcodel,
 *         classic-only}.
 *
 * Topology (shared bottleneck, dumbbell):
 *
 *   probe-sender   ─── 100 Mbps / 1 ms ─┐
 *   bulk-sender-0  ─── 100 Mbps / 1 ms ─┤
 *   bulk-sender-1  ─── 100 Mbps / 1 ms ─┤
 *   ...                                   ├── router ─── 10 Mbps / 5 ms ─── receiver
 *   bulk-sender-N  ─── 100 Mbps / 1 ms ─┘
 *                                          (bottleneck egress carries the
 *                                           queue disc under test)
 *
 * Traffic mix:
 *   - One UDP CBR probe (ECT(1), ~500 kbps) with SendTimeTag.
 *     UDP is required so that packet boundaries are preserved end-to-end
 *     and the tag survives to the receiver's Rx callback (TCP strips tags).
 *   - N BulkSendApplication senders. Default (--mixedTraffic=false): all N
 *     use TcpDctcp ECT(1) (L4S lane). With --mixedTraffic=true: the first
 *     floor(N/2) senders use TcpCubic ECT(0) (classic lane) and the
 *     remaining senders use TcpDctcp ECT(1) (L4S lane). The probe always
 *     stays on the L4S lane regardless of this flag.
 *
 * Per-mode output layout:
 *   <outDir>/<mode>/N<bulkSenders>/probe-owd.csv          — per-packet OWD (t_us, owd_ms)
 *   <outDir>/<mode>/N<bulkSenders>/bulk-goodput.csv       — per-flow bulk goodput, RX-side
 * (flow_id, goodput_mbps) <outDir>/<mode>/N<bulkSenders>/coupling-counters.csv  — 1 Hz DualPI2
 * state (t_us, p_prime, p_C, coupledDropCount) emitted for l4s-* modes only; absent for fqcodel /
 * classic-only
 *
 * Mixed-traffic cell (--mixedTraffic=true) uses a separate subdirectory:
 *   <outDir>/<mode>/mixed/probe-owd.csv
 *   <outDir>/<mode>/mixed/bulk-goodput.csv
 *   <outDir>/<mode>/mixed/coupling-counters.csv
 *
 * What this example shows:
 *
 *   1. l4s-wred           — l4s::QueueDisc with the default classic AQM
 *                           (parent's WRED early-drop). Classic flow
 *                           sees coupled p_C drops + RIO/WRED early
 *                           drops.
 *   2. l4s-coupled-only   — l4s::QueueDisc with ClassicAqm::CoupledOnly.
 *                           Classic sub-queue is auto-configured to
 *                           pass-through FIFO; only coupled p_C and
 *                           tail drops fire.
 *   3. l4s-fqcodel-classic — l4s::QueueDisc with ClassicAqm::FqCoDel.
 *                           Classic path is a mainline FqCoDelQueueDisc
 *                           (flow fair queueing + CoDel target); L4S
 *                           lane still uses the DualPI2 P.I controller
 *                           + coupled drop.
 *   4. fqcodel            — Mainline FqCoDelQueueDisc on the bottleneck.
 *                           No DiffServ4NS at all — used as the
 *                           architectural baseline.
 *   5. classic-only       — Mainline FifoQueueDisc on the bottleneck.
 *                           No AQM — used as the no-AQM reference.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/fq-codel-queue-disc.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/tcp-dctcp.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("DiffServL4sFqCoDelComparison");

namespace
{

// ---------------------------------------------------------------------------
// Mode factory: produce the queue disc selected by --mode=...
// ---------------------------------------------------------------------------

enum class Mode
{
    L4sWred,
    L4sCoupledOnly,
    L4sFqCoDelClassic, // substrate puts FqCoDel on the classic side
    FqCoDel,
    ClassicOnly // mainline FifoQueueDisc, no AQM
};

static Mode
ParseMode(const std::string& s)
{
    if (s == "l4s-wred")
    {
        return Mode::L4sWred;
    }
    if (s == "l4s-coupled-only")
    {
        return Mode::L4sCoupledOnly;
    }
    if (s == "l4s-fqcodel-classic")
    {
        return Mode::L4sFqCoDelClassic;
    }
    if (s == "fqcodel")
    {
        return Mode::FqCoDel;
    }
    if (s == "classic-only")
    {
        return Mode::ClassicOnly;
    }
    NS_ABORT_MSG("unknown mode: " << s);
    return Mode::L4sWred; // unreachable
}

static std::string
ModeToString(Mode m)
{
    switch (m)
    {
    case Mode::L4sWred:
        return "l4s-wred";
    case Mode::L4sCoupledOnly:
        return "l4s-coupled-only";
    case Mode::L4sFqCoDelClassic:
        return "l4s-fqcodel-classic";
    case Mode::FqCoDel:
        return "fqcodel";
    case Mode::ClassicOnly:
        return "classic-only";
    }
    NS_ABORT_MSG("unhandled Mode value");
    return ""; // unreachable
}

// ---------------------------------------------------------------------------
// Coupling-counters sampler state (l4s-* modes only).
//
// Declared here, before InstallSelectedMode, so that the function can capture
// the l4s::QueueDisc pointer and connect the TraceSource.
//
// g_l4sQdisc is set inside InstallSelectedMode for the three l4s-* modes and
// remains nullptr for fqcodel and classic-only (substrate absent).
//
// g_coupledDropCount is incremented by OnCoupledDrop, connected to the
// l4s::QueueDisc "ClassicCoupledDrop" TraceSource.  The TraceSource fires once
// per packet dropped by the coupled p_C mechanism; item and pC are not needed
// here (only the count matters).
//
// SampleCouplingState fires at 1 Hz and appends a row to g_couplingSamples.
// ---------------------------------------------------------------------------

struct CouplingSample
{
    double tSec;
    double pPrime;
    double pC;
    uint64_t coupledDropCount;
};

static Ptr<l4s::QueueDisc> g_l4sQdisc;
static uint64_t g_coupledDropCount = 0;
static std::vector<CouplingSample> g_couplingSamples;

// TraceSource trampoline — connected to l4s::QueueDisc "ClassicCoupledDrop".
// MakeBoundCallback does not accept lambdas; this free function serves as the
// trampoline.  Signature must match ClassicCoupledDropTracedCallback.
void
OnCoupledDrop(Ptr<const QueueDiscItem> /*item*/, double /*pC*/)
{
    ++g_coupledDropCount;
}

void
SampleCouplingState()
{
    if (!g_l4sQdisc)
    {
        return;
    }
    CouplingSample s;
    s.tSec = Simulator::Now().GetSeconds();
    s.pPrime = g_l4sQdisc->GetBaseProb();
    s.pC = g_l4sQdisc->GetLastClassicCoupledProb();
    s.coupledDropCount = g_coupledDropCount;
    g_couplingSamples.push_back(s);
    Simulator::Schedule(Seconds(1.0), &SampleCouplingState);
}

// Install the selected queue disc on a NetDeviceContainer (the bottleneck
// device). Returns the installed disc as a Ptr<QueueDisc> for later
// stats inspection.
static Ptr<QueueDisc>
InstallSelectedMode(NetDeviceContainer bottleneck, Mode mode)
{
    TrafficControlHelper tch;

    if (mode == Mode::FqCoDel)
    {
        tch.SetRootQueueDisc("ns3::FqCoDelQueueDisc");
        QueueDiscContainer discs = tch.Install(bottleneck);
        return discs.Get(0);
    }

    if (mode == Mode::ClassicOnly)
    {
        tch.SetRootQueueDisc("ns3::FifoQueueDisc", "MaxSize", StringValue("1000p"));
        QueueDiscContainer discs = tch.Install(bottleneck);
        return discs.Get(0);
    }

    // L4S modes: install l4s::QueueDisc with a 2-child composition
    // (L4S FIFO at child idx 0, classic AQM at child idx 1). The
    // specific inner classic AQM depends on `mode`:
    //   - L4sWred            -> default stratum::RedQueueDisc with WRED
    //   - L4sCoupledOnly     -> stratum::RedQueueDisc auto-munged to pass-through
    //   - L4sFqCoDelClassic  -> mainline FqCoDelQueueDisc on classic side
    Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
    disc->SetL4sQueueIdx(1);

    const bool redInner = (mode != Mode::L4sFqCoDelClassic);

    if (redInner)
    {
        disc->SetNumQueues(2);
        if (mode == Mode::L4sCoupledOnly)
        {
            disc->SetClassicAqm(l4s::QueueDisc::ClassicAqm::CoupledOnly);
        }
        disc->SetQueueLimit(0, 200);
        disc->AddPhbEntry(0, 0, 0); // DSCP 0 (default) -> queue 0 prec 0
    }
    else
    {
        // FqCoDel classic: pre-build an FqCoDelQueueDisc with an explicit
        // quantum and inject it via the composition injection point.
        // FqCoDel's on-device path reads the quantum from the owning
        // NetDevice's MTU, but here the NetDevice is aggregated to
        // the outer l4s::QueueDisc (not to the child), so the inner
        // needs its quantum set directly.
        Ptr<FqCoDelQueueDisc> fq = CreateObject<FqCoDelQueueDisc>();
        fq->SetQuantum(1500);
        disc->SetClassicAqmDisc(fq);
    }

    // L4S FIFO sizing is common to all modes.
    disc->SetQueueLimit(1, 200);

    // Coupled scheduler keeps classic from starving under heavy L4S load.
    Ptr<l4s::CoupledScheduler> sched =
        CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                          UintegerValue(2),
                                                          "L4sQueueIdx",
                                                          UintegerValue(1),
                                                          "BurstCap",
                                                          UintegerValue(8));
    disc->SetScheduler(sched);

    // Manually attach: ns-3 lets us pre-build a disc and have the
    // TrafficControlHelper install it as root.
    bottleneck.Get(0)->AggregateObject(disc);
    Ptr<TrafficControlLayer> tcl = bottleneck.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
    if (tcl)
    {
        tcl->SetRootQueueDiscOnDevice(bottleneck.Get(0), disc);
    }
    disc->Initialize();

    if (redInner)
    {
        // L4S queue (queue 1) needs RED thresholds wide enough that
        // nothing accidentally fires WRED on the L4S side.
        disc->ConfigQueue(1, 0, 100.0, 200.0, 0.1);

        // Shape-C calibration: all bulks are ECT(1) and route to the L4S
        // sub-queue. The classic sub-queue carries only sparse TCP control
        // traffic (SYN / SYN-ACK / FIN / ACK during pre-ECN-negotiation
        // window). Aggressive RED on classic would drop SYN packets and
        // prevent TCP handshakes from completing. Disable proactive RED
        // on classic (effectively drop-tail at queue limit) and widen the
        // classic queue to absorb burst control traffic at high N.
        disc->SetQueueLimit(0, 2000);
        if (mode == Mode::L4sWred)
        {
            // RED is effectively inactive: MinTh ≈ MaxTh = QueueLimit - 1,
            // MaxProb = 0.0 — RED only drops at queue-full (drop-tail).
            disc->ConfigQueue(0, 0, 1999.0, 1999.0, 0.0);
        }
        else if (mode == Mode::L4sCoupledOnly)
        {
            // CoupledOnly also needs the relaxed classic-RED so SYN
            // packets aren't pre-dropped before TCP can switch to ECT(1).
            disc->ConfigQueue(0, 0, 1999.0, 1999.0, 0.0);
        }
    }
    // For the l4s-fqcodel-classic mode the L4S lane is a FifoQueueDisc
    // (FIFO, no WRED thresholds to configure) and the classic inner
    // is an FqCoDelQueueDisc with its own Target / Interval defaults
    // (5 ms / 100 ms per the FqCoDel attribute defaults).

    // Capture the l4s::QueueDisc pointer so the 1 Hz sampler can read p' and
    // p_C, and connect the ClassicCoupledDrop TraceSource so OnCoupledDrop can
    // maintain the running drop counter.  Both are done here, after Initialize()
    // has wired up the disc children, so the TraceSource is live by the time
    // Simulator::Run() fires the first sampler event.
    g_l4sQdisc = disc;
    disc->TraceConnectWithoutContext("ClassicCoupledDrop", MakeCallback(&OnCoupledDrop));

    return disc;
}

// ---------------------------------------------------------------------------
// UDP CBR probe application that stamps SendTimeTag on every packet
// before Send(). UDP is required so that packet boundaries are preserved
// end-to-end and the tag survives to the receiver's Rx callback (TCP
// segmentation strips per-packet tags before delivery). The TOS byte is
// set to ECT(1) so the L4S classifier routes the probe to the L4S
// sub-queue in L4S modes; FqCoDel and FIFO treat it as any other flow.
// ---------------------------------------------------------------------------

class UdpProbeApp : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("DiffServL4sFqCoDelUdpProbeApp")
                                .SetParent<Application>()
                                .AddConstructor<UdpProbeApp>();
        return tid;
    }

    void Setup(Address remote, uint32_t pktSize, uint64_t bps, uint8_t tos)
    {
        m_remote = remote;
        m_pktSize = pktSize;
        m_bps = bps;
        m_tos = tos;
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), TypeId::LookupByName("ns3::UdpSocketFactory"));
        m_socket->SetIpTos(m_tos);
        m_socket->Bind();
        m_socket->Connect(m_remote);
        m_running = true;
        SendOne();
    }

    void StopApplication() override
    {
        m_running = false;
        Simulator::Cancel(m_event);
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void SendOne()
    {
        if (!m_running)
        {
            return;
        }
        Ptr<Packet> p = Create<Packet>(m_pktSize);
        SendTimeTag tag(Simulator::Now().GetSeconds());
        p->AddPacketTag(tag);
        m_socket->Send(p);
        double gap = static_cast<double>(m_pktSize * 8) / static_cast<double>(m_bps);
        m_event = Simulator::Schedule(Seconds(gap), &UdpProbeApp::SendOne, this);
    }

    Address m_remote;
    Ptr<Socket> m_socket;
    uint32_t m_pktSize{1000};
    uint64_t m_bps{500000};
    uint8_t m_tos{0};
    bool m_running{false};
    EventId m_event;
};

// ---------------------------------------------------------------------------
// Global state for per-packet probe OWD CSV.
// ---------------------------------------------------------------------------

static std::ofstream g_probeOwdCsv;

// ---------------------------------------------------------------------------
// Per-flow RX byte tracking for bulk goodput CSV.
//
// RX-side accounting: bytes counted at the PacketSink receiver. This
// measures what was actually delivered to the application layer, making
// it accurate even in lossy regimes (classic-only mode) where a TX-side
// count would overstate goodput by including retransmitted segments.
// ---------------------------------------------------------------------------

struct PerFlowState
{
    uint64_t rxBytes = 0;
};

static std::vector<PerFlowState> g_perFlow;

// Warmup cutoff set in main() before Simulator::Run(). OnBulkRx ignores
// packets timestamped before this threshold so that slow-start bytes
// during the ramp-up phase do not inflate per-flow goodput estimates.
static double g_warmupSec = 0.0;

// Free function required by MakeBoundCallback (lambdas are not accepted).
// flowId is bound at connect time; p and from are supplied by the Rx
// trace source inherited by PacketSink from SinkApplication.
void
OnBulkRx(uint32_t flowId, Ptr<const Packet> p, const Address& /*from*/)
{
    if (Simulator::Now().GetSeconds() < g_warmupSec)
    {
        return;
    }
    g_perFlow[flowId].rxBytes += p->GetSize();
}

void
OnProbeRx(Ptr<const Packet> p, const Address& /*from*/)
{
    SendTimeTag tag;
    if (!p->PeekPacketTag(tag))
    {
        return; // not a tagged probe packet — guard against spurious Rx
    }
    double now = Simulator::Now().GetSeconds();
    double owd = now - tag.GetSendTime();
    g_probeOwdCsv << static_cast<int64_t>(Simulator::Now().GetMicroSeconds()) << "," << owd * 1000.0
                  << "\n";
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string modeSel = "l4s-wred";
    std::string outputDir = ".";
    double simTime = 5.0;
    double warmup = 10.0;
    uint32_t bulkSenders = 2;
    bool mixedTraffic = false;
    uint32_t probePktSize = 1000;
    uint32_t probeKbps = 500;
    uint64_t bottleneckRateBps = 10000000;

    CommandLine cmd(__FILE__);
    cmd.AddValue("mode",
                 "AQM composition: l4s-wred | l4s-coupled-only | "
                 "l4s-fqcodel-classic | fqcodel | classic-only",
                 modeSel);
    cmd.AddValue("output", "Output directory (absolute or relative to cwd)", outputDir);
    cmd.AddValue("simTime", "Simulation duration (seconds)", simTime);
    cmd.AddValue("warmup", "Warmup seconds excluded from goodput measurement window", warmup);
    cmd.AddValue("bulkSenders",
                 "Number of bulk senders (ECT(1) TcpDctcp, or mixed if "
                 "--mixedTraffic=true)",
                 bulkSenders);
    cmd.AddValue("mixedTraffic",
                 "If true, half the bulks are TcpCubic ECT(0) (classic-lane); "
                 "the other half stay TcpDctcp ECT(1) (L4 lane).",
                 mixedTraffic);
    cmd.AddValue("probePktSize", "UDP probe packet size (bytes)", probePktSize);
    cmd.AddValue("probeKbps", "UDP probe rate (kbps)", probeKbps);
    cmd.AddValue("bottleneck", "Bottleneck rate (bps)", bottleneckRateBps);
    cmd.Parse(argc, argv);

    Mode mode = ParseMode(modeSel);

    // Resolve output directory and create per-mode cell directory.
    // Mixed-traffic runs land in <mode>/mixed/ to avoid colliding with the
    // main sweep cells at <mode>/N<bulkSenders>/.
    std::filesystem::path outDir = std::filesystem::absolute(outputDir);
    const std::string subdir = mixedTraffic ? "mixed" : ("N" + std::to_string(bulkSenders));
    std::filesystem::path cellDir = outDir / ModeToString(mode) / subdir;
    std::filesystem::create_directories(cellDir);

    // Open per-packet probe OWD CSV.
    std::filesystem::path owdPath = cellDir / "probe-owd.csv";
    g_probeOwdCsv.open(owdPath);
    NS_ABORT_MSG_UNLESS(g_probeOwdCsv.is_open(), "Failed to open probe-owd.csv at " << owdPath);
    g_probeOwdCsv << "t_us,owd_ms\n";

    // ECN must be enabled globally so that DCTCP can negotiate ECN and so that
    // the ECT(1) / ECT(0) codepoints are honoured by the IP stack.
    Config::SetDefault("ns3::TcpSocketBase::UseEcn", EnumValue(TcpSocketState::On));

    // Topology: (bulkSenders + 1) senders — router — receiver.
    // Node index layout:
    //   0          : UDP probe sender
    //   1 .. N     : TcpDctcp bulk senders  (N = bulkSenders)
    //   N+1        : router
    //   N+2        : receiver
    uint32_t routerIdx = bulkSenders + 1;
    uint32_t receiverIdx = bulkSenders + 2;
    uint32_t totalNodes = bulkSenders + 3;

    NodeContainer allNodes;
    allNodes.Create(totalNodes);

    Ptr<Node> routerNode = allNodes.Get(routerIdx);
    Ptr<Node> receiverNode = allNodes.Get(receiverIdx);

    // Access links: each sender -> router (100 Mbps / 1 ms).
    PointToPointHelper accessP2p;
    accessP2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessP2p.SetChannelAttribute("Delay", StringValue("1ms"));

    std::vector<NetDeviceContainer> accessDevs;
    for (uint32_t i = 0; i <= bulkSenders; ++i)
    {
        NodeContainer pair(allNodes.Get(i), routerNode);
        accessDevs.push_back(accessP2p.Install(pair));
    }

    // Bottleneck link: router -> receiver (configured rate / 5 ms).
    PointToPointHelper bottleneckP2p;
    bottleneckP2p.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bottleneckRateBps)));
    bottleneckP2p.SetChannelAttribute("Delay", StringValue("5ms"));
    // Shrink device queue to 1 packet so the TC qdisc is the sole queueing layer.
    bottleneckP2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NodeContainer bottleneckPair(routerNode, receiverNode);
    NetDeviceContainer bottleneckDev = bottleneckP2p.Install(bottleneckPair);

    InternetStackHelper stack;
    stack.Install(allNodes);

    // Install the selected disc on the bottleneck egress (router -> receiver).
    Ptr<QueueDisc> bottleneckDisc =
        InstallSelectedMode(NetDeviceContainer(bottleneckDev.Get(0)), mode);

    // Assign IP addresses.
    Ipv4AddressHelper ip;
    // Access subnets: 10.0.1.0/24, 10.0.2.0/24, ..., one per sender.
    std::vector<Ipv4InterfaceContainer> accessIfs;
    for (uint32_t i = 0; i <= bulkSenders; ++i)
    {
        std::ostringstream base;
        base << "10.0." << (i + 1) << ".0";
        ip.SetBase(base.str().c_str(), "255.255.255.0");
        accessIfs.push_back(ip.Assign(accessDevs[i]));
    }
    // Bottleneck subnet: 10.1.0.0/24.
    ip.SetBase("10.1.0.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIf = ip.Assign(bottleneckDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Receiver address (bottleneck interface, receiver side = index 1).
    Ipv4Address receiverAddr = bottleneckIf.GetAddress(1);

    // -----------------------------------------------------------------------
    // UDP probe: port 50001, ECT(1) TOS, tagged with SendTimeTag.
    // -----------------------------------------------------------------------
    constexpr uint16_t kProbePort = 50001;

    PacketSinkHelper probeSinkHelper("ns3::UdpSocketFactory",
                                     InetSocketAddress(Ipv4Address::GetAny(), kProbePort));
    ApplicationContainer probeSinkApp = probeSinkHelper.Install(receiverNode);
    probeSinkApp.Start(Seconds(0.0));
    probeSinkApp.Stop(Seconds(simTime));

    Ptr<PacketSink> probeSink = DynamicCast<PacketSink>(probeSinkApp.Get(0));
    probeSink->TraceConnectWithoutContext("Rx", MakeCallback(&OnProbeRx));

    Ptr<UdpProbeApp> probeApp = CreateObject<UdpProbeApp>();
    probeApp->Setup(InetSocketAddress(receiverAddr, kProbePort),
                    probePktSize,
                    probeKbps * 1000ULL,
                    static_cast<uint8_t>(Ipv4Header::ECN_ECT1));
    allNodes.Get(0)->AddApplication(probeApp);
    probeApp->SetStartTime(Seconds(0.1));
    probeApp->SetStopTime(Seconds(simTime));

    // -----------------------------------------------------------------------
    // Bulk senders: one per bulk sender node, staggered start.
    //
    // Default (mixedTraffic=false): all N use TcpDctcp ECT(1) (L4S lane).
    //
    // Mixed (mixedTraffic=true): the first floor(N/2) senders use
    // TcpCubic ECT(0) (classic lane); the remaining senders use TcpDctcp
    // ECT(1) (L4S lane). Flow IDs 0..floor(N/2)-1 are TcpCubic; the rest
    // are TcpDctcp. This populates the classic lane so the classic AQM and
    // the DualPI2 coupling controller are exercised together.
    //
    // Two-pass install: Config::SetDefault("ns3::TcpL4Protocol::SocketType")
    // only affects sockets created AFTER the call, and BulkSendApplication
    // creates its socket at StartApplication() time (not at Install() time).
    // With staggered starts the last-set SocketType would otherwise win for
    // all flows. The two-pass approach installs all TcpCubic apps first (so
    // TcpCubic is the active default when those apps are installed), then
    // switches the default to TcpDctcp before installing TcpDctcp apps.
    // Because all apps call StartApplication() after Install() completes, the
    // per-app SocketType is frozen by the time Simulator::Run() dispatches
    // the Start events. Sink apps are port-keyed, so the install order for
    // sinks does not matter.
    //
    // The Ipv4Header::ECN_NotECT TOS value (= 0x00) is used for TcpCubic
    // senders: they do not signal ECN capability, so the IP layer sends
    // packets with ECN field = 00 (not ECT). The L4S classifier routes
    // ECT(1) to the L4S sub-queue and non-ECT / ECT(0) to the classic queue.
    // -----------------------------------------------------------------------

    const uint32_t classicCount = mixedTraffic ? (bulkSenders / 2) : 0;
    const uint32_t l4sCount = bulkSenders - classicCount;

    // --- Pass 1: install TcpCubic (classic-lane) senders (IDs 0..classicCount-1) ---
    if (classicCount > 0)
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));
        for (uint32_t i = 0; i < classicCount; ++i)
        {
            uint16_t bulkPort = static_cast<uint16_t>(50000 + i);

            PacketSinkHelper bulkSinkHelper("ns3::TcpSocketFactory",
                                            InetSocketAddress(Ipv4Address::GetAny(), bulkPort));
            ApplicationContainer bulkSinkApp = bulkSinkHelper.Install(receiverNode);
            bulkSinkApp.Start(Seconds(0.0));
            bulkSinkApp.Stop(Seconds(simTime));
            g_perFlow.emplace_back();
            Ptr<PacketSink> bulkSink = DynamicCast<PacketSink>(bulkSinkApp.Get(0));
            NS_ABORT_MSG_UNLESS(bulkSink, "Failed to cast bulk sink app to PacketSink");
            bool connected =
                bulkSink->TraceConnectWithoutContext("Rx", MakeBoundCallback(&OnBulkRx, i));
            NS_ABORT_MSG_UNLESS(connected, "Failed to connect Rx trace on bulk sink " << i);

            BulkSendHelper bulk("ns3::TcpSocketFactory", InetSocketAddress(receiverAddr, bulkPort));
            bulk.SetAttribute("MaxBytes", UintegerValue(0));
            // ECN_NotECT = 0x00: classic flow, not ECN-capable at IP level.
            bulk.SetAttribute("Tos", UintegerValue(static_cast<uint8_t>(Ipv4Header::ECN_NotECT)));
            ApplicationContainer bulkApp = bulk.Install(allNodes.Get(i + 1));
            bulkApp.Start(Seconds(1.0 + 0.01 * i));
            bulkApp.Stop(Seconds(simTime));
        }
    }

    // --- Pass 2: install TcpDctcp (L4S-lane) senders (IDs classicCount..N-1) ---
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpDctcp"));
    for (uint32_t j = 0; j < l4sCount; ++j)
    {
        const uint32_t i = classicCount + j; // global flow index
        uint16_t bulkPort = static_cast<uint16_t>(50000 + i);

        PacketSinkHelper bulkSinkHelper("ns3::TcpSocketFactory",
                                        InetSocketAddress(Ipv4Address::GetAny(), bulkPort));
        ApplicationContainer bulkSinkApp = bulkSinkHelper.Install(receiverNode);
        bulkSinkApp.Start(Seconds(0.0));
        bulkSinkApp.Stop(Seconds(simTime));
        g_perFlow.emplace_back();
        Ptr<PacketSink> bulkSink = DynamicCast<PacketSink>(bulkSinkApp.Get(0));
        NS_ABORT_MSG_UNLESS(bulkSink, "Failed to cast bulk sink app to PacketSink");
        bool connected =
            bulkSink->TraceConnectWithoutContext("Rx", MakeBoundCallback(&OnBulkRx, i));
        NS_ABORT_MSG_UNLESS(connected, "Failed to connect Rx trace on bulk sink " << i);

        BulkSendHelper bulk("ns3::TcpSocketFactory", InetSocketAddress(receiverAddr, bulkPort));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        bulk.SetAttribute("Tos", UintegerValue(static_cast<uint8_t>(Ipv4Header::ECN_ECT1)));
        ApplicationContainer bulkApp = bulk.Install(allNodes.Get(i + 1));
        bulkApp.Start(Seconds(1.0 + 0.01 * i));
        bulkApp.Stop(Seconds(simTime));
    }

    // Propagate warmup threshold to the file-scope variable consulted by OnBulkTx.
    g_warmupSec = warmup;

    // Schedule 1 Hz coupling-state sampler for L4S modes.  g_l4sQdisc was set
    // (and the ClassicCoupledDrop TraceSource connected) inside InstallSelectedMode
    // for the three l4s-* modes; it remains nullptr for fqcodel and classic-only.
    if (mode != Mode::FqCoDel && mode != Mode::ClassicOnly)
    {
        Simulator::Schedule(Seconds(1.0), &SampleCouplingState);
    }

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    // Close probe CSV before Destroy() to flush all buffered rows.
    g_probeOwdCsv.close();

    // -----------------------------------------------------------------------
    // Emit coupling-counters CSV (l4s-* modes only).
    //
    // Schema: t_us (microseconds since sim epoch), p_prime (DualPI2 base
    // probability), p_C (classic coupled drop probability), coupledDropCount
    // (cumulative coupled drops since sim start, monotone non-decreasing).
    //
    // Not emitted for fqcodel and classic-only modes because the l4s::QueueDisc
    // substrate is absent in those modes (g_l4sQdisc remains nullptr).
    // -----------------------------------------------------------------------
    if (mode != Mode::FqCoDel && mode != Mode::ClassicOnly)
    {
        const std::filesystem::path cf = cellDir / "coupling-counters.csv";
        std::ofstream cs(cf);
        NS_ABORT_MSG_UNLESS(cs.is_open(), "Failed to open coupling-counters.csv at " << cf);
        cs << "t_us,p_prime,p_C,coupledDropCount\n";
        for (const auto& s : g_couplingSamples)
        {
            cs << static_cast<int64_t>(s.tSec * 1e6) << "," << s.pPrime << "," << s.pC << ","
               << s.coupledDropCount << "\n";
        }
        cs.close();
    }

    // -----------------------------------------------------------------------
    // Emit per-flow bulk goodput CSV.
    //
    // Measurement window: [warmup, simTime - 1.0] seconds. The trailing
    // 1-second gap accounts for application stop jitter and ensures the
    // denominator matches the steady-state window captured by the probe OWD.
    //
    // RX-side accounting: bytes counted by OnBulkRx represent what was
    // actually delivered to the PacketSink application layer. This is
    // accurate in lossy regimes (classic-only mode) because retransmitted
    // bytes delivered to TCP are coalesced by the receive buffer and only
    // counted once at delivery. For fairness coefficient-of-variation
    // analysis this gives the ground-truth per-flow goodput.
    // -----------------------------------------------------------------------
    const double windowSec = simTime - warmup - 1.0;
    const std::filesystem::path bulkCsv = cellDir / "bulk-goodput.csv";
    std::ofstream gf(bulkCsv);
    NS_ABORT_MSG_UNLESS(gf.is_open(), "Failed to open bulk-goodput.csv at " << bulkCsv);
    gf << "flow_id,goodput_mbps\n";
    for (uint32_t i = 0; i < g_perFlow.size(); ++i)
    {
        const double mbps = (g_perFlow[i].rxBytes * 8.0) / (windowSec * 1e6);
        gf << i << "," << mbps << "\n";
    }
    gf.close();

    // Report.
    QueueDisc::Stats stats = bottleneckDisc->GetStats();

    std::cout << "\n==== diffserv-l4s-fqcodel-comparison ====\n";
    std::cout << "Mode:           " << ModeToString(mode) << "\n";
    std::cout << "Sim time:       " << simTime << " s\n";
    std::cout << "Warmup:         " << warmup << " s\n";
    std::cout << "Bottleneck:     " << bottleneckRateBps / 1e6 << " Mbps\n";
    if (mixedTraffic)
    {
        std::cout << "Bulk senders:   " << bulkSenders << " (mixed: " << classicCount
                  << " TcpCubic ECT(0) + " << l4sCount << " TcpDctcp ECT(1))\n";
    }
    else
    {
        std::cout << "Bulk senders:   " << bulkSenders << " (TcpDctcp, ECT(1))\n";
    }
    std::cout << "Probe:          UDP " << probeKbps << " kbps ECT(1)\n";
    std::cout << "Output:         " << cellDir << "\n\n";
    std::cout << "Probe rx bytes: " << probeSink->GetTotalRx() << "\n";
    std::cout << "Bulk goodput (RX-side, window=" << windowSec << "s):\n";
    for (uint32_t i = 0; i < g_perFlow.size(); ++i)
    {
        const double mbps = (g_perFlow[i].rxBytes * 8.0) / (windowSec * 1e6);
        std::cout << "  flow " << i << ": " << mbps << " Mbps\n";
    }
    std::cout << "Disc dropped (before enqueue, total): " << stats.nTotalDroppedPacketsBeforeEnqueue
              << "\n";
    std::cout << "Disc marked (total): " << stats.nTotalMarkedPackets << "\n";

    // Per-reason breakdown if available.
    for (const auto& kv : stats.nDroppedPacketsBeforeEnqueue)
    {
        std::cout << "  drop reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    std::cout << "==========================================\n";

    Simulator::Destroy();
    return 0;
}
