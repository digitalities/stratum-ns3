/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ns-3 port of DiffServ4NS example-2 (example-2.tcl).
 * Reproduces the three-class DiffServ scenario from the original 2001
 * example using Premium (EF/TokenBucket), Gold (AF/RIO-C/TSW2CM),
 * and Best Effort (TokenBucket) services with TCP and UDP traffic.
 *
 * Two scales are available, selected via --scale:
 *
 *   --scale=quick (default)
 *     5-source / 5-destination topology, 13 nodes, 2 Mbps bottleneck,
 *     100 s simulated time. Three service classes (Premium/Gold/BE)
 *     with RIO-C on Gold. Used for fast turnaround.
 *     Original: ns2/diffserv4ns/examples/example-2/example-2.tcl
 *
 *   --scale=full
 *     469-node thesis-scale reconstruction of Section 4.2 (AF PHB
 *     importance differentiation via a 6-way WRED parameter sweep,
 *     ~5000 s simulated time). The original HTTP traffic model
 *     (PagePool/WebTraf) crashes under DiffServ4NS in ns-2; this port
 *     approximates it with 400 bulk-TCP sessions, same trade-off as the
 *     2026 ns-2 reconstruction. Provides a second data point for
 *     cross-simulator validation of the AF PHB differentiation
 *     semantics.
 *
 * Classification strategy: the ns-2 original used application-type
 * strings ("any telnet", "any ftp") via the simulator's object model.
 * This ns-3 port uses well-known destination port numbers (23/telnet,
 * 21/ftp), which is how real DiffServ edge routers classify traffic
 * (RFC 2475 section 2.3.1).
 *
 * @see thesis Section 4.2, Table 4.3
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/enum.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-core-queue-disc.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-llq-scheduler.h"
#include "ns3/stratum-monitor-helper.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-scfq-scheduler.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/stratum-sfq-scheduler.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <utility>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::CoreQueueDisc;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::LlqScheduler;
using ns3::stratum::MonitorHelper;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::PriorityScheduler;
using ns3::stratum::ScfqScheduler;
using ns3::stratum::SendTimeTag;
using ns3::stratum::SfqScheduler;

NS_LOG_COMPONENT_DEFINE("DiffServExample2");

// ---------------------------------------------------------------------------
// Global state for metric recording (shared by both scales)
// ---------------------------------------------------------------------------

static Ptr<EdgeQueueDisc> g_edgeDisc;    ///< Edge disc for stats queries
static std::ofstream g_serviceRateFile;  ///< Per-queue departure rates
static std::ofstream g_classRateFile;    ///< Per-class / per-DSCP departure rates
static std::ofstream g_queueLenFile;     ///< Per-queue packet count
static std::ofstream g_virtQueueLenFile; ///< Per-precedence AF queue length (quick only)
static std::ofstream g_owdFile;          ///< EF one-way delay (quick only)
static std::ofstream g_ipdvFile;         ///< EF inter-packet delay variation (quick only)

// OWD/IPDV tracking for EF flow (quick scale only)
static double g_sumOwd = 0.0;
static double g_sumIpdv = 0.0;
static uint64_t g_owdPktCount = 0;
static double g_previousOwd = -1.0;
static double g_txTimeMs = 0.0;

// Sink dedup: avoid installing multiple PacketSinks on the same (node, port).
// Used by the full scale to support the dense 469-node sink installation.
static std::set<std::pair<uint32_t, uint16_t>> g_installedSinks;

// ---------------------------------------------------------------------------
// DiffServCbrApplication: CBR source that attaches SendTimeTag
// before socket->Send(). Used by the quick scale's EF flow for OWD tracking.
// ---------------------------------------------------------------------------
class DiffServCbrApplication : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("DiffServCbrApplication")
                                .SetParent<Application>()
                                .AddConstructor<DiffServCbrApplication>();
        return tid;
    }

    DiffServCbrApplication()
        : m_socket(nullptr),
          m_pktSize(512),
          m_dataRate(300000),
          m_running(false),
          m_sendEvent()
    {
    }

    void Setup(Address remote, uint32_t pktSize, uint64_t dataRateBps)
    {
        m_remote = remote;
        m_pktSize = pktSize;
        m_dataRate = dataRateBps;
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Connect(m_remote);
        m_running = true;
        SendPacket();
    }

    void StopApplication() override
    {
        m_running = false;
        if (m_sendEvent.IsPending())
        {
            Simulator::Cancel(m_sendEvent);
        }
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void SendPacket()
    {
        Ptr<Packet> packet = Create<Packet>(m_pktSize);
        SendTimeTag tag(Simulator::Now().GetSeconds());
        packet->AddPacketTag(tag);
        m_socket->Send(packet);

        double intervalSec = static_cast<double>(m_pktSize * 8) / static_cast<double>(m_dataRate);
        if (m_running)
        {
            m_sendEvent = Simulator::Schedule(Seconds(intervalSec),
                                              &DiffServCbrApplication::SendPacket,
                                              this);
        }
    }

    Ptr<Socket> m_socket;
    Address m_remote;
    uint32_t m_pktSize;
    uint64_t m_dataRate;
    bool m_running;
    EventId m_sendEvent;
};

// ---------------------------------------------------------------------------
// Rx callback: compute OWD and IPDV for EF sink (quick scale)
// ---------------------------------------------------------------------------
static void
EfRxCallback(Ptr<const Packet> packet, const Address& /* addr */)
{
    SendTimeTag tag;
    if (packet->PeekPacketTag(tag))
    {
        double owd = Simulator::Now().GetSeconds() - tag.GetSendTime();
        g_sumOwd += owd;
        g_owdPktCount++;

        if (g_previousOwd >= 0.0)
        {
            double ipdv = std::abs(owd - g_previousOwd);
            g_sumIpdv += ipdv;
        }
        g_previousOwd = owd;
    }
}

// ---------------------------------------------------------------------------
// Periodic metric recording callbacks (quick scale)
// ---------------------------------------------------------------------------

/**
 * Record per-queue aggregate departure rate every 1.0 s (quick scale).
 * Matches ns-2: ServiceRate.tr with PremiumRate, GoldRate, BERate.
 */
static void
RecordServiceRateQuick()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    double premiumRate = sched->GetDepartureRate(0, -1) / 1000.0; // kbps
    double goldRate = sched->GetDepartureRate(1, -1) / 1000.0;
    double beRate = sched->GetDepartureRate(2, -1) / 1000.0;

    g_serviceRateFile << now << " " << premiumRate << " " << goldRate << " " << beRate << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordServiceRateQuick);
}

/**
 * Record per-class departure rate every 1.0 s (quick scale).
 * Matches ns-2: ClassRate.tr with EFRate, TelnetRate, FtpRate, BERate.
 * Telnet = Q1/P0, FTP = Q1/P1 + Q1/P2, EF = Q0/P0, BE = Q2 aggregate.
 */
static void
RecordClassRateQuick()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    double efRate = sched->GetDepartureRate(0, 0) / 1000.0;
    double telnetRate = sched->GetDepartureRate(1, 0) / 1000.0;
    double ftpRate = (sched->GetDepartureRate(1, 1) + sched->GetDepartureRate(1, 2)) / 1000.0;
    double beRate = sched->GetDepartureRate(2, -1) / 1000.0;

    g_classRateFile << now << " " << efRate << " " << telnetRate << " " << ftpRate << " " << beRate
                    << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordClassRateQuick);
}

/**
 * Record per-queue packet count every 0.5 s (quick scale).
 * Matches ns-2: QueueLen.tr with PremiumQueue, GoldQueue, BEQueue.
 */
static void
RecordQueueLengthQuick()
{
    double now = Simulator::Now().GetSeconds();

    auto getQLen = [](uint32_t idx) -> int {
        // The edge composer delegates to its inner stratum::RedQueueDisc for
        // sub-queue iteration.
        Ptr<stratum::RedQueueDisc> inner =
            DynamicCast<stratum::RedQueueDisc>(g_edgeDisc->GetInnerDisc());
        if (inner && inner->GetNQueueDiscClasses() > idx)
        {
            return static_cast<int>(inner->GetQueueDiscClass(idx)->GetQueueDisc()->GetNPackets());
        }
        return 0;
    };

    g_queueLenFile << now << " " << getQLen(0) << " " << getQLen(1) << " " << getQLen(2) << "\n";
    Simulator::Schedule(Seconds(0.5), &RecordQueueLengthQuick);
}

/**
 * Record per-precedence virtual queue length for the Gold (AF) queue every 0.5
 * s (quick scale). Matches ns-2: VirQueueLen.tr with TelnetQueue, FTPinQueue,
 * FTPoutQueue. These are the virtual queue lengths within physical queue 1.
 */
