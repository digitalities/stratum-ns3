/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Reconstruction of thesis Scenario 3 (Section 4.3): a complete DiffServ
 * service model with Premium (EF), Olympic (Gold/Silver/Bronze AF tiers),
 * and Best Effort services.
 *
 * Two scales are available, selected via --scale:
 *
 *   --scale=quick (default)
 *     13-node topology (shared with examples 1 and 2). Exercises the
 *     full 5-class service model described in Table 4.5 of the thesis
 *     at small scale for fast turnaround. The original Tcl script for
 *     this scaled scenario was never published.
 *
 *   --scale=full
 *     771-node thesis-scale reconstruction matching the original ns-2
 *     scenario-3.tcl. 200 VoIP (G.723.1), 300 RealAudio (empirical
 *     CDFs), 50 Telnet, 50 FTP, 50 HTTP, 1 background CBR. Bottleneck
 *     3 Mbps / 20 ms; ~5000 s simulated time.
 *
 * Service model (Table 4.5):
 *   Q0  Premium   EF         VoIP (G.723.1)    TB CIR=500k  Drop
 *   Q1  Gold      AF11/AF12  Audio streaming    TSW2CM 600k  RIO-C
 *   Q2  Silver    AF21/AF22  Telnet + FTP       Dumb         WRED
 *   Q3  Bronze    AF31       HTTP               Dumb         WRED
 *   Q4  Best Eff  Default    Background         TB CIR=400k  Drop
 *
 * Scheduler: LLQ (PQ for Q0, SFQ weights 3:3:3:1 for Q1-Q4)
 *
 * @see thesis Section 4.3, Table 4.5, Appendix C
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
#include "ns3/stratum-empirical-cdf-loader.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-llq-scheduler.h"
#include "ns3/stratum-onoff-application.h"
#include "ns3/stratum-onoff-helper.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <utility>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::CoreQueueDisc;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::kAnyPort;
using ns3::stratum::LlqScheduler;
using ns3::stratum::LoadEmpiricalCdfFromFile;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::PriorityScheduler;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("DiffServExample3");

// ---------------------------------------------------------------------------
// Global state for metric recording (shared by both scales)
// ---------------------------------------------------------------------------

static Ptr<EdgeQueueDisc> g_edgeDisc;
static std::ofstream g_serviceRateFile; ///< Per-service departure rates
static std::ofstream g_classRateFile;   ///< Per-class departure rates
static std::ofstream g_queueLenFile;    ///< Per-queue packet count
static std::ofstream g_owdFile;         ///< VoIP one-way delay
static std::ofstream g_ipdvFile;        ///< VoIP inter-packet delay variation

// OWD/IPDV tracking for VoIP flow
static double g_sumOwd = 0.0;  ///< Running sum (for final-mean reporting)
static double g_sumIpdv = 0.0; ///< Running sum (for final-mean reporting)
static uint64_t g_owdPktCount = 0;
static double g_previousOwd = -1.0;
static double g_latestOwd = 0.0;  ///< Most recent packet OWD (full-scale per-packet trace)
static double g_latestIpdv = 0.0; ///< Most recent packet IPDV (full-scale per-packet trace)
static double g_txTimeMs = 0.0;

// Sink dedup: avoid installing multiple PacketSinks on the same (node, port).
// Used by the full scale to support the dense 771-node sink installation.
static std::set<std::pair<uint32_t, uint16_t>> g_installedSinks;

// ---------------------------------------------------------------------------
// DiffServCbrApplication: CBR source with SendTimeTag for OWD tracking
// (used by the quick scale's VoIP flow)
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
          m_pktSize(48),
          m_dataRate(6400),
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
// VoIP Rx callback: compute OWD and IPDV
// ---------------------------------------------------------------------------
static void
VoipRxCallback(Ptr<const Packet> packet, const Address& /* addr */)
{
    SendTimeTag tag;
    if (packet->PeekPacketTag(tag))
    {
        double owd = Simulator::Now().GetSeconds() - tag.GetSendTime();
        g_latestOwd = owd;
        g_sumOwd += owd;
        g_owdPktCount++;

        if (g_previousOwd >= 0.0)
        {
            double ipdv = std::abs(owd - g_previousOwd);
            g_latestIpdv = ipdv;
            g_sumIpdv += ipdv;
        }
        g_previousOwd = owd;
    }
}

// ---------------------------------------------------------------------------
// Periodic metric recording — service rate (shared, schedules same callback
// across both scales because the queue layout is identical)
// ---------------------------------------------------------------------------

/// Per-service departure rate every 1.0 s. Matches Appendix C Figure C.1.
static void
RecordServiceRate()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    double premiumRate = sched->GetDepartureRate(0, -1) / 1000.0;
    double goldRate = sched->GetDepartureRate(1, -1) / 1000.0;
    double silverRate = sched->GetDepartureRate(2, -1) / 1000.0;
    double bronzeRate = sched->GetDepartureRate(3, -1) / 1000.0;
    double beRate = sched->GetDepartureRate(4, -1) / 1000.0;

    g_serviceRateFile << now << " " << premiumRate << " " << goldRate << " " << silverRate << " "
                      << bronzeRate << " " << beRate << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordServiceRate);
}

/// Per-class departure rate every 1.0 s. Matches Appendix C Figure C.2.
static void
RecordClassRate()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    double voipRate = sched->GetDepartureRate(0, 0) / 1000.0;
    double streamRate = sched->GetDepartureRate(1, -1) / 1000.0;
    double telnetRate = sched->GetDepartureRate(2, 0) / 1000.0;
    double ftpRate = sched->GetDepartureRate(2, 1) / 1000.0;
    double httpRate = sched->GetDepartureRate(3, -1) / 1000.0;
    double bgRate = sched->GetDepartureRate(4, -1) / 1000.0;

    g_classRateFile << now << " " << voipRate << " " << streamRate << " " << telnetRate << " "
                    << ftpRate << " " << httpRate << " " << bgRate << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordClassRate);
}

/// Per-queue packet count every 0.5 s. Matches Appendix C Figure C.3.
static void
RecordQueueLength()
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

    g_queueLenFile << now << " " << getQLen(0) << " " << getQLen(1) << " " << getQLen(2) << " "
                   << getQLen(3) << " " << getQLen(4) << "\n";
    Simulator::Schedule(Seconds(0.5), &RecordQueueLength);
}

/// Cumulative-mean OWD/IPDV trace (quick scale). Matches Appendix C Figure C.5.
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

/**
 * Per-packet OWD/IPDV trace (full scale).
 *
 * Writes the *most-recent-packet* values (g_latestOwd / g_latestIpdv) to
 * match ns-2's LossMonitor::owd_ / ipdv_ semantics (see
 * src/ns-2.35/tools/loss-monitor.cc): owd_ is overwritten on each received
 * packet with that packet's instantaneous OWD, and the Tcl scenario's
 * record_delay proc reads it every 0.5 s. This preserves per-packet jitter
 * variance in the trace; emitting a cumulative mean instead would average
 * it into a near-flat line.
 *
 * g_sumOwd / g_sumIpdv / g_owdPktCount are retained as running accumulators
 * for population-mean reporting (e.g. post-sim summaries) but are no longer
 * fed into the trace output.
 */