static void
RecordVirtualQueueLengthQuick()
{
    double now = Simulator::Now().GetSeconds();

    int telnetVQ = g_edgeDisc->GetVirtualQueueLen(1, 0); // Q1/P0 = Telnet (AF11)
    int ftpInVQ = g_edgeDisc->GetVirtualQueueLen(1, 1);  // Q1/P1 = FTP in (AF12)
    int ftpOutVQ = g_edgeDisc->GetVirtualQueueLen(1, 2); // Q1/P2 = FTP out (AF13)

    g_virtQueueLenFile << now << " " << telnetVQ << " " << ftpInVQ << " " << ftpOutVQ << "\n";
    Simulator::Schedule(Seconds(0.5), &RecordVirtualQueueLengthQuick);
}

/**
 * Record average OWD and IPDV for EF flow every 0.5 s (quick scale).
 * Reports cumulative mean (matching ns-2 behaviour).
 */
static void
RecordDelayQuick()
{
    double now = Simulator::Now().GetSeconds();

    if (g_owdPktCount >= 2)
    {
        double meanOwd = g_sumOwd / static_cast<double>(g_owdPktCount);
        double meanIpdv = g_sumIpdv / static_cast<double>(g_owdPktCount - 1);
        g_owdFile << now << " " << (meanOwd * 1000.0 - g_txTimeMs) << "\n";
        g_ipdvFile << now << " " << meanIpdv * 1000.0 << "\n";
    }
    else
    {
        g_owdFile << now << " 0\n";
        g_ipdvFile << now << " 0\n";
    }
    Simulator::Schedule(Seconds(0.5), &RecordDelayQuick);
}

// ---------------------------------------------------------------------------
// Periodic metric recording callbacks (full scale)
// ---------------------------------------------------------------------------

/// Per-queue departure rate every 1.0 s (full scale: AF + Default).
static void
RecordServiceRateFull()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    double afRate = sched->GetDepartureRate(0, -1) / 1000.0;
    double defaultRate = sched->GetDepartureRate(1, -1) / 1000.0;

    g_serviceRateFile << now << " " << afRate << " " << defaultRate << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordServiceRateFull);
}

/// Per-DSCP departure rate every 1.0 s (full scale): Telnet (DP0=10), FTP
/// (DP1=12), HTTP (DP2=14), BE-in (0), BE-out (50).
static void
RecordClassRateFull()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    double telnetRate = sched->GetDepartureRate(0, 0) / 1000.0;
    double ftpRate = sched->GetDepartureRate(0, 1) / 1000.0;
    double httpRate = sched->GetDepartureRate(0, 2) / 1000.0;
    double beInRate = sched->GetDepartureRate(1, 0) / 1000.0;
    double beOutRate = sched->GetDepartureRate(1, 1) / 1000.0;

    g_classRateFile << now << " " << telnetRate << " " << ftpRate << " " << httpRate << " "
                    << beInRate << " " << beOutRate << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordClassRateFull);
}

/// Per-queue instantaneous length every 1.0 s (full scale).
static void
RecordQueueLengthFull()
{
    double now = Simulator::Now().GetSeconds();

    auto getQLen = [](uint32_t idx) -> int {
        Ptr<stratum::RedQueueDisc> inner =
            DynamicCast<stratum::RedQueueDisc>(g_edgeDisc->GetInnerDisc());
        if (inner && inner->GetNQueueDiscClasses() > idx)
        {
            return static_cast<int>(inner->GetQueueDiscClass(idx)->GetQueueDisc()->GetNPackets());
        }
        return 0;
    };

    g_queueLenFile << now << " " << getQLen(0) << " " << getQLen(1) << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordQueueLengthFull);
}

// ---------------------------------------------------------------------------
// Helpers used by the full scale: sink dedup + auto-incrementing subnets
// ---------------------------------------------------------------------------

static void
EnsureSink(Ptr<Node> node, uint16_t port, const std::string& protocol)
{
    uint32_t nodeId = node->GetId();
    auto key = std::make_pair(nodeId, port);
    if (g_installedSinks.count(key) > 0)
    {
        return;
    }
    g_installedSinks.insert(key);

    PacketSinkHelper sink(protocol, InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer app = sink.Install(node);
    app.Start(Seconds(0.0));
    app.Stop(Seconds(1e9));
}

static uint32_t g_subnetIdx = 1;

static Ipv4InterfaceContainer
AssignSubnet(Ipv4AddressHelper& addr, NetDeviceContainer& devices)
{
    uint32_t x = g_subnetIdx / 256;
    uint32_t y = g_subnetIdx % 256;
    std::ostringstream base;
    base << "10." << x << "." << y << ".0";
    addr.SetBase(base.str().c_str(), "255.255.255.0");
    g_subnetIdx++;
    return addr.Assign(devices);
}

// ---------------------------------------------------------------------------
// WRED parameter table (full scale; verified against thesis Figure 4.3)
// Format: {dp0_min, dp0_max, dp0_maxP, dp1_..., dp2_...}
// ---------------------------------------------------------------------------
struct WredParams
{
    double dp0_min, dp0_max, dp0_maxP;
    double dp1_min, dp1_max, dp1_maxP;
    double dp2_min, dp2_max, dp2_maxP;
};

static const WredParams kWredSweep[6] = {
    // Set 1 — staggered
    {50.0, 70.0, 0.1, 30.0, 50.0, 0.2, 10.0, 30.0, 0.5},
    // Set 2 — staggered (shifted)
    {65.0, 95.0, 0.1, 35.0, 65.0, 0.2, 5.0, 35.0, 0.5},
    // Set 3 — partially overlapped
    {45.0, 65.0, 0.1, 30.0, 50.0, 0.2, 15.0, 35.0, 0.5},
    // Set 4 — partially overlapped (wider)
    {40.0, 60.0, 0.1, 30.0, 50.0, 0.2, 20.0, 40.0, 0.5},
    // Set 5 — overlapped (narrow)
    {20.0, 60.0, 0.1, 20.0, 60.0, 0.2, 20.0, 60.0, 0.5},
    // Set 6 — overlapped (wide)
    {20.0, 80.0, 0.1, 20.0, 80.0, 0.2, 20.0, 80.0, 0.5},
};

// ---------------------------------------------------------------------------
// Quick-scale scenario runner: 13-node, three-class DiffServ reproduction.
// ---------------------------------------------------------------------------
static int
RunQuickScenario(const std::string& scheduler,
                 uint32_t efPacketSize,
                 double simTime,
                 uint32_t seed,
                 const std::string& outputDir)
{
    // Create output directory
    std::string runDir = outputDir + "/" + scheduler;
    diffserv::EnsureDir(runDir);

    // ---- RNG ----
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    // txTime for OWD correction: pktSize * 8 / linkRateKbps (ms)
    g_txTimeMs = static_cast<double>(efPacketSize) * 8.0 / 2000.0;

    Ptr<UniformRandomVariable> rndStartTime = CreateObject<UniformRandomVariable>();
    rndStartTime->SetAttribute("Min", DoubleValue(0.0));
    rndStartTime->SetAttribute("Max", DoubleValue(2.0));

    Ptr<UniformRandomVariable> rndSourceNode = CreateObject<UniformRandomVariable>();
    rndSourceNode->SetAttribute("Min", DoubleValue(0.0));
    rndSourceNode->SetAttribute("Max", DoubleValue(4.0));

    // ---- Nodes ----
    NodeContainer sources;
    sources.Create(5); // s(0)..s(4)

    NodeContainer routers;
    routers.Create(3); // e1, core, e2

    NodeContainer destinations;
    destinations.Create(5); // dest(0)..dest(4)

    Ptr<Node> e1 = routers.Get(0);
    Ptr<Node> core = routers.Get(1);
    Ptr<Node> e2 = routers.Get(2);

    // ---- Links ----
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));

    // Source -> e1 access links
    std::vector<NetDeviceContainer> srcDevs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        srcDevs[i] = p2pAccess.Install(sources.Get(i), e1);
    }

    // e1 <-> core bottleneck (2 Mbps, 5 ms)
    // Set device queue to 1 packet so all queueing is in the DiffServ queue disc.
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devE1Core = p2pBottleneck.Install(e1, core);

    // core <-> e2 (5 Mbps, 3 ms, DropTail)
    PointToPointHelper p2pCoreE2;
    p2pCoreE2.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    p2pCoreE2.SetChannelAttribute("Delay", StringValue("3ms"));
    NetDeviceContainer devCoreE2 = p2pCoreE2.Install(core, e2);

    // e2 -> destination access links
    std::vector<NetDeviceContainer> dstDevs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        dstDevs[i] = p2pAccess.Install(e2, destinations.Get(i));
    }

    // ---- Internet stack ----
    InternetStackHelper internet;
    NodeContainer allNodes;
    allNodes.Add(sources);
    allNodes.Add(routers);
    allNodes.Add(destinations);
    internet.Install(allNodes);

    // ---- IP addresses ----
    Ipv4AddressHelper addr;
    uint32_t subnetIdx = 1;

    // Source -> e1
    std::vector<Ipv4InterfaceContainer> srcIfs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        std::string base = "10.0." + std::to_string(subnetIdx++) + ".0";
        addr.SetBase(base.c_str(), "255.255.255.0");
        srcIfs[i] = addr.Assign(srcDevs[i]);
    }

    // e1 <-> core
    addr.SetBase(("10.0." + std::to_string(subnetIdx++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifE1Core = addr.Assign(devE1Core);

    // core <-> e2
    addr.SetBase(("10.0." + std::to_string(subnetIdx++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifCoreE2 = addr.Assign(devCoreE2);

    // e2 -> destinations
    std::vector<Ipv4InterfaceContainer> dstIfs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        std::string base = "10.0." + std::to_string(subnetIdx++) + ".0";
        addr.SetBase(base.c_str(), "255.255.255.0");
        dstIfs[i] = addr.Assign(dstDevs[i]);
    }

    // ---- Routing ----
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Destination addresses for traffic generators
    Ipv4Address destAddr0 = dstIfs[0].GetAddress(1); // d0 — EF destination
    // Source address of s(0) for Premium mark rule
    Ipv4Address srcAddr0 = srcIfs[0].GetAddress(0); // s(0) source IP

    // ====================================================================
    // DiffServ Edge configuration (e1 -> core)
    //
    // Three service classes:
    // Q0 = Premium (EF): TokenBucket + tail-drop
    // Q1 = Gold (AF): Dumb(telnet) + TSW2CM(FTP) + RIO-C (WRED)
    // Q2 = Best Effort: TokenBucket + tail-drop
    //
    // ns-2 TCS (Table 4.2 in thesis):
    // EF: CIR 500 kbps, CBS 100 KB, tail-drop
    // AF: Telnet=Dumb, FTP=TSW2CM(500 kbps), RIO-C with WRED params
    // BE: CIR 700 kbps, CBS 100 KB, tail-drop
    // ====================================================================

    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    // --- Physical queues ---
    edgeInner->SetNumQueues(3);
    edgeInner->SetNumPrec(0, 2); // Premium: 2 prec (in/out-profile)
    edgeInner->SetNumPrec(1, 3); // Gold: 3 prec (AF11/AF12/AF13)
    edgeInner->SetNumPrec(2, 2); // BE: 2 prec (in/out-profile)

    // Queue limits (packets) — set BEFORE Initialize
    edgeInner->SetQueueLimit(0, 50);  // Premium: 50 packets
    edgeInner->SetQueueLimit(1, 150); // Gold: 150 packets
    edgeInner->SetQueueLimit(2, 100); // BE: 100 packets

    // --- Scheduler ---
    Ptr<stratum::Scheduler> sched;
    if (scheduler == "PQ")
    {
        sched = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                              UintegerValue(3),
                                                              "WinLen",
                                                              DoubleValue(1.0));
    }
    else if (scheduler == "SCFQ")
    {
        auto scfq = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                              UintegerValue(3),
                                                              "LinkBandwidth",
                                                              DoubleValue(2000000.0));
        scfq->SetParam(0, 3.0);  // Premium weight
        scfq->SetParam(1, 10.0); // Gold weight
        scfq->SetParam(2, 7.0);  // BE weight
        sched = scfq;
    }
    else if (scheduler == "LLQ")
    {
        // LLQ: PQ for queue 0 (Premium), SFQ for queues 1-2 (Gold, BE)
        // ns-2: LLQ SFQ 1700000 — 1.7 Mbps for the FQ sub-scheduler
        auto llq =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(3),
                                                     "LinkBandwidth",
                                                     DoubleValue(1700000.0),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::SFQ));
        llq->SetParam(1, 10.0); // Gold weight
        llq->SetParam(2, 7.0);  // BE weight
        sched = llq;
    }
    else
    {
        NS_FATAL_ERROR("Unknown scheduler: " << scheduler << ". Use PQ, SCFQ, or LLQ.");
    }
    edgeInner->SetScheduler(sched);

    // --- Mark rules ---
    // Rule 1: packets FROM s(0) → DSCP 46 (Premium/EF)
    // ns-2: addMarkRule 46 [$s(0) id] -1 any any
    edgeDisc->AddMarkRule({.dscp = 46, .srcAddr = srcAddr0});

    // Rule 2: telnet traffic (dstPort 23) → DSCP 10 (AF11)
    // ns-2: addMarkRule 10 -1 -1 any telnet
    edgeDisc->AddMarkRule({.dscp = 10, .dstPort = 23});

    // Rule 3: FTP traffic (dstPort 21) → DSCP 12 (AF12)
    // ns-2: addMarkRule 12 -1 -1 any ftp
    edgeDisc->AddMarkRule({.dscp = 12, .dstPort = 21});

    // --- Policy entries ---
    // Premium (EF): TokenBucket CIR=500kbps, CBS=100K bytes
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 46, .cirBps = 500000.0, .cbsBytes = 100000.0});
    helper.AddDumbPolicy(edgeDisc, 51); // out-of-profile EF

    // Gold (AF): Dumb for telnet (no metering), TSW2CM for FTP
    helper.AddDumbPolicy(edgeDisc, 10); // Telnet — no policing
    // TSW2CM for FTP: CIR = algBW/2/2 = 2000000/2/2 = 500000 bps
    helper.AddTsw2cmPolicy(edgeDisc, {.codePt = 12, .cirBps = 500000.0});
    helper.AddDumbPolicy(edgeDisc,
                         14); // FTP out-of-profile (downstream of TSW2CM)

    // Best Effort: TokenBucket CIR=700kbps, CBS=100K bytes
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 0, .cirBps = 700000.0, .cbsBytes = 100000.0});
    helper.AddDumbPolicy(edgeDisc, 50); // out-of-profile BE

    // --- Policer entries ---
    // Premium: TokenBucket 46 → 51 (out-of-profile)
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 46,
                            .downgrade1 = 51,
                            .downgrade2 = 51,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 51,
                            .downgrade1 = 51,
                            .downgrade2 = 51,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // Gold: Dumb for telnet, TSW2CM for FTP: 12 → 14 (downgrade)
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 10,
                            .downgrade1 = 10,
                            .downgrade2 = 10,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TSW2CM,
                            .initialCodePt = 12,
                            .downgrade1 = 14,
                            .downgrade2 = 14,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 14,
                            .downgrade1 = 14,
                            .downgrade2 = 14,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // Best Effort: TokenBucket 0 → 50 (out-of-profile)
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 0,
                            .downgrade1 = 50,
                            .downgrade2 = 50,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 50,
                            .downgrade1 = 50,
                            .downgrade2 = 50,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // --- PHB table ---
    // Premium
    helper.AddPhbEntry(edgeInner, 46, 0, 0); // EF in-profile → Q0/P0
    helper.AddPhbEntry(edgeInner, 51, 0, 1); // EF out-of-profile → Q0/P1

    // Gold (AF PHB group)
    helper.AddPhbEntry(edgeInner, 10, 1, 0); // AF11 (Telnet) → Q1/P0
    helper.AddPhbEntry(edgeInner, 12, 1, 1); // AF12 (FTP in) → Q1/P1
    helper.AddPhbEntry(edgeInner, 14, 1, 2); // AF13 (FTP out) → Q1/P2

    // Best Effort
    helper.AddPhbEntry(edgeInner, 0, 2, 0);  // BE in-profile → Q2/P0
    helper.AddPhbEntry(edgeInner, 50, 2, 1); // BE out-of-profile → Q2/P1

    // --- Install edge disc ---
    Ptr<NetDevice> e1Dev = devE1Core.Get(0);
    stratum::InstallRoot(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    // --- MRED modes (set per-queue AFTER Initialize, when sub-queues exist) ---
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 0); // Premium: tail drop
    edgeInner->SetMredMode(MredMode::RIO_C, 1);     // Gold: RIO-C (WRED)
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 2); // BE: tail drop

    // --- Set bandwidth and mean packet size for RIO-C calculation ---
    // ns-2: meanPktSize 1300; setQueueBW 1 1000000
    edgeInner->SetMeanPacketSize(1300);
    edgeInner->SetQueueBandwidth(1, 1000000.0); // Gold queue: 1 Mbps

    // --- RED/DROP thresholds ---
    // Premium Q0: tail-drop, in-profile max=30, out-of-profile drop-all
    helper.ConfigQueue(
        edgeInner,
        {.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 30.0, .maxP = 1.0}); // P0: accept up to 30
    helper.ConfigQueue(edgeInner,
                       {.queue = 0,
                        .prec = 1,
                        .thMin = -1.0,
                        .thMax = -1.0,
                        .maxP = 0.0}); // P1: drop all (thMin<0)

    // Gold Q1: RIO-C (WRED) per-precedence thresholds
    helper.ConfigQueue(edgeInner,
                       {.queue = 1,
                        .prec = 0,
                        .thMin = 60.0,
                        .thMax = 110.0,
                        .maxP = 0.02}); // AF11 (Telnet): gentle
    helper.ConfigQueue(edgeInner,
                       {.queue = 1,
                        .prec = 1,
                        .thMin = 30.0,
                        .thMax = 60.0,
                        .maxP = 0.6}); // AF12 (FTP in): moderate
    helper.ConfigQueue(edgeInner,
                       {.queue = 1,
                        .prec = 2,
                        .thMin = 5.0,
                        .thMax = 10.0,
                        .maxP = 0.8}); // AF13 (FTP out): aggressive

    // BE Q2: tail-drop, in-profile max=100, out-of-profile drop-all
    helper.ConfigQueue(edgeInner,
                       {.queue = 2,
                        .prec = 0,
                        .thMin = 100.0,
                        .thMax = 100.0,
                        .maxP = 1.0}); // P0: accept up to 100
    helper.ConfigQueue(edgeInner,
                       {.queue = 2,
                        .prec = 1,
                        .thMin = -1.0,
                        .thMax = -1.0,
                        .maxP = 0.0}); // P1: drop all (thMin<0)

    // Store global reference for metric recording
    g_edgeDisc = edgeDisc;

    // ====================================================================
    // DiffServ Core configuration (core -> e1)
    // Minimal: 1 queue, tail-drop, threshold=50
    // ====================================================================
    Ptr<CoreQueueDisc> coreDisc = CreateObject<CoreQueueDisc>();
    auto coreInner = helper.InstallRedInner(coreDisc);
    coreInner->SetNumQueues(1);
    coreInner->SetNumPrec(0, 1);

    Ptr<PriorityScheduler> corePq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(1),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    coreInner->SetScheduler(corePq);

    helper.AddPhbEntry(coreInner, 10, 0, 0);
    helper.AddPhbEntry(coreInner, 0, 0, 0);

    Ptr<NetDevice> coreDev = devE1Core.Get(1);
    stratum::InstallRoot(coreDev, coreDisc);
    coreDisc->Initialize();

    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(coreInner,
                       {.queue = 0, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});

    // ====================================================================
    // Traffic 1: Premium / EF (1 CBR, 300 kbps, 1300-byte packets)
    //
    // ns-2: cbr_connection 0 0 $s(0) $dest(0) 1 $EFPacketSize $EFRate
    // ====================================================================
    uint16_t efPort = 9000;
    double efRateBps = 300000.0;

    // EF sink on dest(0)
    PacketSinkHelper efSinkHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), efPort));
    ApplicationContainer efSinkApp = efSinkHelper.Install(destinations.Get(0));
    efSinkApp.Start(Seconds(0.0));
    efSinkApp.Stop(Seconds(simTime));

    // OWD/IPDV Rx callback
    efSinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeCallback(&EfRxCallback));

    // EF source: s(0) → dest(0), CBR at 300 kbps
    double efStartTime = rndStartTime->GetValue() + 1.0; // ns-2: uniform [1, 3]
    Ptr<DiffServCbrApplication> efCbr = CreateObject<DiffServCbrApplication>();
    efCbr->Setup(InetSocketAddress(destAddr0, efPort),
                 efPacketSize,
                 static_cast<uint64_t>(efRateBps));
    sources.Get(0)->AddApplication(efCbr);
    efCbr->SetStartTime(Seconds(efStartTime));
    efCbr->SetStopTime(Seconds(simTime));

    std::cout << "EF: s(0)->dest(0) - CBR - PktSize: " << efPacketSize << " - Rate: " << efRateBps
              << "bps - Start: " << efStartTime << "\n";

    // ====================================================================
    // Traffic 2 & 3: Gold / AF — Telnet + FTP (TCP)
    //
    // Install ONE sink per (destination node, port) to avoid bind conflicts.
    // ns-3 enforces real socket semantics — only one bind per (addr, port).
    //
    // Telnet: OnOffApplication with TCP, short bursts + exponential off-time
    // (approximates ns-2's Application/Telnet with tcplib inter-arrival)
    // FTP: BulkSendApplication (standard ns-3 FTP equivalent)
    // ====================================================================
    uint16_t telnetPort = 23;
    uint16_t ftpPort = 21;

    // Install sinks once per destination node (dest 1-3 used by Telnet/FTP)
    for (uint32_t d = 1; d <= 3; d++)
    {
        PacketSinkHelper telnetSink("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), telnetPort));
        ApplicationContainer ts = telnetSink.Install(destinations.Get(d));
        ts.Start(Seconds(0.0));
        ts.Stop(Seconds(simTime));

        PacketSinkHelper ftpSink("ns3::TcpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), ftpPort));
        ApplicationContainer fs = ftpSink.Install(destinations.Get(d));
        fs.Start(Seconds(0.0));
        fs.Stop(Seconds(simTime));
    }

    // 12 Telnet connections: 4 per pair (s(1-3) → dest(1-3))
    for (uint32_t i = 0; i < 12; i++)
    {
        uint32_t pairIdx = i / 4 + 1;            // 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3
        auto startTime = static_cast<double>(i); // ns-2: start = i

        OnOffHelper telnetOnOff("ns3::TcpSocketFactory",
                                InetSocketAddress(dstIfs[pairIdx].GetAddress(1), telnetPort));
        telnetOnOff.SetAttribute("DataRate", StringValue("50kbps"));
        telnetOnOff.SetAttribute("PacketSize", UintegerValue(512));
        telnetOnOff.SetAttribute("OnTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0.01]"));
        telnetOnOff.SetAttribute("OffTime",
                                 StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));

        ApplicationContainer telnetApp = telnetOnOff.Install(sources.Get(pairIdx));
        telnetApp.Start(Seconds(startTime));
        telnetApp.Stop(Seconds(simTime));

        std::cout << "Telnet[" << i << "]: s(" << pairIdx << ")->dest(" << pairIdx
                  << ") - TCP - Start: " << startTime << "\n";
    }

    // 12 FTP connections: 4 per pair (s(1-3) → dest(1-3))
    for (uint32_t i = 0; i < 12; i++)
    {
        uint32_t pairIdx = i / 4 + 1;
        auto startTime = static_cast<double>(i); // ns-2: start = i

        BulkSendHelper ftpBulk("ns3::TcpSocketFactory",
                               InetSocketAddress(dstIfs[pairIdx].GetAddress(1), ftpPort));
        ftpBulk.SetAttribute("MaxBytes", UintegerValue(0));    // Unlimited
        ftpBulk.SetAttribute("SendSize", UintegerValue(1460)); // MSS

        ApplicationContainer ftpApp = ftpBulk.Install(sources.Get(pairIdx));
        ftpApp.Start(Seconds(startTime));
        ftpApp.Stop(Seconds(simTime));

        std::cout << "FTP[" << i << "]: s(" << pairIdx << ")->dest(" << pairIdx
                  << ") - TCP BulkSend - Start: " << startTime << "\n";
    }

    // ====================================================================
    // Traffic 4: Best Effort (23 CBR UDP flows, 100 kbps each)
    //
    // ns-2: random src/dst from s(1-4)/dest(1-4), packet sizes 64..1472
    // ====================================================================
    uint16_t bgPort = 10000;

    uint32_t bgPktSize = 64;
    for (uint32_t i = 0; i < 23; i++)
    {
        double startTime = rndStartTime->GetValue();
        // ns-2: [expr [$rndSourceNode integer 4]+1] → random from {1,2,3,4}
        uint32_t srcIdx = static_cast<uint32_t>(rndSourceNode->GetInteger(0, 3)) + 1;
        uint32_t dstIdx = static_cast<uint32_t>(rndSourceNode->GetInteger(0, 3)) + 1;

        OnOffHelper bgOnOff("ns3::UdpSocketFactory",
                            InetSocketAddress(dstIfs[dstIdx].GetAddress(1), bgPort));
        bgOnOff.SetAttribute("DataRate", StringValue("100000bps"));
        bgOnOff.SetAttribute("PacketSize", UintegerValue(bgPktSize));
        bgOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        bgOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer bgApp = bgOnOff.Install(sources.Get(srcIdx));
        bgApp.Start(Seconds(startTime));
        bgApp.Stop(Seconds(simTime));

        std::cout << "BG[" << i << "]: s(" << srcIdx << ")->dest(" << dstIdx
                  << ") - CBR - PktSize: " << bgPktSize << " - Rate: 100kbps - Start: " << startTime
                  << "\n";

        bgPktSize += 64; // 64, 128, ..., 1472 (matching ns-2)
    }

    // ====================================================================
    // Open trace files
    // ====================================================================
    g_serviceRateFile.open(runDir + "/ServiceRate.tr");
    g_classRateFile.open(runDir + "/ClassRate.tr");
    g_queueLenFile.open(runDir + "/QueueLen.tr");
    g_virtQueueLenFile.open(runDir + "/VirQueueLen.tr");
    g_owdFile.open(runDir + "/OWD.tr");
    g_ipdvFile.open(runDir + "/IPDV.tr");

    // ====================================================================
    // Schedule metric recording (matching ns-2 start times)
    // ====================================================================
    Simulator::Schedule(Seconds(0.0), &RecordServiceRateQuick);
    Simulator::Schedule(Seconds(0.0), &RecordClassRateQuick);
    Simulator::Schedule(Seconds(6.0), &RecordQueueLengthQuick);
    Simulator::Schedule(Seconds(6.0), &RecordVirtualQueueLengthQuick);
    Simulator::Schedule(Seconds(6.0), &RecordDelayQuick);

    // ====================================================================
    // Print configuration
    // ====================================================================
    std::cout << "\n--- Configuration ---\n"
              << "Scheduler: " << scheduler << "\n"
              << "EF packet size: " << efPacketSize << "\n"
              << "Simulation time: " << simTime << " s\n"
              << "Queues: 3 (Premium/Gold/BE)\n"
              << "Gold MRED: RIO-C\n\n";

    // ====================================================================
    // Run simulation
    // ====================================================================
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ====================================================================
    // Print final statistics
    // ====================================================================
    edgeDisc->PrintStats();

    // Close trace files
    g_serviceRateFile.close();
    g_classRateFile.close();
    g_queueLenFile.close();
    g_virtQueueLenFile.close();
    g_owdFile.close();
    g_ipdvFile.close();

    Simulator::Destroy();

    return 0;
}