static void
RecordDelayFull()
{
    double now = Simulator::Now().GetSeconds();

    if (g_owdPktCount >= 2)
    {
        g_owdFile << now << " " << (g_latestOwd * 1000.0 - g_txTimeMs) << "\n";
        g_ipdvFile << now << " " << g_latestIpdv * 1000.0 << "\n";
    }
    else
    {
        g_owdFile << now << " 0\n";
        g_ipdvFile << now << " 0\n";
    }
    Simulator::Schedule(Seconds(0.5), &RecordDelayFull);
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
// Quick-scale scenario runner: 13-node, 5-class service model.
// ---------------------------------------------------------------------------
static int
RunQuickScenario(double simTime, uint32_t seed, const std::string& outputDir)
{
    diffserv::EnsureDir(outputDir);

    // ---- RNG ----
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    // VoIP packet size for txTime correction (48 bytes = 2 × G.723.1 frames)
    uint32_t voipPktSize = 48;
    g_txTimeMs = static_cast<double>(voipPktSize) * 8.0 / 2000.0;

    Ptr<UniformRandomVariable> rndStart = CreateObject<UniformRandomVariable>();
    rndStart->SetAttribute("Min", DoubleValue(0.0));
    rndStart->SetAttribute("Max", DoubleValue(5.0));

    // ---- Nodes ----
    NodeContainer sources;
    sources.Create(5);

    NodeContainer routers;
    routers.Create(3);

    NodeContainer destinations;
    destinations.Create(5);

    Ptr<Node> e1 = routers.Get(0);
    Ptr<Node> core = routers.Get(1);
    Ptr<Node> e2 = routers.Get(2);

    // ---- Links ----
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));

    std::vector<NetDeviceContainer> srcDevs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        srcDevs[i] = p2pAccess.Install(sources.Get(i), e1);
    }

    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devE1Core = p2pBottleneck.Install(e1, core);

    PointToPointHelper p2pCoreE2;
    p2pCoreE2.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    p2pCoreE2.SetChannelAttribute("Delay", StringValue("3ms"));
    NetDeviceContainer devCoreE2 = p2pCoreE2.Install(core, e2);

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

    std::vector<Ipv4InterfaceContainer> srcIfs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        std::string base = "10.0." + std::to_string(subnetIdx++) + ".0";
        addr.SetBase(base.c_str(), "255.255.255.0");
        srcIfs[i] = addr.Assign(srcDevs[i]);
    }

    addr.SetBase(("10.0." + std::to_string(subnetIdx++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifE1Core = addr.Assign(devE1Core);

    addr.SetBase(("10.0." + std::to_string(subnetIdx++) + ".0").c_str(), "255.255.255.0");
    Ipv4InterfaceContainer ifCoreE2 = addr.Assign(devCoreE2);

    std::vector<Ipv4InterfaceContainer> dstIfs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        std::string base = "10.0." + std::to_string(subnetIdx++) + ".0";
        addr.SetBase(base.c_str(), "255.255.255.0");
        dstIfs[i] = addr.Assign(dstDevs[i]);
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Source/destination addresses for classification
    Ipv4Address srcAddr0 = srcIfs[0].GetAddress(0); // s(0) = VoIP sources

    // ====================================================================
    // DiffServ Edge: complete service model (thesis Table 4.5)
    //
    //   Q0  Premium   EF         TB 500k/10KB     Drop
    //   Q1  Gold      AF11/AF12  TSW2CM 600k      RIO-C
    //   Q2  Silver    AF21/AF22  Dumb             WRED
    //   Q3  Bronze    AF31       Dumb             WRED
    //   Q4  Best Eff  Default    TB 400k/2KB      Drop
    //
    //   Scheduler: LLQ (PQ for Q0, SFQ 3:3:3:1 for Q1-Q4)
    // ====================================================================

    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    // --- Physical queues ---
    edgeInner->SetNumQueues(5);
    edgeInner->SetNumPrec(0, 2); // Premium: 2 prec (in/out-profile)
    edgeInner->SetNumPrec(1, 2); // Gold: 2 prec (AF11/AF12)
    edgeInner->SetNumPrec(2, 2); // Silver: 2 prec (AF21/AF22)
    edgeInner->SetNumPrec(3, 1); // Bronze: 1 prec (AF31 only)
    edgeInner->SetNumPrec(4, 2); // BE: 2 prec (in/out-profile)

    // Queue limits
    edgeInner->SetQueueLimit(0, 20);  // Premium: small (VoIP = low latency)
    edgeInner->SetQueueLimit(1, 100); // Gold
    edgeInner->SetQueueLimit(2, 100); // Silver
    edgeInner->SetQueueLimit(3, 100); // Bronze
    edgeInner->SetQueueLimit(4, 50);  // BE

    // --- Scheduler: LLQ with SFQ (thesis: "LLQ ... SFQ is the selected
    // sub-scheduler") --- Bandwidth for FQ sub-scheduler: ~1.5 Mbps (2 Mbps minus
    // ~500 kbps Premium)
    auto llq = CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                        UintegerValue(5),
                                                        "LinkBandwidth",
                                                        DoubleValue(1500000.0),
                                                        "FqVariant",
                                                        EnumValue(LlqScheduler::FqVariant::SFQ));
    // SFQ weights: Gold 30%, Silver 30%, Bronze 30%, BE 10%
    llq->SetParam(1, 3.0); // Gold
    llq->SetParam(2, 3.0); // Silver
    llq->SetParam(3, 3.0); // Bronze
    llq->SetParam(4, 1.0); // BE
    edgeInner->SetScheduler(llq);

    // --- Mark rules (port-based classification) ---
    // Rule 1: VoIP from s(0) → DSCP 46 (Premium/EF)
    edgeDisc->AddMarkRule({.dscp = 46, .srcAddr = srcAddr0});
    // Rule 2: Audio streaming (dstPort 5004) → DSCP 10 (Gold/AF11)
    edgeDisc->AddMarkRule({.dscp = 10, .dstPort = 5004});
    // Rule 3: Telnet (dstPort 23) → DSCP 18 (Silver/AF21)
    edgeDisc->AddMarkRule({.dscp = 18, .dstPort = 23});
    // Rule 4: FTP (dstPort 21) → DSCP 20 (Silver/AF22)
    edgeDisc->AddMarkRule({.dscp = 20, .dstPort = 21});
    // Rule 5: HTTP (dstPort 80) → DSCP 26 (Bronze/AF31)
    edgeDisc->AddMarkRule({.dscp = 26, .dstPort = 80});
    // Default: unmatched → DSCP 0 (Best Effort)

    // --- Policy entries ---
    // Premium: TokenBucket CIR=500kbps CBS=10KB
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 46, .cirBps = 500000.0, .cbsBytes = 10000.0});
    helper.AddDumbPolicy(edgeDisc, 51);

    // Gold: TSW2CM CIR=600kbps (thesis: "CIR 600 kbps")
    helper.AddTsw2cmPolicy(edgeDisc, {.codePt = 10, .cirBps = 600000.0});
    helper.AddDumbPolicy(edgeDisc, 12);

    // Silver: Dumb (no metering for Telnet or FTP)
    helper.AddDumbPolicy(edgeDisc, 18);
    helper.AddDumbPolicy(edgeDisc, 20);

    // Bronze: Dumb (no metering for HTTP)
    helper.AddDumbPolicy(edgeDisc, 26);

    // Best Effort: TokenBucket CIR=400kbps CBS=2KB
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 0, .cirBps = 400000.0, .cbsBytes = 2000.0});
    helper.AddDumbPolicy(edgeDisc, 50);

    // --- Policer entries ---
    // Premium: TB 46 → 51 (out-of-profile)
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

    // Gold: TSW2CM 10 → 12 (downgrade)
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TSW2CM,
                            .initialCodePt = 10,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 12,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // Silver: Dumb (no downgrade)
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 18,
                            .downgrade1 = 18,
                            .downgrade2 = 18,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 20,
                            .downgrade1 = 20,
                            .downgrade2 = 20,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // Bronze: Dumb
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 26,
                            .downgrade1 = 26,
                            .downgrade2 = 26,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // Best Effort: TB 0 → 50
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
    helper.AddPhbEntry(edgeInner, 46, 0, 0); // EF in → Q0/P0
    helper.AddPhbEntry(edgeInner, 51, 0, 1); // EF out → Q0/P1

    helper.AddPhbEntry(edgeInner, 10, 1, 0); // AF11 (stream in) → Q1/P0
    helper.AddPhbEntry(edgeInner, 12, 1, 1); // AF12 (stream out) → Q1/P1

    helper.AddPhbEntry(edgeInner, 18, 2, 0); // AF21 (Telnet) → Q2/P0
    helper.AddPhbEntry(edgeInner, 20, 2, 1); // AF22 (FTP) → Q2/P1

    helper.AddPhbEntry(edgeInner, 26, 3, 0); // AF31 (HTTP) → Q3/P0

    helper.AddPhbEntry(edgeInner, 0, 4, 0);  // BE in → Q4/P0
    helper.AddPhbEntry(edgeInner, 50, 4, 1); // BE out → Q4/P1

    // --- Install edge disc ---
    Ptr<NetDevice> e1Dev = devE1Core.Get(0);
    stratum::InstallRoot(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    // --- MRED modes (per-queue, AFTER Initialize) ---
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 0); // Premium: tail drop
    edgeInner->SetMredMode(MredMode::RIO_C, 1);     // Gold: RIO-C
    edgeInner->SetMredMode(MredMode::WRED, 2);      // Silver: WRED
    edgeInner->SetMredMode(MredMode::WRED, 3);      // Bronze: WRED
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 4); // BE: tail drop

    // --- Per-queue bandwidth for RED (thesis pp. 74-75: the setQueueBW fix) ---
    edgeInner->SetMeanPacketSize(500);         // Weighted avg across traffic types
    edgeInner->SetQueueBandwidth(1, 450000.0); // Gold: 30% of 1.5 Mbps
    edgeInner->SetQueueBandwidth(2, 450000.0); // Silver: 30% of 1.5 Mbps
    edgeInner->SetQueueBandwidth(3, 450000.0); // Bronze: 30% of 1.5 Mbps

    // --- RED/DROP thresholds (thesis Table 4.5) ---
    // Q0 Premium: tail-drop
    helper.ConfigQueue(
        edgeInner,
        {.queue = 0, .prec = 0, .thMin = 20.0, .thMax = 20.0, .maxP = 1.0}); // accept up to limit
    helper.ConfigQueue(edgeInner,
                       {.queue = 0,
                        .prec = 1,
                        .thMin = -1.0,
                        .thMax = -1.0,
                        .maxP = 0.0}); // drop all out-of-profile

    // Q1 Gold: RIO-C (thesis: "Green: 60,110,0.02 / Yellow: 30,60,0.6")
    helper.ConfigQueue(
        edgeInner,
        {.queue = 1, .prec = 0, .thMin = 60.0, .thMax = 110.0, .maxP = 0.02}); // AF11 green
    helper.ConfigQueue(
        edgeInner,
        {.queue = 1, .prec = 1, .thMin = 30.0, .thMax = 60.0, .maxP = 0.6}); // AF12 yellow

    // Q2 Silver: WRED (thesis: "30,50 / 0.1,0.2")
    helper.ConfigQueue(
        edgeInner,
        {.queue = 2, .prec = 0, .thMin = 30.0, .thMax = 50.0, .maxP = 0.1}); // AF21 Telnet
    helper.ConfigQueue(
        edgeInner,
        {.queue = 2, .prec = 1, .thMin = 30.0, .thMax = 50.0, .maxP = 0.2}); // AF22 FTP

    // Q3 Bronze: WRED (thesis: "30,60 / 0.5")
    helper.ConfigQueue(
        edgeInner,
        {.queue = 3, .prec = 0, .thMin = 30.0, .thMax = 60.0, .maxP = 0.5}); // AF31 HTTP

    // Q4 Best Effort: tail-drop
    helper.ConfigQueue(edgeInner,
                       {.queue = 4, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 4,
                        .prec = 1,
                        .thMin = -1.0,
                        .thMax = -1.0,
                        .maxP = 0.0}); // drop all out-of-profile

    g_edgeDisc = edgeDisc;

    // ====================================================================
    // DiffServ Core (core -> e1): minimal, 1 queue, tail-drop
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

    Ptr<NetDevice> coreDev = devE1Core.Get(1);
    stratum::InstallRoot(coreDev, coreDisc);
    coreDisc->Initialize();
    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(coreInner,
                       {.queue = 0, .prec = 0, .thMin = 60.0, .thMax = 60.0, .maxP = 1.0});

    // ====================================================================
    // Traffic 1: Premium / VoIP (G.723.1 codec)
    //
    // Thesis: 6.3 kbps, 2 × 30ms frames per packet (48 bytes payload),
    // exponential ON/OFF (ON=1.004s, OFF=1.587s), 200 connections.
    // Scaled down to 10 connections from s(0) → dest(0).
    // ====================================================================
    uint16_t voipPort = 5060;
    uint32_t numVoip = 10;

    // VoIP sink
    PacketSinkHelper voipSink("ns3::UdpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), voipPort));
    ApplicationContainer voipSinkApp = voipSink.Install(destinations.Get(0));
    voipSinkApp.Start(Seconds(0.0));
    voipSinkApp.Stop(Seconds(simTime));
    voipSinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeCallback(&VoipRxCallback));

    for (uint32_t i = 0; i < numVoip; i++)
    {
        double startTime = rndStart->GetValue();

        // VoIP: OnOff (UDP) with G.723.1 profile
        OnOffHelper voipOnOff("ns3::UdpSocketFactory",
                              InetSocketAddress(dstIfs[0].GetAddress(1), voipPort));
        voipOnOff.SetAttribute("DataRate", StringValue("6400bps"));
        voipOnOff.SetAttribute("PacketSize", UintegerValue(voipPktSize));
        voipOnOff.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=1.004]"));
        voipOnOff.SetAttribute("OffTime",
                               StringValue("ns3::ExponentialRandomVariable[Mean=1.587]"));

        ApplicationContainer app = voipOnOff.Install(sources.Get(0));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime));
    }

    std::cout << "VoIP: " << numVoip << " connections s(0)->dest(0)"
              << " - G.723.1 6.3kbps ON/OFF\n";

    // ====================================================================
    // Traffic 2: Gold / Audio streaming (RealAudio-like)
    //
    // Thesis: 245-byte packets, bursty model (burst 0.05ms, idle 1800ms).
    // Approximated with OnOff (UDP): short bursts, long idle.
    // 5 connections from s(1) → dest(1).
    // ====================================================================
    uint16_t streamPort = 5004;
    uint32_t numStream = 5;

    PacketSinkHelper streamSink("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), streamPort));
    ApplicationContainer streamSinkApp = streamSink.Install(destinations.Get(1));
    streamSinkApp.Start(Seconds(0.0));
    streamSinkApp.Stop(Seconds(simTime));

    for (uint32_t i = 0; i < numStream; i++)
    {
        double startTime = rndStart->GetValue();

        OnOffHelper streamOnOff("ns3::UdpSocketFactory",
                                InetSocketAddress(dstIfs[1].GetAddress(1), streamPort));
        streamOnOff.SetAttribute("DataRate", StringValue("200kbps"));
        streamOnOff.SetAttribute("PacketSize", UintegerValue(245));
        streamOnOff.SetAttribute("OnTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0.5]"));
        streamOnOff.SetAttribute("OffTime",
                                 StringValue("ns3::ExponentialRandomVariable[Mean=1.8]"));

        ApplicationContainer app = streamOnOff.Install(sources.Get(1));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime));
    }

    std::cout << "Streaming: " << numStream << " connections s(1)->dest(1)"
              << " - RealAudio-like bursty UDP\n";

    // ====================================================================
    // Traffic 3: Silver / Telnet + FTP
    //
    // Thesis: 50 Telnet + 50 FTP. Scaled to 4 + 4 from s(2) → dest(2).
    // ====================================================================
    uint16_t telnetPort = 23;
    uint16_t ftpPort = 21;

    // Install sinks once per (node, port)
    PacketSinkHelper telnetSink("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), telnetPort));
    ApplicationContainer tsSink = telnetSink.Install(destinations.Get(2));
    tsSink.Start(Seconds(0.0));
    tsSink.Stop(Seconds(simTime));

    PacketSinkHelper ftpSink("ns3::TcpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), ftpPort));
    ApplicationContainer fsSink = ftpSink.Install(destinations.Get(2));
    fsSink.Start(Seconds(0.0));
    fsSink.Stop(Seconds(simTime));

    // 4 Telnet connections
    for (uint32_t i = 0; i < 4; i++)
    {
        auto startTime = static_cast<double>(i);

        OnOffHelper telnetOnOff("ns3::TcpSocketFactory",
                                InetSocketAddress(dstIfs[2].GetAddress(1), telnetPort));
        telnetOnOff.SetAttribute("DataRate", StringValue("50kbps"));
        telnetOnOff.SetAttribute("PacketSize", UintegerValue(512));
        telnetOnOff.SetAttribute("OnTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0.01]"));
        telnetOnOff.SetAttribute("OffTime",
                                 StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));

        ApplicationContainer app = telnetOnOff.Install(sources.Get(2));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime));
    }

    // 4 FTP connections
    for (uint32_t i = 0; i < 4; i++)
    {
        auto startTime = static_cast<double>(i);

        BulkSendHelper ftpBulk("ns3::TcpSocketFactory",
                               InetSocketAddress(dstIfs[2].GetAddress(1), ftpPort));
        ftpBulk.SetAttribute("MaxBytes", UintegerValue(0));
        ftpBulk.SetAttribute("SendSize", UintegerValue(1460));

        ApplicationContainer app = ftpBulk.Install(sources.Get(2));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime));
    }

    std::cout << "Silver: 4 Telnet + 4 FTP s(2)->dest(2)\n";

    // ====================================================================
    // Traffic 4: Bronze / HTTP (web traffic)
    //
    // Thesis: 400 HTTP sessions. Scaled to 5 TCP OnOff connections
    // approximating bursty web page downloads from s(3) → dest(3).
    // ====================================================================
    uint16_t httpPort = 80;

    PacketSinkHelper httpSink("ns3::TcpSocketFactory",
                              InetSocketAddress(Ipv4Address::GetAny(), httpPort));
    ApplicationContainer httpSinkApp = httpSink.Install(destinations.Get(3));
    httpSinkApp.Start(Seconds(0.0));
    httpSinkApp.Stop(Seconds(simTime));

    for (uint32_t i = 0; i < 5; i++)
    {
        double startTime = rndStart->GetValue();

        OnOffHelper httpOnOff("ns3::TcpSocketFactory",
                              InetSocketAddress(dstIfs[3].GetAddress(1), httpPort));
        httpOnOff.SetAttribute("DataRate", StringValue("500kbps"));
        httpOnOff.SetAttribute("PacketSize", UintegerValue(1460));
        // Web-like: short burst (page download), then reading pause
        httpOnOff.SetAttribute("OnTime", StringValue("ns3::ExponentialRandomVariable[Mean=1.0]"));
        httpOnOff.SetAttribute("OffTime", StringValue("ns3::ExponentialRandomVariable[Mean=5.0]"));

        ApplicationContainer app = httpOnOff.Install(sources.Get(3));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime));
    }

    std::cout << "HTTP: 5 connections s(3)->dest(3) - web-like TCP\n";

    // ====================================================================
    // Traffic 5: Best Effort / Background CBR
    //
    // 10 UDP CBR flows from s(4) → dest(4), 100 kbps each
    // ====================================================================
    uint16_t bgPort = 10000;

    PacketSinkHelper bgSink("ns3::UdpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), bgPort));
    ApplicationContainer bgSinkApp = bgSink.Install(destinations.Get(4));
    bgSinkApp.Start(Seconds(0.0));
    bgSinkApp.Stop(Seconds(simTime));

    uint32_t bgPktSize = 64;
    for (uint32_t i = 0; i < 10; i++)
    {
        double startTime = rndStart->GetValue();

        OnOffHelper bgOnOff("ns3::UdpSocketFactory",
                            InetSocketAddress(dstIfs[4].GetAddress(1), bgPort));
        bgOnOff.SetAttribute("DataRate", StringValue("100000bps"));
        bgOnOff.SetAttribute("PacketSize", UintegerValue(bgPktSize));
        bgOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1000]"));
        bgOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer app = bgOnOff.Install(sources.Get(4));
        app.Start(Seconds(startTime));
        app.Stop(Seconds(simTime));

        bgPktSize += 128; // 64, 192, ..., 1216
    }

    std::cout << "BG: 10 CBR flows s(4)->dest(4) - 100kbps each\n";

    // ====================================================================
    // Trace files and metric recording
    // ====================================================================
    g_serviceRateFile.open(outputDir + "/ServiceRate.tr");
    g_classRateFile.open(outputDir + "/ClassRate.tr");
    g_queueLenFile.open(outputDir + "/QueueLen.tr");
    g_owdFile.open(outputDir + "/OWD.tr");
    g_ipdvFile.open(outputDir + "/IPDV.tr");

    Simulator::Schedule(Seconds(0.0), &RecordServiceRate);
    Simulator::Schedule(Seconds(0.0), &RecordClassRate);
    Simulator::Schedule(Seconds(6.0), &RecordQueueLength);
    Simulator::Schedule(Seconds(6.0), &RecordDelayQuick);

    // ====================================================================
    // Print configuration
    // ====================================================================
    std::cout << "\n--- Scenario 3: Complete Service Model ---\n"
              << "Scheduler: LLQ (PQ + SFQ weights 3:3:3:1)\n"
              << "Queues: 5 (Premium/Gold/Silver/Bronze/BE)\n"
              << "Premium: EF, TB 500k/10KB, Drop\n"
              << "Gold:    AF11/AF12, TSW2CM 600k, RIO-C\n"
              << "Silver:  AF21/AF22, Dumb, WRED\n"
              << "Bronze:  AF31, Dumb, WRED\n"
              << "BE:      Default, TB 400k/2KB, Drop\n"
              << "Simulation time: " << simTime << " s\n\n";

    // ====================================================================
    // Run
    // ====================================================================
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    edgeDisc->PrintStats();

    g_serviceRateFile.close();
    g_classRateFile.close();
    g_queueLenFile.close();
    g_owdFile.close();
    g_ipdvFile.close();

    Simulator::Destroy();

    return 0;
}