// ---------------------------------------------------------------------------
// Full-scale scenario runner: 469-node WRED parameter sweep.
// ---------------------------------------------------------------------------
static int
RunFullScenario(double simTime,
                uint32_t seed,
                uint32_t paramSet,
                const std::string& outputDirBase,
                uint32_t numHttpOverride,
                bool useStockQueue,
                const std::string& classifierMode,
                double srtcmTelnetCirBps,
                double srtcmFtpCirBps,
                double srtcmHttpCirBps,
                double srtcmCbsSeconds,
                const std::string& tcpVariant,
                int tcpSack,
                int tcpTimestamp,
                int tcpWindowScaling,
                uint32_t tcpMaxWindowSize)
{
    // Apply TCP-attribute overrides BEFORE any TcpSocket / BulkSendApp is
    // installed so all sockets pick up the new defaults at construction.
    if (!tcpVariant.empty())
    {
        TypeId tid = TypeId::LookupByName(tcpVariant);
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(tid));
    }
    if (tcpSack != -1)
    {
        Config::SetDefault("ns3::TcpSocketBase::Sack", BooleanValue(tcpSack != 0));
    }
    if (tcpTimestamp != -1)
    {
        Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(tcpTimestamp != 0));
    }
    if (tcpWindowScaling != -1)
    {
        Config::SetDefault("ns3::TcpSocketBase::WindowScaling",
                           BooleanValue(tcpWindowScaling != 0));
    }
    if (tcpMaxWindowSize > 0)
    {
        Config::SetDefault("ns3::TcpSocketBase::MaxWindowSize", UintegerValue(tcpMaxWindowSize));
    }

    if (classifierMode != "port-based" && classifierMode != "srtcm")
    {
        std::cerr << "ERROR: --classifier must be 'port-based' or 'srtcm'\n";
        return 1;
    }

    if (paramSet < 1 || paramSet > 6)
    {
        std::cerr << "ERROR: paramSet must be in 1..6 (got " << paramSet << ")\n";
        return 1;
    }
    const WredParams& wred = kWredSweep[paramSet - 1];

    std::string outputDir = outputDirBase + "/set-" + std::to_string(paramSet);
    diffserv::EnsureDir(outputDir);

    // ---- RNG ----
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    Ptr<UniformRandomVariable> rndDelay = CreateObject<UniformRandomVariable>();
    rndDelay->SetAttribute("Min", DoubleValue(10.0));
    rndDelay->SetAttribute("Max", DoubleValue(100.0));

    Ptr<UniformRandomVariable> rndBw = CreateObject<UniformRandomVariable>();
    rndBw->SetAttribute("Min", DoubleValue(22.0));
    rndBw->SetAttribute("Max", DoubleValue(32.0));

    // ---- Nodes (469 total) ----
    NodeContainer allNodes;
    allNodes.Create(469);
    auto n = [&](uint32_t idx) -> Ptr<Node> { return allNodes.Get(idx); };

    std::cout << "Topology: 469 nodes created\n";

    // ---- Internet stacks ----
    Ipv4AddressHelper addr;
    InternetStackHelper internet;
    internet.Install(allNodes);

    PointToPointHelper p2p;

    // --- DiffServ bottleneck (n0 -> n466, 3 Mbps / 20 ms) ---
    p2p.SetDeviceAttribute("DataRate", StringValue("3Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("20ms"));
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devBottleneck = p2p.Install(n(0), n(466));
    Ipv4InterfaceContainer ifBottleneck = AssignSubnet(addr, devBottleneck);
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100p"));

    // --- n466 <-> n1 (client-side) ---
    p2p.SetDeviceAttribute("DataRate", StringValue("3Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("20ms"));
    NetDeviceContainer dev466_1 = p2p.Install(n(466), n(1));
    AssignSubnet(addr, dev466_1);

    // --- Server access (n0 <-> n2-n5) ---
    uint32_t accessDelays[] = {20, 30, 40, 60};
    for (uint32_t i = 0; i < 4; i++)
    {
        p2p.SetDeviceAttribute("DataRate", StringValue("1.5Mbps"));
        std::string delay = std::to_string(accessDelays[i]) + "ms";
        p2p.SetChannelAttribute("Delay", StringValue(delay));
        NetDeviceContainer dev = p2p.Install(n(0), n(2 + i));
        AssignSubnet(addr, dev);
    }

    // --- Server links (n6-n45 <-> n2-n5, 10 per router) ---
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    for (uint32_t i = 0; i < 40; i++)
    {
        uint32_t base = i / 10 + 2;
        auto delayMs = static_cast<uint32_t>(rndDelay->GetValue());
        p2p.SetChannelAttribute("Delay", StringValue(std::to_string(delayMs) + "ms"));
        NetDeviceContainer dev = p2p.Install(n(base), n(6 + i));
        AssignSubnet(addr, dev);
    }

    // --- Client links (n46-n465 <-> n1) ---
    for (uint32_t i = 0; i < 420; i++)
    {
        auto bwMbps = static_cast<uint32_t>(rndBw->GetValue());
        p2p.SetDeviceAttribute("DataRate", StringValue(std::to_string(bwMbps) + "Mbps"));
        auto delayMs = static_cast<uint32_t>(rndDelay->GetValue());
        p2p.SetChannelAttribute("Delay", StringValue(std::to_string(delayMs) + "ms"));
        NetDeviceContainer dev = p2p.Install(n(1), n(46 + i));
        AssignSubnet(addr, dev);
    }

    // --- Background traffic endpoints ---
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev0_467 = p2p.Install(n(0), n(467));
    AssignSubnet(addr, dev0_467);
    NetDeviceContainer dev466_468 = p2p.Install(n(466), n(468));
    AssignSubnet(addr, dev466_468);

    std::cout << "Links: " << g_subnetIdx - 1 << " subnets assigned\n";

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ====================================================================
    // Diagnostic fast-path: replace our DiffServ queue disc with a stock
    // ns-3 RedQueueDisc on the bottleneck. No classification, no
    // AF/Default differentiation — everything sees the same RED.
    // Used to isolate whether scaling crashes come from our code or from
    // ns-3 core/TCP at high concurrent-flow counts.
    // ====================================================================
    if (useStockQueue)
    {
        TrafficControlHelper tchStock;
        tchStock.SetRootQueueDisc("ns3::RedQueueDisc",
                                  "MinTh",
                                  DoubleValue(30.0),
                                  "MaxTh",
                                  DoubleValue(60.0),
                                  "MaxSize",
                                  StringValue("100p"),
                                  "LinkBandwidth",
                                  StringValue("3Mbps"),
                                  "LinkDelay",
                                  StringValue("20ms"),
                                  "MeanPktSize",
                                  UintegerValue(1000));
        tchStock.Install(devBottleneck.Get(0));
        tchStock.Install(devBottleneck.Get(1));

        std::cout << "[DIAGNOSTIC] Using stock ns3::RedQueueDisc "
                     "(no DiffServ classification). paramSet ignored.\n";

        Ipv4Address addr468_stock = n(468)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();

        // Telnet
        for (uint32_t i = 0; i < 50; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            EnsureSink(n(dst), 23, "ns3::TcpSocketFactory");
            // ~200 kbps aggregate, aligned with ns-2.35 Application/Telnet.
            OnOffHelper telnet("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, 23));
            telnet.SetAttribute("DataRate", StringValue("50kbps"));
            telnet.SetAttribute("PacketSize", UintegerValue(512));
            telnet.SetAttribute("OnTime",
                                StringValue("ns3::ConstantRandomVariable[Constant=0.16]"));
            telnet.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=1.0]"));
            ApplicationContainer app = telnet.Install(n(src));
            app.Start(Seconds(static_cast<double>(i)));
            app.Stop(Seconds(50.0));
        }

        // FTP
        for (uint32_t i = 0; i < 50; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            EnsureSink(n(dst), 21, "ns3::TcpSocketFactory");
            BulkSendHelper ftp("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, 21));
            ftp.SetAttribute("MaxBytes", UintegerValue(50000));
            ftp.SetAttribute("SendSize", UintegerValue(1460));
            ApplicationContainer app = ftp.Install(n(src));
            app.Start(Seconds(static_cast<double>(i)));
            app.Stop(Seconds(50.0));
        }

        // HTTP
        uint32_t nh = (numHttpOverride > 0) ? numHttpOverride : 400;
        for (uint32_t i = 0; i < nh; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 420) + 46;
            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            EnsureSink(n(dst), 80, "ns3::TcpSocketFactory");
            BulkSendHelper http("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, 80));
            http.SetAttribute("MaxBytes", UintegerValue(0));
            http.SetAttribute("SendSize", UintegerValue(1460));
            ApplicationContainer app = http.Install(n(src));
            app.Start(Seconds(static_cast<double>(i) * 0.25));
            app.Stop(Seconds(simTime - 10.0));
        }

        // BG
        EnsureSink(n(468), 10000, "ns3::UdpSocketFactory");
        OnOffHelper bg("ns3::UdpSocketFactory", InetSocketAddress(addr468_stock, 10000));
        bg.SetAttribute("DataRate", StringValue("500000bps"));
        bg.SetAttribute("PacketSize", UintegerValue(512));
        bg.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000000]"));
        bg.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer bgApp = bg.Install(n(467));
        bgApp.Start(Seconds(0.0));
        bgApp.Stop(Seconds(simTime));

        std::cout << "[DIAGNOSTIC] " << 50 << " Telnet + 50 FTP + " << nh
                  << " HTTP + 1 BG on stock RED\n";

        Simulator::Stop(Seconds(simTime));
        Simulator::Run();
        Simulator::Destroy();
        return 0;
    }

    // ====================================================================
    // DiffServ Edge: 2 queues (AF + Default), SFQ scheduler 17:3
    // ====================================================================
    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    edgeInner->SetNumQueues(2);
    edgeInner->SetNumPrec(0, 3); // AF: 3 drop precedences (DP0/DP1/DP2)
    edgeInner->SetNumPrec(1, 2); // Default: 2 prec (in/out-profile)

    edgeInner->SetQueueLimit(0, 100); // AF
    edgeInner->SetQueueLimit(1, 50);  // Default

    // Scheduler: SFQ on 3 Mbps with weights AF:Default = 17:3 (85% : 15%)
    auto sfq = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                        UintegerValue(2),
                                                        "LinkBandwidth",
                                                        DoubleValue(3000000.0));
    sfq->SetParam(0, 17.0);
    sfq->SetParam(1, 3.0);
    edgeInner->SetScheduler(sfq);

    // --- Mark rules: port-based OR per-flow srTCM ---
    if (classifierMode == "port-based")
    {
        // Rule 1: Telnet (TCP dstPort 23)  -> DSCP 10 (AF11, DP0)
        edgeDisc->AddMarkRule({.dscp = 10, .dstPort = 23});
        // Rule 2: FTP (TCP dstPort 21)     -> DSCP 12 (AF12, DP1)
        edgeDisc->AddMarkRule({.dscp = 12, .dstPort = 21});
        // Rule 3: HTTP (TCP dstPort 80)    -> DSCP 14 (AF13, DP2)
        edgeDisc->AddMarkRule({.dscp = 14, .dstPort = 80});
        // Default: unmatched -> DSCP 0 (Default/BE)
    }
    else // srtcm
    {
        std::cout << "Classifier: srTCM per-flow (CIR Telnet=" << srtcmTelnetCirBps
                  << " FTP=" << srtcmFtpCirBps << " HTTP=" << srtcmHttpCirBps
                  << " CBS=" << srtcmCbsSeconds << "s)\n";

        // CBS in bytes = (CIR in bps / 8) * window seconds.
        auto cirToBytesCbs = [srtcmCbsSeconds](double bps) {
            return (bps / 8.0) * srtcmCbsSeconds;
        };
        const double telnetCbs = cirToBytesCbs(srtcmTelnetCirBps);
        const double ftpCbs = cirToBytesCbs(srtcmFtpCirBps);
        const double httpCbs = cirToBytesCbs(srtcmHttpCirBps);

        // Resolve numHttp the same way the traffic-generation block does.
        const uint32_t numTelnetRules = 50;
        const uint32_t numFtpRules = 50;
        const uint32_t numHttpRules = (numHttpOverride > 0) ? numHttpOverride : 400;

        // srcPort = 0 acts as a wildcard against ephemeral TCP ports
        // (see diffserv::PerFlowPolicyClassifier::ApplyPolicyOrPassthrough fallback).
        const uint16_t kWildcardSrcPort = 0;

        // Telnet: DP0 stays GREEN (10), OOP -> DP1 (12 / 12 for yellow/red
        // reclass). Plan: Telnet green=10, yellow=10, red=12.
        for (uint32_t i = 0; i < numTelnetRules; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            Ipv4Address srcAddr = n(src)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            helper.AddSrTcmMeterRule(edgeDisc,
                                     {.srcIp = srcAddr,
                                      .srcPort = kWildcardSrcPort,
                                      .dstIp = dstAddr,
                                      .dstPort = 23,
                                      .proto = 6,
                                      .greenDscp = 10,
                                      .yellowDscp = 10,
                                      .redDscp = 12,
                                      .cirBps = srtcmTelnetCirBps,
                                      .cbsBytes = telnetCbs});
        }

        // FTP: DP1 stays GREEN (12), OOP -> DP2 (14 / 14).
        // Plan: FTP green=12, yellow=12, red=14.
        for (uint32_t i = 0; i < numFtpRules; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            Ipv4Address srcAddr = n(src)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            helper.AddSrTcmMeterRule(edgeDisc,
                                     {.srcIp = srcAddr,
                                      .srcPort = kWildcardSrcPort,
                                      .dstIp = dstAddr,
                                      .dstPort = 21,
                                      .proto = 6,
                                      .greenDscp = 12,
                                      .yellowDscp = 12,
                                      .redDscp = 14,
                                      .cirBps = srtcmFtpCirBps,
                                      .cbsBytes = ftpCbs});
        }

        // HTTP: reclassification across all three drop precedences.
        // Plan: HTTP green=10, yellow=12, red=14.
        for (uint32_t i = 0; i < numHttpRules; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 420) + 46;
            Ipv4Address srcAddr = n(src)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            helper.AddSrTcmMeterRule(edgeDisc,
                                     {.srcIp = srcAddr,
                                      .srcPort = kWildcardSrcPort,
                                      .dstIp = dstAddr,
                                      .dstPort = 80,
                                      .proto = 6,
                                      .greenDscp = 10,
                                      .yellowDscp = 12,
                                      .redDscp = 14,
                                      .cirBps = srtcmHttpCirBps,
                                      .cbsBytes = httpCbs});
        }
    }

    // --- Policy entries: Dumb for all three AF classes, TokenBucket for BE ---
    helper.AddDumbPolicy(edgeDisc, 10);
    helper.AddDumbPolicy(edgeDisc, 12);
    helper.AddDumbPolicy(edgeDisc, 14);
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 0, .cirBps = 500000.0, .cbsBytes = 10000.0});
    helper.AddDumbPolicy(edgeDisc, 50);

    // --- Policer entries ---
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 10,
                            .downgrade1 = 10,
                            .downgrade2 = 10,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 12,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 14,
                            .downgrade1 = 14,
                            .downgrade2 = 14,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 0,
                            .downgrade1 = 50,
                            .downgrade2 = 50,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 50,
                            .downgrade1 = 50,
                            .downgrade2 = 50,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // --- PHB table: DSCP -> (queue, drop_prec) ---
    helper.AddPhbEntry(edgeInner, 10, 0, 0); // Telnet -> Q0/DP0
    helper.AddPhbEntry(edgeInner, 12, 0, 1); // FTP    -> Q0/DP1
    helper.AddPhbEntry(edgeInner, 14, 0, 2); // HTTP   -> Q0/DP2
    helper.AddPhbEntry(edgeInner, 0, 1, 0);  // BE in  -> Q1/P0
    helper.AddPhbEntry(edgeInner, 50, 1, 1); // BE out -> Q1/P1

    // --- Install edge disc on n0 -> n466 ---
    Ptr<NetDevice> e1Dev = devBottleneck.Get(0);
    stratum::InstallRoot(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    // MRED modes (after Initialize)
    edgeInner->SetMredMode(MredMode::WRED, 0);      // AF: WRED
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 1); // Default: tail drop

    // Per-queue bandwidth (for RED's EWMA)
    edgeInner->SetMeanPacketSize(1000);
    edgeInner->SetQueueBandwidth(0, 2550000.0); // 85% of 3 Mbps

    // --- RED/WRED thresholds for selected parameter set ---
    helper.ConfigQueue(edgeInner,
                       {.queue = 0,
                        .prec = 0,
                        .thMin = wred.dp0_min,
                        .thMax = wred.dp0_max,
                        .maxP = wred.dp0_maxP});
    helper.ConfigQueue(edgeInner,
                       {.queue = 0,
                        .prec = 1,
                        .thMin = wred.dp1_min,
                        .thMax = wred.dp1_max,
                        .maxP = wred.dp1_maxP});
    helper.ConfigQueue(edgeInner,
                       {.queue = 0,
                        .prec = 2,
                        .thMin = wred.dp2_min,
                        .thMax = wred.dp2_max,
                        .maxP = wred.dp2_maxP});

    // Default queue: tail-drop at QueueLimit, 1 is reject (-1,-1)
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});

    g_edgeDisc = edgeDisc;

    // ====================================================================
    // DiffServ Core (n466 -> n0): minimal, 1 queue, tail-drop
    // ====================================================================
    Ptr<CoreQueueDisc> coreDisc = CreateObject<CoreQueueDisc>();
    auto coreInner = helper.InstallRedInner(coreDisc);
    coreInner->SetNumQueues(1);
    coreInner->SetNumPrec(0, 1);

    Ptr<PriorityScheduler> corePq = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                  UintegerValue(1),
                                                                                  "WinLen",
                                                                                  DoubleValue(1.0));
    coreInner->SetScheduler(corePq);
    helper.AddPhbEntry(coreInner, 0, 0, 0);

    Ptr<NetDevice> coreDev = devBottleneck.Get(1);
    stratum::InstallRoot(coreDev, coreDisc);
    coreDisc->Initialize();
    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(coreInner,
                       {.queue = 0, .prec = 0, .thMin = 60.0, .thMax = 60.0, .maxP = 1.0});

    // ====================================================================
    // Resolve sink addresses
    // ====================================================================
    Ipv4Address addr468 = n(468)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();

    // ====================================================================
    // Traffic 1: Telnet (50 connections, active 0-50 s per thesis §4.2)
    // Source: n6-n45 -> n46-n95 (port 23)
    // ====================================================================
    uint16_t telnetPort = 23;
    uint32_t numTelnet = 50;

    for (uint32_t i = 0; i < numTelnet; i++)
    {
        uint32_t src = (i % 40) + 6;
        uint32_t dst = (i % 50) + 46;
        auto startTime = static_cast<double>(i); // 0..49 s

        Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(n(dst), telnetPort, "ns3::TcpSocketFactory");

        // Aligned to ns-2.35 Application/Telnet (MSS 512 B, exp(interval_=1 s))
        // ≈ 4 kbps per connection × 50 = ~200 kbps aggregate during 0-50 s.
        OnOffHelper telnetOnOff("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, telnetPort));
        telnetOnOff.SetAttribute("DataRate", StringValue("50kbps"));
        telnetOnOff.SetAttribute("PacketSize", UintegerValue(512));
        telnetOnOff.SetAttribute("OnTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0.16]"));
        telnetOnOff.SetAttribute("OffTime",
                                 StringValue("ns3::ExponentialRandomVariable[Mean=1.0]"));

        ApplicationContainer app = telnetOnOff.Install(n(src));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(50.0)); // thesis: "activated during first 50 seconds"
    }
    std::cout << "Telnet: " << numTelnet << " connections (n6-n45 -> n46-n95)"
              << ", active 0-50s\n";

    // ====================================================================
    // Traffic 2: FTP (50 connections, 50 KB each, active 0-50 s)
    // Source: n6-n45 -> n46-n95 (port 21)
    // ====================================================================
    uint16_t ftpPort = 21;
    uint32_t numFtp = 50;

    for (uint32_t i = 0; i < numFtp; i++)
    {
        uint32_t src = (i % 40) + 6;
        uint32_t dst = (i % 50) + 46;
        auto startTime = static_cast<double>(i);

        Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(n(dst), ftpPort, "ns3::TcpSocketFactory");

        // Finite 50 KB transfer per connection (matches ns-2 reconstruction)
        BulkSendHelper ftpBulk("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, ftpPort));
        ftpBulk.SetAttribute("MaxBytes", UintegerValue(50000));
        ftpBulk.SetAttribute("SendSize", UintegerValue(1460));

        ApplicationContainer app = ftpBulk.Install(n(src));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(50.0));
    }
    std::cout << "FTP: " << numFtp << " connections (n6-n45 -> n46-n95)"
              << ", 50 KB each, active 0-50s\n";

    // ====================================================================
    // Traffic 3: HTTP (400 bulk-TCP sessions, active full simulation)
    //
    // Thesis: 400 PagePool/WebTraf sessions. PagePool/WebTraf does not
    // exist in ns-3; we approximate with 400 BulkSendApplication streams
    // (same trade-off as the ns-2 http_session helper via Application/HTTP).
    //
    // Direction: n6-n45 (servers) -> n46-n465 (clients), same as Telnet/FTP.
    // Packets with dstPort=80 traverse the DiffServ edge (n0 -> n466)
    // outbound and are classified by the port-based MarkRule as DSCP 14.
    // In the thesis the HTTP traffic is logically server-to-client (content
    // delivery), which matches this direction.
    // ====================================================================
    uint16_t httpPort = 80;
    // HTTP session count: thesis §4.2 specifies 400; the ns-2 reconstruction
    // uses 400. Earlier the ns-3 port crashed (SIGSEGV) at >=250 concurrent
    // BulkSend TCP flows due to a TCP persist-timer null-deref upstream;
    // resolved by patches/ns3/0001-tcp-persist-empty-buffer.patch (carried
    // locally pending the upstream merge). Default now matches the ns-2
    // reference at 400. Tunable via --numHttp for experimentation.
    uint32_t numHttp = (numHttpOverride > 0) ? numHttpOverride : 400;

    for (uint32_t i = 0; i < numHttp; i++)
    {
        uint32_t src = (i % 40) + 6;                      // a server
        uint32_t dst = (i % 420) + 46;                    // a client
        double startTime = static_cast<double>(i) * 0.25; // stagger over 100 s

        Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(n(dst), httpPort, "ns3::TcpSocketFactory");

        BulkSendHelper httpBulk("ns3::TcpSocketFactory", InetSocketAddress(dstAddr, httpPort));
        httpBulk.SetAttribute("MaxBytes", UintegerValue(0)); // unlimited (bulk)
        // SendSize 1000 matches the ns-2 reference TCP segment size
        // (Agent/TCP packetSize_=1000 per src/ns-2.35/tcl/lib/ns-default.tcl).
        // Aligning the two removes a 1.46x per-segment-bytes asymmetry that
        // had been driving deltas in the cross-simulator comparison.
        httpBulk.SetAttribute("SendSize", UintegerValue(1000));

        ApplicationContainer app = httpBulk.Install(n(src));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime - 10.0));
    }
    std::cout << "HTTP: " << numHttp << " bulk-TCP sessions (n6-n45 -> n46-n465)"
              << ", staggered 0-100s, active until " << (simTime - 10) << "s\n";

    // ====================================================================
    // Traffic 4: Background CBR (n467 -> n468, 500 kbps, 512 B)
    // ====================================================================
    uint16_t bgPort = 10000;
    EnsureSink(n(468), bgPort, "ns3::UdpSocketFactory");

    OnOffHelper bgOnOff("ns3::UdpSocketFactory", InetSocketAddress(addr468, bgPort));
    bgOnOff.SetAttribute("DataRate", StringValue("500000bps"));
    bgOnOff.SetAttribute("PacketSize", UintegerValue(512));
    bgOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000000]"));
    bgOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

    ApplicationContainer bgApp = bgOnOff.Install(n(467));
    bgApp.Start(Seconds(0.0));
    bgApp.Stop(Seconds(simTime));
    std::cout << "BG: 1 CBR flow (n467 -> n468) - 500 kbps, 512 B\n";

    // ====================================================================
    // Trace files and metric recording
    // ====================================================================
    g_serviceRateFile.open(outputDir + "/ServiceRate.tr");
    g_classRateFile.open(outputDir + "/ClassRate.tr");
    g_queueLenFile.open(outputDir + "/QueueLen.tr");

    Simulator::Schedule(Seconds(0.0), &RecordServiceRateFull);
    Simulator::Schedule(Seconds(0.0), &RecordClassRateFull);
    Simulator::Schedule(Seconds(0.0), &RecordQueueLengthFull);

    // ====================================================================
    // Print configuration summary
    // ====================================================================
    std::cout << "\n--- Scenario 2 Fullscale: 469-node AF PHB WRED sweep ---\n"
              << "paramSet:   " << paramSet << " ("
              << ((paramSet <= 2)   ? "staggered"
                  : (paramSet <= 4) ? "partially overlapped"
                                    : "overlapped")
              << ")\n"
              << "Bottleneck: 3 Mbps / 20 ms (n0 -> n466)\n"
              << "Scheduler:  SFQ 17:3 (AF:Default) on 3 Mbps\n"
              << "Queues:     Q0 AF (3 prec, WRED), Q1 Default (2 prec, TB+Drop)\n"
              << "WRED Q0 DP0=(" << wred.dp0_min << "," << wred.dp0_max << "," << wred.dp0_maxP
              << ")\n"
              << "     Q0 DP1=(" << wred.dp1_min << "," << wred.dp1_max << "," << wred.dp1_maxP
              << ")\n"
              << "     Q0 DP2=(" << wred.dp2_min << "," << wred.dp2_max << "," << wred.dp2_maxP
              << ")\n"
              << "Simulation: " << simTime << " s\n"
              << "Output:     " << outputDir << "\n\n";

    // ====================================================================
    // Statistics monitor — collects per-DSCP byte counters split by
    // TcpRetransmitTag presence so the post-run print can derive thesis
    // goodput = origBytes / (origBytes + retxBytes). The helper subscribes
    // to edgeDisc's StratumEnqueue/StratumDrop traces; the queue disc's own internal
    // counters (printed via edgeDisc->PrintStats()) remain authoritative
    // for the legacy "Packets Statistics" view.
    // ====================================================================
    MonitorHelper monitor;
    monitor.Install(edgeDisc);

    // ====================================================================
    // Run
    // ====================================================================
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    edgeDisc->PrintStats();
    monitor.PrintStats();

    g_serviceRateFile.close();
    g_classRateFile.close();
    g_queueLenFile.close();

    Simulator::Destroy();
    return 0;
}

// ---------------------------------------------------------------------------
// main: parse CLI and dispatch to the requested scale.
// ---------------------------------------------------------------------------

int
main(int argc, char* argv[])
{
    // ---- Scale selector ----
    std::string scale = "quick";

    // ---- Quick-scale arguments (unchanged defaults) ----
    std::string scheduler = "PQ";
    uint32_t efPacketSize = 1300;
    double simTime = 100.0;
    uint32_t seed = 42;
    std::string outputDir = "output/ns3/example-2";

    // ---- Full-scale arguments (preserved defaults from the retired binary) ----
    double simTimeFull = 5000.0;
    uint32_t paramSet = 1;
    std::string outputDirFull = "output/ns3/example-2-fullscale";
    uint32_t numHttpOverride = 0;
    bool useStockQueue = false;
    std::string classifierMode = "port-based";
    double srtcmTelnetCirBps = 50000.0;
    double srtcmFtpCirBps = 160000.0;
    double srtcmHttpCirBps = 6375.0;
    double srtcmCbsSeconds = 1.0;
    std::string tcpVariant;
    int tcpSack = -1;
    int tcpTimestamp = -1;
    int tcpWindowScaling = -1;
    uint32_t tcpMaxWindowSize = 0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("scale", "Scenario scale: 'quick' (default) or 'full'", scale);
    // Shared across both scales
    cmd.AddValue("seed", "RNG seed", seed);
    // Quick-scale flags
    cmd.AddValue("scheduler", "Scheduler type (quick scale): PQ, SCFQ, LLQ", scheduler);
    cmd.AddValue("efPacketSize", "EF packet size in bytes (quick scale)", efPacketSize);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("outputDir", "Output directory for traces", outputDir);
    // Full-scale flags
    cmd.AddValue("paramSet", "WRED parameter set 1..6 (full scale)", paramSet);
    cmd.AddValue("numHttp",
                 "Number of HTTP sessions (full scale; 0 = use default 400)",
                 numHttpOverride);
    cmd.AddValue("stockQueue",
                 "Full-scale diagnostic: replace the DiffServ edge queue with stock "
                 "ns-3 RedQueueDisc (no classification, no AF/Default differentiation). "
                 "Used to isolate whether scaling crashes originate in DiffServ code or "
                 "in ns-3 core.",
                 useStockQueue);
    cmd.AddValue("classifier",
                 "Full-scale classifier mode: 'port-based' (default) or 'srtcm' "
                 "(per-flow rate-metered)",
                 classifierMode);
    cmd.AddValue("srtcmTelnetCir",
                 "srTCM Telnet per-flow CIR in bps (full scale)",
                 srtcmTelnetCirBps);
    cmd.AddValue("srtcmFtpCir", "srTCM FTP per-flow CIR in bps (full scale)", srtcmFtpCirBps);
    cmd.AddValue("srtcmHttpCir", "srTCM HTTP per-flow CIR in bps (full scale)", srtcmHttpCirBps);
    cmd.AddValue("srtcmCbsSeconds", "srTCM CBS window in seconds (full scale)", srtcmCbsSeconds);
    cmd.AddValue("tcpVariant",
                 "TCP variant TypeId, e.g. ns3::TcpLinuxReno (full scale; empty = "
                 "ns-3 default TcpCubic)",
                 tcpVariant);
    cmd.AddValue("tcpSack", "Override TCP SACK (full scale): -1=default, 0=off, 1=on", tcpSack);
    cmd.AddValue("tcpTimestamp",
                 "Override TCP Timestamp (full scale): -1=default, 0=off, 1=on",
                 tcpTimestamp);
    cmd.AddValue("tcpWindowScaling",
                 "Override TCP WindowScaling (full scale): -1=default, 0=off, 1=on",
                 tcpWindowScaling);
    cmd.AddValue("tcpMaxWindowSize",
                 "Override TCP MaxWindowSize in bytes (full scale; 0 = ns-3 default 65535)",
                 tcpMaxWindowSize);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(scale != "quick" && scale != "full",
                    "scale must be 'quick' or 'full' (got '" << scale << "')");

    if (scale == "full")
    {
        // Honour --simTime if supplied; otherwise fall back to the full-scale
        // default of 5000 s. The CommandLine API gives no "was set?" probe, so
        // detect the quick default and substitute.
        double effSimTime = (simTime == 100.0) ? simTimeFull : simTime;
        // Same idea for --outputDir (quick default carries example-2 directory).
        std::string effOutputDir =
            (outputDir == "output/ns3/example-2") ? outputDirFull : outputDir;

        return RunFullScenario(effSimTime,
                               seed,
                               paramSet,
                               effOutputDir,
                               numHttpOverride,
                               useStockQueue,
                               classifierMode,
                               srtcmTelnetCirBps,
                               srtcmFtpCirBps,
                               srtcmHttpCirBps,
                               srtcmCbsSeconds,
                               tcpVariant,
                               tcpSack,
                               tcpTimestamp,
                               tcpWindowScaling,
                               tcpMaxWindowSize);
    }

    return RunQuickScenario(scheduler, efPacketSize, simTime, seed, outputDir);
}