// ---------------------------------------------------------------------------
// Full-scale scenario runner: 771-node, ns-2 scenario-3.tcl reconstruction.
// ---------------------------------------------------------------------------
static int
RunFullScenario(double simTime,
                uint32_t seed,
                const std::string& outputDir,
                std::string cdfDir,
                bool realAudioCbr = false,
                bool deterministicLoad = false)
{
    if (!cdfDir.empty() && cdfDir.back() != '/')
    {
        cdfDir.push_back('/');
    }

    diffserv::EnsureDir(outputDir);

    // ---- RNG ----
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    // VoIP packet size for txTime correction (48 bytes = 2 x G.723.1 frames)
    uint32_t voipPktSize = 48;
    // txTime on the 3 Mbps bottleneck link
    g_txTimeMs = static_cast<double>(voipPktSize) * 8.0 / 3000.0;

    Ptr<UniformRandomVariable> rndDelay = CreateObject<UniformRandomVariable>();
    rndDelay->SetAttribute("Min", DoubleValue(10.0));
    rndDelay->SetAttribute("Max", DoubleValue(100.0));

    Ptr<UniformRandomVariable> rndBw = CreateObject<UniformRandomVariable>();
    rndBw->SetAttribute("Min", DoubleValue(22.0));
    rndBw->SetAttribute("Max", DoubleValue(32.0));

    // RealAudio empirical CDFs — port of ns-2 scenario-3.tcl §Traffic 2.
    //   userintercdf1 — inter-user-arrival (seconds)
    //   sflowcdf      — number of sequential flows per user (count)
    //   flowdurcdf    — flow duration (minutes, scaled ×60 to seconds)
    //   fratecdf      — per-flow emission rate (kbps)
    Ptr<EmpiricalRandomVariable> rvUserInter = LoadEmpiricalCdfFromFile(cdfDir + "userintercdf1");
    Ptr<EmpiricalRandomVariable> rvSflow = LoadEmpiricalCdfFromFile(cdfDir + "sflowcdf");
    Ptr<EmpiricalRandomVariable> rvFlowDur = LoadEmpiricalCdfFromFile(cdfDir + "flowdurcdf");
    Ptr<EmpiricalRandomVariable> rvFrate = LoadEmpiricalCdfFromFile(cdfDir + "fratecdf");
    rvUserInter->SetStream(101);
    rvSflow->SetStream(102);
    rvFlowDur->SetStream(103);
    rvFrate->SetStream(104);

    // ---- Nodes (771 total) ----
    // n0: server-side router
    // n1: client-side router
    // n2-n5: server access routers
    // n6-n45: 40 servers
    // n46-n465: 420 clients
    // n466: bottleneck node
    // n467: BG source
    // n468: BG sink
    // n469: VoIP/RealAudio access router
    // n470-n769: 300 sender nodes
    // n770: VoIP/RealAudio sink

    NodeContainer allNodes;
    allNodes.Create(771);

    auto n = [&](uint32_t idx) -> Ptr<Node> { return allNodes.Get(idx); };

    std::cout << "Topology: 771 nodes created\n";

    // ---- Links ----
    Ipv4AddressHelper addr;
    InternetStackHelper internet;
    internet.Install(allNodes);

    PointToPointHelper p2p;

    // --- Core links ---
    // n0 -> n466: DiffServ bottleneck (3 Mbps / 20 ms)
    p2p.SetDeviceAttribute("DataRate", StringValue("3Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("20ms"));
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devBottleneck = p2p.Install(n(0), n(466));
    Ipv4InterfaceContainer ifBottleneck = AssignSubnet(addr, devBottleneck);
    // Reset queue setting for subsequent links
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100p"));

    // n466 <-> n1 (client-side)
    p2p.SetDeviceAttribute("DataRate", StringValue("3Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("20ms"));
    NetDeviceContainer dev466_1 = p2p.Install(n(466), n(1));
    AssignSubnet(addr, dev466_1);

    // Server access routers (n0 <-> n2-n5)
    uint32_t accessDelays[] = {20, 30, 40, 60};
    for (uint32_t i = 0; i < 4; i++)
    {
        p2p.SetDeviceAttribute("DataRate", StringValue("1.5Mbps"));
        std::string delay = std::to_string(accessDelays[i]) + "ms";
        p2p.SetChannelAttribute("Delay", StringValue(delay));
        NetDeviceContainer dev = p2p.Install(n(0), n(2 + i));
        AssignSubnet(addr, dev);
    }

    // Server links: n6-n45 <-> n2-n5 (10 per router, random delay 10-100ms)
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    for (uint32_t i = 0; i < 40; i++)
    {
        uint32_t base = i / 10 + 2; // n2, n3, n4, n5
        auto delayMs = static_cast<uint32_t>(rndDelay->GetValue());
        std::string delay = std::to_string(delayMs) + "ms";
        p2p.SetChannelAttribute("Delay", StringValue(delay));
        NetDeviceContainer dev = p2p.Install(n(base), n(6 + i));
        AssignSubnet(addr, dev);
    }

    // Client links: n46-n465 <-> n1 (random BW 22-32 Mbps, random delay 10-100ms)
    for (uint32_t i = 0; i < 420; i++)
    {
        auto bwMbps = static_cast<uint32_t>(rndBw->GetValue());
        std::string bw = std::to_string(bwMbps) + "Mbps";
        p2p.SetDeviceAttribute("DataRate", StringValue(bw));
        auto delayMs = static_cast<uint32_t>(rndDelay->GetValue());
        std::string delay = std::to_string(delayMs) + "ms";
        p2p.SetChannelAttribute("Delay", StringValue(delay));
        NetDeviceContainer dev = p2p.Install(n(1), n(46 + i));
        AssignSubnet(addr, dev);
    }

    // VoIP/RealAudio access: n469 <-> n0
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev469_0 = p2p.Install(n(469), n(0));
    AssignSubnet(addr, dev469_0);

    // VoIP/RealAudio sender links: n470-n769 <-> n469
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    for (uint32_t i = 0; i < 300; i++)
    {
        NetDeviceContainer dev = p2p.Install(n(469), n(470 + i));
        AssignSubnet(addr, dev);
    }

    // VoIP sink: n770 <-> n466
    p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev466_770 = p2p.Install(n(466), n(770));
    AssignSubnet(addr, dev466_770);

    // Background traffic endpoints
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer dev0_467 = p2p.Install(n(0), n(467));
    AssignSubnet(addr, dev0_467);
    NetDeviceContainer dev466_468 = p2p.Install(n(466), n(468));
    AssignSubnet(addr, dev466_468);

    std::cout << "Links: " << g_subnetIdx - 1 << " subnets assigned\n";

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ====================================================================
    // DiffServ Edge: complete service model (thesis Table 4.5)
    //   Scheduler: LLQ (PQ for Q0, SFQ 3:3:3:1 for Q1-Q4)
    //   Bottleneck: 3 Mbps -> LLQ BW = 2,700,000
    // ====================================================================

    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    // --- Physical queues ---
    edgeInner->SetNumQueues(5);
    edgeInner->SetNumPrec(0, 2); // Premium: 2 prec (in/out-profile)
    edgeInner->SetNumPrec(1, 2); // Gold: 2 prec (AF11/AF12)
    edgeInner->SetNumPrec(2, 2); // Silver: 2 prec (AF21/AF22)
    edgeInner->SetNumPrec(3, 1); // Bronze: 1 prec (AF31 only)
    edgeInner->SetNumPrec(4, 2); // BE: 2 prec (in/out-profile)

    // Queue limits
    edgeInner->SetQueueLimit(0, 20);  // Premium: small (VoIP = low latency)
    edgeInner->SetQueueLimit(1, 100); // Gold
    edgeInner->SetQueueLimit(2, 100); // Silver
    edgeInner->SetQueueLimit(3, 100); // Bronze
    edgeInner->SetQueueLimit(4, 50);  // BE

    // --- Scheduler: LLQ with SFQ ---
    // LLQ bandwidth: 2,700,000 bps (3 Mbps minus ~300 kbps Premium headroom)
    auto llq = CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                        UintegerValue(5),
                                                        "LinkBandwidth",
                                                        DoubleValue(2700000.0),
                                                        "FqVariant",
                                                        EnumValue(LlqScheduler::FqVariant::SFQ));
    // SFQ weights: Gold 30%, Silver 30%, Bronze 30%, BE 10%
    llq->SetParam(1, 3.0); // Gold
    llq->SetParam(2, 3.0); // Silver
    llq->SetParam(3, 3.0); // Bronze
    llq->SetParam(4, 1.0); // BE
    edgeInner->SetScheduler(llq);

    // --- Mark rules (port-based classification) ---
    edgeDisc->AddMarkRule({.dscp = 46, .dstPort = 5060});
    edgeDisc->AddMarkRule({.dscp = 10, .dstPort = 5004});
    edgeDisc->AddMarkRule({.dscp = 18, .dstPort = 23});
    edgeDisc->AddMarkRule({.dscp = 20, .dstPort = 21});
    edgeDisc->AddMarkRule({.dscp = 26, .dstPort = 80});
    // Default: unmatched -> DSCP 0 (Best Effort)

    // --- Policy entries ---
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 46, .cirBps = 500000.0, .cbsBytes = 10000.0});
    helper.AddDumbPolicy(edgeDisc, 51);
    helper.AddTsw2cmPolicy(edgeDisc, {.codePt = 10, .cirBps = 600000.0});
    helper.AddDumbPolicy(edgeDisc, 12);
    helper.AddDumbPolicy(edgeDisc, 18);
    helper.AddDumbPolicy(edgeDisc, 20);
    helper.AddDumbPolicy(edgeDisc, 26);
    helper.AddTokenBucketPolicy(edgeDisc, {.codePt = 0, .cirBps = 400000.0, .cbsBytes = 2000.0});
    helper.AddDumbPolicy(edgeDisc, 50);

    // --- Policer entries ---
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
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TSW2CM,
                            .initialCodePt = 10,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TSW2CM)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 12,
                            .downgrade1 = 12,
                            .downgrade2 = 12,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 18,
                            .downgrade1 = 18,
                            .downgrade2 = 18,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 20,
                            .downgrade1 = 20,
                            .downgrade2 = 20,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 26,
                            .downgrade1 = 26,
                            .downgrade2 = 26,
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

    // --- PHB table ---
    helper.AddPhbEntry(edgeInner, 46, 0, 0);
    helper.AddPhbEntry(edgeInner, 51, 0, 1);
    helper.AddPhbEntry(edgeInner, 10, 1, 0);
    helper.AddPhbEntry(edgeInner, 12, 1, 1);
    helper.AddPhbEntry(edgeInner, 18, 2, 0);
    helper.AddPhbEntry(edgeInner, 20, 2, 1);
    helper.AddPhbEntry(edgeInner, 26, 3, 0);
    helper.AddPhbEntry(edgeInner, 0, 4, 0);
    helper.AddPhbEntry(edgeInner, 50, 4, 1);

    // --- Install edge disc on n0 -> n466 ---
    Ptr<NetDevice> e1Dev = devBottleneck.Get(0);
    stratum::InstallRoot(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    // --- MRED modes (per-queue, AFTER Initialize) ---
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 0); // Premium: tail drop
    edgeInner->SetMredMode(MredMode::RIO_C, 1);     // Gold: RIO-C
    edgeInner->SetMredMode(MredMode::WRED, 2);      // Silver: WRED
    edgeInner->SetMredMode(MredMode::WRED, 3);      // Bronze: WRED
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 4); // BE: tail drop

    // --- Per-queue bandwidth for RED ---
    edgeInner->SetMeanPacketSize(500);
    edgeInner->SetQueueBandwidth(1, 810000.0); // Gold: 30% of 2.7 Mbps
    edgeInner->SetQueueBandwidth(2, 810000.0); // Silver: 30% of 2.7 Mbps
    edgeInner->SetQueueBandwidth(3, 810000.0); // Bronze: 30% of 2.7 Mbps

    // --- RED/DROP thresholds (thesis Table 4.5) ---
    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 0, .thMin = 20.0, .thMax = 20.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 0, .thMin = 60.0, .thMax = 110.0, .maxP = 0.02});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 1, .thMin = 30.0, .thMax = 60.0, .maxP = 0.6});
    helper.ConfigQueue(edgeInner,
                       {.queue = 2, .prec = 0, .thMin = 30.0, .thMax = 50.0, .maxP = 0.1});
    helper.ConfigQueue(edgeInner,
                       {.queue = 2, .prec = 1, .thMin = 30.0, .thMax = 50.0, .maxP = 0.2});
    helper.ConfigQueue(edgeInner,
                       {.queue = 3, .prec = 0, .thMin = 30.0, .thMax = 60.0, .maxP = 0.5});
    helper.ConfigQueue(edgeInner,
                       {.queue = 4, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 4, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});

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
    // Resolve destination addresses for sinks
    // ====================================================================

    Ipv4Address addr770 = n(770)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
    Ipv4Address addr468 = n(468)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();

    // ====================================================================
    // Traffic 1: Premium / VoIP (200 connections)
    //
    // Thesis: G.723.1 ON/OFF, 6.4 kbps, 48B packets, staggered 1s apart
    // Source: n470-n669 -> n770 (port 5060)
    // ====================================================================
    uint16_t voipPort = 5060;
    uint32_t numVoip = 200;

    // VoIP sink on n770
    EnsureSink(n(770), voipPort, "ns3::UdpSocketFactory");
    // Connect VoIP Rx callback for OWD/IPDV tracking on the first sink
    {
        Ptr<Application> sinkApp = n(770)->GetApplication(0);
        sinkApp->TraceConnectWithoutContext("Rx", MakeCallback(&VoipRxCallback));
    }

    if (deterministicLoad)
    {
        // Deterministic mode: 200 CBR flows, one per source node.
        // Aggregate = 480 kbps → 480000/200 = 2400 bps per node.
        // PacketSize = 48 B (G.723.1 frame); UDP; port 5060 → DSCP 46 (EF).
        constexpr uint32_t kVoipAggBps = 480000;
        const uint32_t voipPerFlowBps = kVoipAggBps / numVoip;
        for (uint32_t i = 0; i < numVoip; i++)
        {
            OnOffHelper cbrVoip("ns3::UdpSocketFactory",
                                InetSocketAddress(addr770, voipPort));
            cbrVoip.SetAttribute("DataRate", DataRateValue(DataRate(voipPerFlowBps)));
            cbrVoip.SetAttribute("PacketSize", UintegerValue(voipPktSize));
            cbrVoip.SetAttribute(
                "OnTime",
                StringValue("ns3::ConstantRandomVariable[Constant=1000000000]"));
            cbrVoip.SetAttribute("OffTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer apps = cbrVoip.Install(n(470 + i));
            apps.Start(Seconds(1.0 + i * 0.001));
            apps.Stop(Seconds(simTime));
        }
        std::cout << "VoIP: " << numVoip << " CBR flows (n470-n669 -> n770)"
                  << " - deterministic " << kVoipAggBps / 1000 << " kbps agg\n";
    }
    else
    {
        stratum::OnOffHelper voipHelper(InetSocketAddress(addr770, voipPort));
        voipHelper.SetAttribute("PacketSize", UintegerValue(voipPktSize));
        voipHelper.SetAttribute("DataRateBps", UintegerValue(6400)); // 6.4 kbps
        voipHelper.SetAttribute("OnMean", DoubleValue(1.004));       // G.723.1 talk-spurt
        voipHelper.SetAttribute("OffMean", DoubleValue(1.587));      // G.723.1 silence

        for (uint32_t i = 0; i < numVoip; i++)
        {
            ApplicationContainer apps = voipHelper.Install(n(470 + i));
            apps.Start(Seconds(static_cast<double>(i))); // stagger over 200s
            apps.Stop(Seconds(simTime));
        }

        std::cout << "VoIP: " << numVoip << " connections (n470-n669 -> n770)"
                  << " - G.723.1 6.4kbps ON/OFF\n";
    }

    // ====================================================================
    // Traffic 2: Gold / RealAudio (300 users, variable # sequential flows each)
    //
    // Port of ns-2 scenario-3.tcl §Traffic 2 — four empirical CDFs drive:
    //   user arrival time (cumulative sum of userintercdf1 draws),
    //   number of sequential flows per user (sflowcdf draw, clamped to ≥1),
    //   each flow's duration (flowdurcdf draw × 60 s),
    //   each flow's emission rate (fratecdf draw, kbps).
    //
    // ON=0.5s, OFF=exp(1.8s), 245-B packets — RealAudio streaming shape.
    //
    // Source: n470-n769 -> n770 (port 5004)
    // ====================================================================
    uint16_t streamPort = 5004;
    uint32_t numUsers = 300;

    EnsureSink(n(770), streamPort, "ns3::UdpSocketFactory");

    if (deterministicLoad)
    {
        // Deterministic mode: 300 CBR flows, one per source node n470–n769.
        // Aggregate = 900 kbps → 900000/300 = 3000 bps per node.
        // PacketSize = 245 B; UDP; port 5004 → DSCP 10 (Gold/AF11).
        constexpr uint32_t kGoldAggBps = 900000;
        const uint32_t goldPerFlowBps = kGoldAggBps / numUsers;
        for (uint32_t i = 0; i < numUsers; i++)
        {
            OnOffHelper cbrGold("ns3::UdpSocketFactory",
                                InetSocketAddress(addr770, streamPort));
            cbrGold.SetAttribute("DataRate", DataRateValue(DataRate(goldPerFlowBps)));
            cbrGold.SetAttribute("PacketSize", UintegerValue(245));
            cbrGold.SetAttribute(
                "OnTime",
                StringValue("ns3::ConstantRandomVariable[Constant=1000000000]"));
            cbrGold.SetAttribute("OffTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer app = cbrGold.Install(n(470 + i));
            app.Start(Seconds(1.0 + i * 0.001));
            app.Stop(Seconds(simTime));
        }
        std::cout << "Gold: " << numUsers << " CBR flows (n470-n769 -> n770)"
                  << " - deterministic " << kGoldAggBps / 1000 << " kbps agg\n";
    }
    else
    {
        double userStartTime = 0.0;
        uint64_t totalFlows = 0;
        for (uint32_t i = 0; i < numUsers; i++)
        {
            if (i > 0)
            {
                userStartTime += rvUserInter->GetValue();
            }

            auto numFlows = static_cast<uint32_t>(rvSflow->GetValue());
            if (numFlows < 1)
            {
                numFlows = 1;
            }

            double flowStart = userStartTime;
            double dur = rvFlowDur->GetValue() * 60.0; // CDF is in minutes

            for (uint32_t j = 0; j < numFlows; j++)
            {
                double flowStop = flowStart + dur;
                if (flowStop > simTime - 1.0)
                {
                    flowStop = simTime - 1.0;
                }
                if (flowStop <= flowStart)
                {
                    break; // No room left in the simulation window
                }

                // ns-2's Application/Traffic/RealAudio emits with session-average
                // rate ≈ rate_. ns-3's OnOff with OnTime/OffTime duty = 0.5/(0.5+1.8)
                // = 0.2174 would instead average at DataRate × 0.2174 per session.
                // Scale DataRate by 1/duty (≈4.6) so the session-average matches
                // the sampled rate from fratecdf — bursty structure preserved, mean
                // correct.
                constexpr double kOnOffDutyCompensation = 1.0 / 0.2174; // 0.5/(0.5+1.8)
                double rateKbps = rvFrate->GetValue();
                // CBR-diagnostic mode: in --realAudioCbr mode, emit a
                // deterministic CBR at the sampled session-average rate
                // (no duty-cycle compensation, no ON/OFF burstiness).
                double dataRateBps = realAudioCbr
                                         ? (rateKbps * 1000.0)
                                         : (rateKbps * 1000.0 * kOnOffDutyCompensation);
                std::ostringstream rateStr;
                rateStr << static_cast<uint32_t>(dataRateBps) << "bps";

                OnOffHelper streamOnOff("ns3::UdpSocketFactory",
                                        InetSocketAddress(addr770, streamPort));
                streamOnOff.SetAttribute("DataRate", StringValue(rateStr.str()));
                streamOnOff.SetAttribute("PacketSize", UintegerValue(245));
                if (realAudioCbr)
                {
                    // CBR: always-on, no idle interval.
                    streamOnOff.SetAttribute(
                        "OnTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
                    streamOnOff.SetAttribute(
                        "OffTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=0]"));
                }
                else
                {
                    streamOnOff.SetAttribute(
                        "OnTime",
                        StringValue("ns3::ConstantRandomVariable[Constant=0.5]"));
                    streamOnOff.SetAttribute(
                        "OffTime",
                        StringValue("ns3::ExponentialRandomVariable[Mean=1.8]"));
                }

                ApplicationContainer app = streamOnOff.Install(n(470 + i));
                app.Start(Seconds(flowStart));
                app.Stop(Seconds(flowStop));
                totalFlows++;

                // 1 ms gap, then next sequential flow under same user.
                flowStart = flowStop + 0.001;
                dur = rvFlowDur->GetValue() * 60.0;
            }
        }

        std::cout << "RealAudio: " << numUsers << " users -> " << totalFlows
                  << " sequential flows (empirical CDFs, 245B bursty UDP)\n";
    }

    // ====================================================================
    // Traffic 3: Silver / Telnet (50 connections)
    // Source: n6-n45 -> n46-n95 (port 23)
    // ====================================================================
    uint16_t telnetPort = 23;
    uint32_t numTelnet = 50;

    // ====================================================================
    // Traffic 4: Silver / FTP (50 connections)
    // Source: n6-n45 -> n46-n95 (port 21)
    // ====================================================================
    uint16_t ftpPort = 21;
    uint32_t numFtp = 50;

    if (deterministicLoad)
    {
        // Deterministic mode: Silver aggregate = 900 kbps split equally between
        // Telnet (port 23 → DSCP 18/AF21) and FTP (port 21 → DSCP 20/AF22).
        // 40 unique source nodes (n6–n45); one CBR per source per sub-class.
        // Per-source rate = 450000 / 40 = 11250 bps; PacketSize = 245 B; UDP.
        constexpr uint32_t kSilverHalfBps = 450000; // 450 kbps per sub-class
        constexpr uint32_t kSilverSrcCount = 40;
        const uint32_t silverPerFlowBps = kSilverHalfBps / kSilverSrcCount;

        // Telnet sub-class (DSCP 18/AF21): one CBR per unique server node
        Ipv4Address dstTelnet = n(46)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(n(46), telnetPort, "ns3::UdpSocketFactory");
        for (uint32_t i = 0; i < kSilverSrcCount; i++)
        {
            OnOffHelper cbrTelnet("ns3::UdpSocketFactory",
                                  InetSocketAddress(dstTelnet, telnetPort));
            cbrTelnet.SetAttribute("DataRate", DataRateValue(DataRate(silverPerFlowBps)));
            cbrTelnet.SetAttribute("PacketSize", UintegerValue(245));
            cbrTelnet.SetAttribute(
                "OnTime",
                StringValue("ns3::ConstantRandomVariable[Constant=1000000000]"));
            cbrTelnet.SetAttribute("OffTime",
                                   StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer app = cbrTelnet.Install(n(6 + i));
            app.Start(Seconds(1.0 + i * 0.001));
            app.Stop(Seconds(simTime));
        }

        // FTP sub-class (DSCP 20/AF22): one CBR per unique server node
        Ipv4Address dstFtp = n(47)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(n(47), ftpPort, "ns3::UdpSocketFactory");
        for (uint32_t i = 0; i < kSilverSrcCount; i++)
        {
            OnOffHelper cbrFtp("ns3::UdpSocketFactory",
                               InetSocketAddress(dstFtp, ftpPort));
            cbrFtp.SetAttribute("DataRate", DataRateValue(DataRate(silverPerFlowBps)));
            cbrFtp.SetAttribute("PacketSize", UintegerValue(245));
            cbrFtp.SetAttribute(
                "OnTime",
                StringValue("ns3::ConstantRandomVariable[Constant=1000000000]"));
            cbrFtp.SetAttribute("OffTime",
                                StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer app = cbrFtp.Install(n(6 + i));
            app.Start(Seconds(1.0 + i * 0.001));
            app.Stop(Seconds(simTime));
        }

        std::cout << "Silver: " << kSilverSrcCount << " CBR flows/sub-class (n6-n45)"
                  << " - deterministic " << (kSilverHalfBps * 2) / 1000 << " kbps agg\n";
    }
    else
    {
        for (uint32_t i = 0; i < numTelnet; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            double startTime = static_cast<double>(i) * 1.0;

            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            EnsureSink(n(dst), telnetPort, "ns3::TcpSocketFactory");

            OnOffHelper telnetOnOff("ns3::TcpSocketFactory",
                                    InetSocketAddress(dstAddr, telnetPort));
            telnetOnOff.SetAttribute("DataRate", StringValue("50kbps"));
            telnetOnOff.SetAttribute("PacketSize", UintegerValue(512));
            telnetOnOff.SetAttribute("OnTime",
                                     StringValue("ns3::ConstantRandomVariable[Constant=0.01]"));
            telnetOnOff.SetAttribute(
                "OffTime",
                StringValue("ns3::ExponentialRandomVariable[Mean=0.5]"));

            ApplicationContainer app = telnetOnOff.Install(n(src));
            app.Start(Seconds(startTime));
            app.Stop(Seconds(simTime));
        }

        std::cout << "Telnet: " << numTelnet << " connections (n6-n45 -> n46-n95)\n";

        for (uint32_t i = 0; i < numFtp; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            double startTime = static_cast<double>(i) * 1.0;

            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            EnsureSink(n(dst), ftpPort, "ns3::TcpSocketFactory");

            BulkSendHelper ftpBulk("ns3::TcpSocketFactory",
                                   InetSocketAddress(dstAddr, ftpPort));
            ftpBulk.SetAttribute("MaxBytes", UintegerValue(0));
            ftpBulk.SetAttribute("SendSize", UintegerValue(1460));

            ApplicationContainer app = ftpBulk.Install(n(src));
            app.Start(Seconds(startTime));
            app.Stop(Seconds(simTime));
        }

        std::cout << "FTP: " << numFtp << " connections (n6-n45 -> n46-n95)\n";
    }

    // ====================================================================
    // Traffic 5: Bronze / HTTP (50 connections)
    //
    // Thesis: 400 HTTP sessions (PagePool/WebTraf).
    // Approximated with 50 TCP OnOff connections (same as ns-2 workaround).
    // Source: n6-n45 -> n46-n95 (port 80)
    // ====================================================================
    uint16_t httpPort = 80;
    uint32_t numHttp = 50;

    if (deterministicLoad)
    {
        // Deterministic mode: Bronze aggregate = 900 kbps.
        // 40 unique source nodes (n6–n45); one CBR per source.
        // Per-source rate = 900000 / 40 = 22500 bps; PacketSize = 245 B; UDP.
        // port 80 → DSCP 26 (Bronze/AF31).
        constexpr uint32_t kBronzeAggBps = 900000;
        constexpr uint32_t kBronzeSrcCount = 40;
        const uint32_t bronzePerFlowBps = kBronzeAggBps / kBronzeSrcCount;
        Ipv4Address dstHttp = n(46)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
        EnsureSink(n(46), httpPort, "ns3::UdpSocketFactory");
        for (uint32_t i = 0; i < kBronzeSrcCount; i++)
        {
            OnOffHelper cbrHttp("ns3::UdpSocketFactory",
                                InetSocketAddress(dstHttp, httpPort));
            cbrHttp.SetAttribute("DataRate", DataRateValue(DataRate(bronzePerFlowBps)));
            cbrHttp.SetAttribute("PacketSize", UintegerValue(245));
            cbrHttp.SetAttribute(
                "OnTime",
                StringValue("ns3::ConstantRandomVariable[Constant=1000000000]"));
            cbrHttp.SetAttribute("OffTime",
                                 StringValue("ns3::ConstantRandomVariable[Constant=0]"));
            ApplicationContainer app = cbrHttp.Install(n(6 + i));
            app.Start(Seconds(1.0 + i * 0.001));
            app.Stop(Seconds(simTime));
        }
        std::cout << "Bronze: " << kBronzeSrcCount << " CBR flows (n6-n45)"
                  << " - deterministic " << kBronzeAggBps / 1000 << " kbps agg\n";
    }
    else
    {
        for (uint32_t i = 0; i < numHttp; i++)
        {
            uint32_t src = (i % 40) + 6;
            uint32_t dst = (i % 50) + 46;
            double startTime = static_cast<double>(i) * 1.0;

            Ipv4Address dstAddr = n(dst)->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            EnsureSink(n(dst), httpPort, "ns3::TcpSocketFactory");

            OnOffHelper httpOnOff("ns3::TcpSocketFactory",
                                  InetSocketAddress(dstAddr, httpPort));
            httpOnOff.SetAttribute("DataRate", StringValue("500kbps"));
            httpOnOff.SetAttribute("PacketSize", UintegerValue(1460));
            httpOnOff.SetAttribute("OnTime",
                                   StringValue("ns3::ExponentialRandomVariable[Mean=1.0]"));
            httpOnOff.SetAttribute("OffTime",
                                   StringValue("ns3::ExponentialRandomVariable[Mean=5.0]"));

            ApplicationContainer app = httpOnOff.Install(n(src));
            app.Start(Seconds(startTime));
            app.Stop(Seconds(simTime));
        }

        std::cout << "HTTP: " << numHttp << " connections (n6-n45 -> n46-n95)\n";
    }

    // ====================================================================
    // Traffic 6: Best Effort / Background CBR
    //
    // Thesis: "CBR flow from n467 to n468, 500 kbps, 512B packets"
    // Source: n467 -> n468 (port 10000)
    // ====================================================================
    uint16_t bgPort = 10000;

    EnsureSink(n(468), bgPort, "ns3::UdpSocketFactory");

    if (deterministicLoad)
    {
        // Deterministic mode: BE aggregate = 600 kbps (1 source node).
        // PacketSize = 245 B; UDP; port 10000 → unclassified (DSCP 0 / BE).
        constexpr uint32_t kBeAggBps = 600000;
        OnOffHelper cbrBg("ns3::UdpSocketFactory", InetSocketAddress(addr468, bgPort));
        cbrBg.SetAttribute("DataRate", DataRateValue(DataRate(kBeAggBps)));
        cbrBg.SetAttribute("PacketSize", UintegerValue(245));
        cbrBg.SetAttribute("OnTime",
                           StringValue("ns3::ConstantRandomVariable[Constant=1000000000]"));
        cbrBg.SetAttribute("OffTime",
                           StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer bgApp = cbrBg.Install(n(467));
        bgApp.Start(Seconds(1.0 + 0 * 0.001));
        bgApp.Stop(Seconds(simTime));
        std::cout << "BE: 1 CBR flow (n467 -> n468)"
                  << " - deterministic " << kBeAggBps / 1000 << " kbps\n";
    }
    else
    {
        OnOffHelper bgOnOff("ns3::UdpSocketFactory", InetSocketAddress(addr468, bgPort));
        bgOnOff.SetAttribute("DataRate", StringValue("500000bps"));
        bgOnOff.SetAttribute("PacketSize", UintegerValue(512));
        bgOnOff.SetAttribute("OnTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=1000000]"));
        bgOnOff.SetAttribute("OffTime",
                             StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        ApplicationContainer bgApp = bgOnOff.Install(n(467));
        bgApp.Start(Seconds(0.0));
        bgApp.Stop(Seconds(simTime));

        std::cout << "BG: 1 CBR flow (n467 -> n468) - 500 kbps, 512B\n";
    }

    // ====================================================================
    // Trace files and metric recording
    // ====================================================================
    g_serviceRateFile.open(outputDir + "/ServiceRate.tr");
    g_classRateFile.open(outputDir + "/ClassRate.tr");
    g_queueLenFile.open(outputDir + "/QueueLen.tr");
    g_owdFile.open(outputDir + "/OWD.tr");
    g_ipdvFile.open(outputDir + "/IPDV.tr");

    Simulator::Schedule(Seconds(0.0), &RecordServiceRate);
    Simulator::Schedule(Seconds(0.0), &RecordClassRate);
    Simulator::Schedule(Seconds(6.0), &RecordQueueLength);
    Simulator::Schedule(Seconds(6.0), &RecordDelayFull);

    // ====================================================================
    // Print configuration
    // ====================================================================
    std::cout << "\n--- Scenario 3 Fullscale: 771-node Complete Service Model ---\n"
              << "Bottleneck: 3 Mbps / 20 ms (n0 -> n466)\n"
              << "Scheduler: LLQ (PQ + SFQ weights 3:3:3:1, BW=2,700,000)\n"
              << "Queues: 5 (Premium/Gold/Silver/Bronze/BE)\n"
              << "Premium: EF, TB 500k/10KB, Drop\n"
              << "Gold:    AF11/AF12, TSW2CM 600k, RIO-C\n"
              << "Silver:  AF21/AF22, Dumb, WRED\n"
              << "Bronze:  AF31, Dumb, WRED\n"
              << "BE:      Default, TB 400k/2KB, Drop\n"
              << "Simulation time: " << simTime << " s\n\n";

    // ====================================================================
    // Run
    // ====================================================================
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    edgeDisc->PrintStats();

    g_serviceRateFile.close();
    g_classRateFile.close();
    g_queueLenFile.close();
    g_owdFile.close();
    g_ipdvFile.close();

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

    // ---- Shared / quick-scale arguments (unchanged defaults) ----
    double simTime = 100.0;
    uint32_t seed = 42;
    std::string outputDir = "output/ns3/example-3";

    // ---- Full-scale specific arguments ----
    double simTimeFull = 5000.0;
    std::string outputDirFull = "output/ns3/example-3-fullscale";
    // Default location when this example runs from the ns-3 root via
    // `./ns3 run "diffserv-example-3 --scale=full"`. The
    // NS3_DIFFSERV_DATA_DIR env-var (with trailing-slash
    // normalization) overrides the default; --cdfDir=<path> overrides
    // both via cmd.Parse() below.
    std::string cdfDir = []() {
        if (const char* p = std::getenv("NS3_DIFFSERV_DATA_DIR"); p && *p)
        {
            std::string s(p);
            if (s.back() != '/')
            {
                s.push_back('/');
            }
            return s;
        }
        return std::string("contrib/stratum/examples/example-3-data/");
    }();

    // CBR-substitution diagnostic for the Gold-class RealAudio
    // generator. When true, Traffic 2 emits a deterministic CBR at
    // the session-average rate sampled from the empirical CDF,
    // rather than an OnOff with ON=0.5s / OffTime=exp(1.8s). Used
    // to characterise generator-variance contribution to the
    // ns-3 vs ns-2 Gold-class throughput delta.
    bool realAudioCbr = false;
    bool deterministicLoad = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("scale", "Scenario scale: 'quick' (default) or 'full'", scale);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("outputDir", "Output directory for traces", outputDir);
    cmd.AddValue("cdfDir", "Directory containing RealAudio empirical CDFs (full scale)", cdfDir);
    cmd.AddValue("realAudioCbr",
                 "Substitute deterministic CBR at session-average rate for the "
                 "Gold-class RealAudio generator (generator-variance diagnostic)",
                 realAudioCbr);
    cmd.AddValue("deterministicLoad",
                 "Replace all Scenario-3 class generators with fixed deterministic "
                 "CBR (identical across simulators) for cross-generation convergence",
                 deterministicLoad);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(scale != "quick" && scale != "full",
                    "scale must be 'quick' or 'full' (got '" << scale << "')");

    NS_ABORT_MSG_IF(deterministicLoad && scale != "full",
                    "--deterministicLoad is only available with --scale=full");

    if (scale == "full")
    {
        // If the user did not override --simTime, use the full-scale default.
        double effSimTime = (simTime == 100.0) ? simTimeFull : simTime;
        // Same idea for --outputDir.
        std::string effOutputDir =
            (outputDir == "output/ns3/example-3") ? outputDirFull : outputDir;

        return RunFullScenario(effSimTime, seed, effOutputDir, cdfDir, realAudioCbr,
                               deterministicLoad);
    }

    return RunQuickScenario(simTime, seed, outputDir);
}
