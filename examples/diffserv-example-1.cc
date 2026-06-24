/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * ns-3 port of DiffServ4NS example-1 (simulation-1.tcl).
 * Reproduces the topology, traffic mix, and DiffServ configuration from the
 * original 2001 scenario using PQ scheduling with an srTCM/TokenBucket policer.
 *
 * Original: ns2/diffserv4ns/examples/example-1/simulation-1.tcl
 * Topology: 5 sources -> edge e1 -> core -> edge e2 -> 5 destinations
 * Bottleneck: e1->core 2 Mbps / 5 ms (DiffServ edge)
 *
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
#include "ns3/stratum-meter.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-scfq-scheduler.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/stratum-sfq-scheduler.h"
#include "ns3/stratum-wf2qp-scheduler.h"
#include "ns3/stratum-wfq-scheduler.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::CoreQueueDisc;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::LlqScheduler;
using ns3::stratum::Meter;
using ns3::stratum::MeterType;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::PolicyEntry;
using ns3::stratum::PriorityScheduler;
using ns3::stratum::ScfqScheduler;
using ns3::stratum::SendTimeTag;
using ns3::stratum::SfqScheduler;
using ns3::stratum::Wf2qPlusScheduler;
using ns3::stratum::WfqScheduler;

NS_LOG_COMPONENT_DEFINE("DiffServExample1");

// ---------------------------------------------------------------------------
// Global state for metric recording
// ---------------------------------------------------------------------------

static Ptr<EdgeQueueDisc> g_edgeDisc; ///< Edge disc for stats queries
static std::ofstream g_serviceRateFile;
static std::ofstream g_efQueueLenFile;
static std::ofstream g_beQueueLenFile;
static std::ofstream g_owdFile;
static std::ofstream g_ipdvFile;
static std::ofstream g_ipdvSamplesFile; ///< Per-packet IPDV samples for KS test
static std::ofstream g_owdSamplesFile;  ///< Per-packet OWD samples for KS test
static std::ofstream g_scfqLogFile;
static std::ofstream g_owdEfFile;       ///< OWD-ef.tr — per-packet EF OWD, whitespace-sep
static std::ofstream g_owdBeFile;       ///< OWD-be.tr — per-packet BE OWD, whitespace-sep
static std::ofstream g_flowRateFile;    ///< FlowRate.csv     — per-window per-class throughput
static std::ofstream g_meterColourFile; ///< MeterColour.csv  — per-window per-class colour counts

/// Per-window accumulators reset every kSampleWindowSec (default 100 ms).
struct PerWindowAccum
{
    uint64_t bytesEf = 0;
    uint64_t bytesBe = 0;
    uint32_t greenEf = 0;
    uint32_t yellowEf = 0;
    uint32_t redEf = 0;
    uint32_t greenBe = 0;
    uint32_t yellowBe = 0;
    uint32_t redBe = 0;
};

static PerWindowAccum g_acc;
static constexpr double kSampleWindowSec = 0.1; // 100 ms cadence

// OWD/IPDV tracking for EF flow
static double g_sumOwd = 0.0;
static double g_sumIpdv = 0.0;
static uint64_t g_owdPktCount = 0;
static double g_previousOwd = -1.0;

// OWD tracking for BE flow
static double g_sumBeOwd = 0.0;
static uint64_t g_beOwdPktCount = 0;
static double g_previousBeOwd = -1.0;
// Transmission time on the bottleneck link (ms), subtracted from OWD to match
// ns-2. ns-2 formula: sumOWD*1000/pkts - txTime, where txTime =
// pktSize*8/linkRateKbps.
static double g_txTimeMs = 0.0;

// ---------------------------------------------------------------------------
// DiffServCbrApplication: minimal CBR source that attaches SendTimeTag
// to every packet BEFORE calling socket->Send(). This guarantees the tag is
// present when the packet enters the queue disc.
//
// OnOffApplication's "Tx" trace fires AFTER Send(), which means tags added
// in the callback arrive too late — the packet is already in the queue disc.
// This custom app avoids that timing problem.
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

        // Add SendTimeTag BEFORE sending — guaranteed to be on the
        // packet when the queue disc processes it
        SendTimeTag tag(Simulator::Now().GetSeconds());
        packet->AddPacketTag(tag);

        m_socket->Send(packet);

        // Schedule next packet: interval = pktSize * 8 / dataRate
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
// Rx callback: read SendTimeTag, compute OWD and IPDV for EF sink
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

        // Per-packet OWD sample (): time_s, owd_ms (tx-corrected).
        if (g_owdSamplesFile.is_open())
        {
            g_owdSamplesFile << Simulator::Now().GetSeconds() << " " << (owd * 1000.0 - g_txTimeMs)
                             << "\n";
        }

        // Per-packet EF OWD: time_s, owd_s (raw, not tx-corrected).
        if (g_owdEfFile.is_open())
        {
            g_owdEfFile << Simulator::Now().GetSeconds() << " " << owd << "\n";
        }

        g_acc.bytesEf += packet->GetSize();

        if (g_previousOwd >= 0.0)
        {
            double ipdv = std::abs(owd - g_previousOwd);
            g_sumIpdv += ipdv;
            // Per-packet IPDV sample (): time_s, ipdv_ms.
            if (g_ipdvSamplesFile.is_open())
            {
                g_ipdvSamplesFile << Simulator::Now().GetSeconds() << " " << (ipdv * 1000.0)
                                  << "\n";
            }
        }
        g_previousOwd = owd;
    }
}

/// Rx callback: mirror of EfRxCallback for the BE class. Writes per-packet OWD to OWD-be.tr.
static void
BeRxCallback(Ptr<const Packet> packet, const Address& /* addr */)
{
    SendTimeTag tag;
    if (packet->PeekPacketTag(tag))
    {
        double owd = Simulator::Now().GetSeconds() - tag.GetSendTime();
        g_sumBeOwd += owd;
        g_beOwdPktCount++;

        // Per-packet BE OWD: time_s, owd_s (raw).
        if (g_owdBeFile.is_open())
        {
            g_owdBeFile << Simulator::Now().GetSeconds() << " " << owd << "\n";
        }

        g_acc.bytesBe += packet->GetSize();

        g_previousBeOwd = owd;
    }
}

// ---------------------------------------------------------------------------
// Periodic metric recording callbacks
// ---------------------------------------------------------------------------

/// Consumer for the substrate-level Meter::MeterColour trace.
/// Increments per-class colour counters in g_acc; the per-window
/// emitter (RecordPerWindow) drains and writes them.
static void
OnMeterColour(ns3::stratum::Colour colour, uint32_t classId, ns3::Time /*when*/)
{
    // The incoming classId is the PolicyEntry::policyIndex from the
    // substrate trace, which equals the MeterType enum value:
    // TOKEN_BUCKET=1 (installed on EF traffic), DUMB=0 (installed on BE
    // but not traced because DUMB never varies colour). Remap to the
    // sink-side convention used by FlowRate.csv (0=EF, 1=BE) so both
    // CSVs share a consistent classId semantics.
    if (classId == 1)
    { // TOKEN_BUCKET on EF traffic
        if (colour == ns3::stratum::Colour::GREEN)
        {
            ++g_acc.greenEf;
        }
        else if (colour == ns3::stratum::Colour::YELLOW)
        {
            ++g_acc.yellowEf;
        }
        else
        {
            ++g_acc.redEf;
        }
    }
    else
    {
        if (colour == ns3::stratum::Colour::GREEN)
        {
            ++g_acc.greenBe;
        }
        else if (colour == ns3::stratum::Colour::YELLOW)
        {
            ++g_acc.yellowBe;
        }
        else
        {
            ++g_acc.redBe;
        }
    }
}

/// Drains per-window accumulators every kSampleWindowSec, writing one row
/// per (window, classId) pair to FlowRate.csv and MeterColour.csv, then
/// resets the accumulators and reschedules itself.
static void
RecordPerWindow()
{
    const double now = Simulator::Now().GetSeconds();

    // FlowRate.csv: time,classId,rate_kbps
    if (g_flowRateFile.is_open())
    {
        const double efKbps = (g_acc.bytesEf * 8.0 / 1000.0) / kSampleWindowSec;
        const double beKbps = (g_acc.bytesBe * 8.0 / 1000.0) / kSampleWindowSec;
        g_flowRateFile << now << ",0," << efKbps << "\n"; // classId 0 = EF
        g_flowRateFile << now << ",1," << beKbps << "\n"; // classId 1 = BE
    }

    // MeterColour.csv: time,classId,green,yellow,red
    if (g_meterColourFile.is_open())
    {
        g_meterColourFile << now << ",0," << g_acc.greenEf << "," << g_acc.yellowEf << ","
                          << g_acc.redEf << "\n";
        g_meterColourFile << now << ",1," << g_acc.greenBe << "," << g_acc.yellowBe << ","
                          << g_acc.redBe << "\n";
    }

    g_acc = {};
    Simulator::Schedule(Seconds(kSampleWindowSec), &RecordPerWindow);
}

/**
 * Record per-queue departure rate every 1.0 s using
 * `Scheduler::GetDepartureRate()` (TSW-style estimator).
 */
static void
RecordDepartureRate()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();

    // GetDepartureRate returns bits/s (TSW avg * 8); convert to kbps
    double efRate = sched->GetDepartureRate(0, -1) / 1000.0;
    double beRate = sched->GetDepartureRate(1, -1) / 1000.0;

    g_serviceRateFile << now << " " << efRate << " " << beRate << "\n";

    Simulator::Schedule(Seconds(1.0), &RecordDepartureRate);
}

/**
 * Record per-queue packet count every 0.5 s.
 */
static void
RecordQueueLength()
{
    double now = Simulator::Now().GetSeconds();

    // Get sub-queue packet counts via the composer's inner
    // stratum::RedQueueDisc — the edge composer's direct child is the inner,
    // not a sub-queue.
    int efLen = 0;
    int beLen = 0;
    Ptr<stratum::RedQueueDisc> inner =
        DynamicCast<stratum::RedQueueDisc>(g_edgeDisc->GetInnerDisc());
    if (inner && inner->GetNQueueDiscClasses() > 0)
    {
        efLen = static_cast<int>(inner->GetQueueDiscClass(0)->GetQueueDisc()->GetNPackets());
    }
    if (inner && inner->GetNQueueDiscClasses() > 1)
    {
        beLen = static_cast<int>(inner->GetQueueDiscClass(1)->GetQueueDisc()->GetNPackets());
    }

    g_efQueueLenFile << now << " " << efLen << "\n";
    g_beQueueLenFile << now << " " << beLen << "\n";

    Simulator::Schedule(Seconds(0.5), &RecordQueueLength);
}

/**
 * Record average OWD and IPDV for EF flow every 0.5 s.
 * Reports cumulative mean (matching ns-2 behaviour).
 */
static void
RecordDelay()
{
    double now = Simulator::Now().GetSeconds();

    if (g_owdPktCount >= 2)
    {
        double meanOwd = g_sumOwd / static_cast<double>(g_owdPktCount);
        double meanIpdv = g_sumIpdv / static_cast<double>(g_owdPktCount - 1);

        // Output in milliseconds, subtract txTime (matching ns-2 formula:
        // sumOWD*1000/pkts - txTime, see simulation-1.tcl line 231)
        g_owdFile << now << " " << (meanOwd * 1000.0 - g_txTimeMs) << "\n";
        g_ipdvFile << now << " " << meanIpdv * 1000.0 << "\n";
    }
    else
    {
        g_owdFile << now << " 0\n";
        g_ipdvFile << now << " 0\n";
    }

    Simulator::Schedule(Seconds(0.5), &RecordDelay);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(int argc, char* argv[])
{
    // ---- Command line arguments ----
    std::string scheduler = "PQ";
    uint32_t packetSize = 512;
    double simTime = 200.0;
    uint32_t seed = 42;
    std::string outputDir = "output/ns3/example-1";
    double star = 0.0;      // STAR parameter (0 = use default 3:17 weights)
    uint32_t bePktSize = 0; // Fixed BE packet size (0 = default varying 64..1280)
    uint32_t beFlows = 20;  // Number of BE flows (20 default, 23 for Test C)
    // L2 framing overhead bytes per packet. Sentinel UINT32_MAX = auto-detect
    // from the bottleneck NetDevice via diffserv::Helper::DetectL2OverheadBytes
    // (PointToPoint -> 2 PPP, Csma -> 14 Ethernet). Pass an explicit value
    // (incl. 0) on the CLI to override.
    uint32_t l2OverheadBytes = std::numeric_limits<uint32_t>::max();

    CommandLine cmd(__FILE__);
    cmd.AddValue("scheduler", "Scheduler type: PQ, WFQ, SCFQ, SFQ, WF2Qp, LLQ", scheduler);
    cmd.AddValue("packetSize", "EF packet size in bytes", packetSize);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("outputDir", "Output directory for traces", outputDir);
    cmd.AddValue("star",
                 "STAR parameter (Service-To-Arrival-Ratio). "
                 "Overrides FQ weights: Sr = STAR * EF_rate / link_rate. "
                 "0 = use default 3:17 weights (thesis §4.1 Test A)",
                 star);
    cmd.AddValue("bePktSize",
                 "Fixed BE packet size in bytes. "
                 "0 = default varying sizes (64..1280). "
                 "Set to a fixed value for Test B sweeps.",
                 bePktSize);
    cmd.AddValue("beFlows", "Number of BE flows (default 20, use 23 for Test C)", beFlows);
    cmd.AddValue("l2OverheadBytes",
                 "L2 framing overhead bytes per packet on the bottleneck link. "
                 "Default: auto-detect from the netdev via "
                 "Helper::DetectL2OverheadBytes (PointToPoint -> 2 PPP, "
                 "Csma -> 14 Ethernet, others -> 0). Pass an explicit non-default "
                 "value to override (e.g. 0 to force pure IP-byte accounting, "
                 "or a higher value to model additional framing like "
                 "LLC/SNAP/VLAN/FCS).",
                 l2OverheadBytes);
    cmd.Parse(argc, argv);

    // Compute FQ weight ratio from STAR parameter (thesis §4.1)
    // STAR = Sr * lr / Ar, where Sr = wEF/(wEF+wBE), lr = 2 Mbps, Ar = 300 kbps
    // => Sr = STAR * 300000 / 2000000 = STAR * 0.15
    double linkRateBps = 2000000.0;
    double cirEfBps = 300000.0;
    double wEF = 3.0;
    double wBE = 17.0;
    if (star > 0.0)
    {
        double sr = star * cirEfBps / linkRateBps;
        if (sr >= 1.0)
        {
            NS_FATAL_ERROR("STAR=" << star << " implies Sr=" << sr
                                   << " >= 1.0 (EF would need more than link rate)");
        }
        // Normalise to integer-friendly weights: wEF = sr * 20, wBE = (1-sr) * 20
        wEF = sr * 20.0;
        wBE = (1.0 - sr) * 20.0;
    }

    // Create output directory — include STAR in path if set
    std::string runDir;
    if (star > 0.0)
    {
        std::ostringstream oss;
        oss << outputDir << "/STAR-" << std::fixed << std::setprecision(1) << star << "-"
            << scheduler << "-" << std::string(4 - std::to_string(packetSize).length(), '0')
            << packetSize;
        runDir = oss.str();
    }
    else
    {
        runDir = outputDir + "/" + scheduler + "-" +
                 std::string(4 - std::to_string(packetSize).length(), '0') +
                 std::to_string(packetSize);
    }
    diffserv::EnsureDir(runDir);

    // ---- RNG seed ----
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    // Compute txTime for OWD correction (ns-2 subtracts this from reported OWD).
    // txTime = pktSize * 8 / bottleneckLinkRateKbps (in milliseconds).
    // bottleneck is 2 Mbps = 2000 kbps.
    g_txTimeMs = static_cast<double>(packetSize) * 8.0 / 2000.0;

    // Use the same random variable structure as ns-2:
    // rndStartTime and rndSourceNode for traffic start times and source selection
    Ptr<UniformRandomVariable> rndStartTime = CreateObject<UniformRandomVariable>();
    rndStartTime->SetAttribute("Min", DoubleValue(0.0));
    rndStartTime->SetAttribute("Max", DoubleValue(5.0));

    Ptr<UniformRandomVariable> rndSourceNode = CreateObject<UniformRandomVariable>();
    rndSourceNode->SetAttribute("Min", DoubleValue(0.0));
    rndSourceNode->SetAttribute("Max", DoubleValue(5.0));

    // ---- Create nodes ----
    NodeContainer sources;
    sources.Create(5);

    NodeContainer routers;
    routers.Create(3); // e1, core, e2

    NodeContainer destinations;
    destinations.Create(5);

    Ptr<Node> e1 = routers.Get(0);
    Ptr<Node> core = routers.Get(1);
    Ptr<Node> e2 = routers.Get(2);

    // ---- Links ----
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pAccess.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    // Source -> e1 access links
    std::vector<NetDeviceContainer> srcDevs(5);
    for (uint32_t i = 0; i < 5; i++)
    {
        srcDevs[i] = p2pAccess.Install(sources.Get(i), e1);
    }

    // e1 <-> core bottleneck (2 Mbps, 5 ms) — duplex P2P with
    // per-direction queue discs. The NetDevice queue is set to 1
    // packet so all queueing happens in the DiffServ queue disc; the
    // default of 100 packets would add ~204 ms of buffering at 2 Mbps
    // and would diverge from ns-2 behaviour (no NetDevice queue
    // between disc and link). DataRate stays canonical; L2-framing
    // compensation is at the SCHEDULER + METER layer via
    // L2OverheadBytes, not at the link.
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devE1Core = p2pBottleneck.Install(e1, core);

    // Resolve the auto-detect sentinel now that the bottleneck
    // netdev is in hand; detected once and used for both scheduler
    // and meter below.
    if (l2OverheadBytes == std::numeric_limits<uint32_t>::max())
    {
        l2OverheadBytes = diffserv::Helper::DetectL2OverheadBytes(devE1Core.Get(0));
    }
    std::cout << "L2 framing overhead bytes: " << l2OverheadBytes
              << " (charged by scheduler + meter for wire-byte accounting)\n";

    // core <-> e2 (5 Mbps, 3 ms, DropTail)
    PointToPointHelper p2pCoreE2;
    p2pCoreE2.SetDeviceAttribute("DataRate", StringValue("5Mbps"));
    p2pCoreE2.SetChannelAttribute("Delay", StringValue("3ms"));
    p2pCoreE2.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
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

    // Get destination node IDs for mark rules (matching ns-2 'node id' addresses)
    // ns-2 uses node IDs for address matching; ns-3 uses IP addresses.
    // We use the destination IP addresses on the e2-facing interfaces.
    Ipv4Address destAddr0 = dstIfs[0].GetAddress(1); // d0 — EF destination
    Ipv4Address destAddr1 = dstIfs[1].GetAddress(1); // d1 — BE destination

    // ====================================================================
    // DiffServ Edge configuration (e1 -> core)
    //
    // IMPORTANT: Configure the queue disc BEFORE installation. The ns-3
    // TrafficControlHelper calls Initialize() during Install(), which
    // triggers CheckConfig() → creates child sub-queues using m_numQueues.
    // If we configure after Install, only 1 sub-queue exists (the default)
    // and all BE traffic is dropped due to missing queue 1.
    //
    // Approach: Uninstall default QueueDisc, create and configure the edge
    // disc manually, then install via TrafficControlHelper.
    // ====================================================================

    // Step 1: Create and fully configure the edge disc BEFORE Install
    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();

    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    // 2 physical queues
    edgeInner->SetNumQueues(2);
    edgeInner->SetNumPrec(0, 2); // queue 0: 2 precedence levels (EF in/out)
    edgeInner->SetNumPrec(1, 1); // queue 1: 1 precedence level (BE)

    // Queue sizes — set BEFORE Initialize so sub-queues get correct limits
    edgeInner->SetQueueLimit(0, 30); // EF queue: 30 packets
    edgeInner->SetQueueLimit(1, 50); // BE queue: 50 packets

    // Scheduler — matches ns-2 simulation-1.tcl lines 113–139
    Ptr<stratum::Scheduler> sched;
    if (scheduler == "PQ")
    {
        sched = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                              UintegerValue(2),
                                                              "WinLen",
                                                              DoubleValue(1.0));
    }
    else if (scheduler == "WFQ")
    {
        auto wfq = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                            UintegerValue(2),
                                                            "LinkBandwidth",
                                                            DoubleValue(linkRateBps));
        wfq->SetParam(0, wEF);
        wfq->SetParam(1, wBE);
        sched = wfq;
    }
    else if (scheduler == "SCFQ")
    {
        auto scfq = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                              UintegerValue(2),
                                                              "LinkBandwidth",
                                                              DoubleValue(linkRateBps));
        scfq->SetParam(0, wEF);
        scfq->SetParam(1, wBE);
        g_scfqLogFile.open(runDir + "/ScfqDecisions.csv");
        if (g_scfqLogFile.is_open())
        {
            scfq->SetLogStream(&g_scfqLogFile);
        }
        sched = scfq;
    }
    else if (scheduler == "SFQ")
    {
        auto sfq = CreateObjectWithAttributes<SfqScheduler>("NumQueues",
                                                            UintegerValue(2),
                                                            "LinkBandwidth",
                                                            DoubleValue(linkRateBps));
        sfq->SetParam(0, wEF);
        sfq->SetParam(1, wBE);
        sched = sfq;
    }
    else if (scheduler == "WF2Qp")
    {
        auto wf2q = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                                  UintegerValue(2),
                                                                  "LinkBandwidth",
                                                                  DoubleValue(linkRateBps));
        wf2q->SetParam(0, wEF);
        wf2q->SetParam(1, wBE);
        sched = wf2q;
    }
    else if (scheduler == "LLQ")
    {
        // LLQ: PQ for queue 0, WFQ for queues 1..N-1
        auto llq =
            CreateObjectWithAttributes<LlqScheduler>("NumQueues",
                                                     UintegerValue(2),
                                                     "LinkBandwidth",
                                                     DoubleValue(linkRateBps),
                                                     "FqVariant",
                                                     EnumValue(LlqScheduler::FqVariant::WFQ));
        llq->SetParam(0, wEF);
        llq->SetParam(1, wBE);
        sched = llq;
    }
    else
    {
        NS_FATAL_ERROR("Unknown scheduler: " << scheduler
                                             << ". Use PQ, WFQ, SCFQ, SFQ, WF2Qp, or LLQ.");
    }
    // L2 framing overhead — wire-byte basis on the scheduler.
    // Propagated to LLQ inner FQ via
    // LlqScheduler::NotifyConstructionCompleted.
    sched->SetL2OverheadBytes(l2OverheadBytes);
    edgeInner->SetScheduler(sched);

    // Mark rules: classify by destination IP address.
    // Pass an Ipv4Address directly; {} means wildcard (any address).
    // EF: packets to d0 -> DSCP 46
    edgeDisc->AddMarkRule({.dscp = 46, .dstAddr = destAddr0});
    // BE: packets to d1 -> DSCP 0
    edgeDisc->AddMarkRule({.dscp = 0, .dstAddr = destAddr1});

    // Policy entries.
    // CBS = 4687 B = Cisco IOS MQC default (Bc = CIR * 125 ms = 300 kbps / 8 *
    // 0.125 = 4687.5 B). Operator-default operating point that the TF-TANT
    // reference testbed used; the +5 % EF over-rate observed under
    // work-conserving schedulers is the textbook signature of this CBS.
    double cbsEfBytes = 4687.0;
    helper.AddTokenBucketPolicy(edgeDisc,
                                {.codePt = 46, .cirBps = cirEfBps, .cbsBytes = cbsEfBytes});
    helper.AddDumbPolicy(edgeDisc, 48);
    helper.AddDumbPolicy(edgeDisc, 0);

    // Policer entries
    // TokenBucket is a two-colour meter (GREEN/RED, no YELLOW).
    // GREEN -> initialCodePt (46), RED -> downgrade2 (48).
    // downgrade1 is unused but set to 48 for safety.
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::TOKEN_BUCKET,
                            .initialCodePt = 46,
                            .downgrade1 = 48,
                            .downgrade2 = 48,
                            .policyIndex = static_cast<uint32_t>(PolicerType::TOKEN_BUCKET)});
    helper.AddPolicerEntry(edgeDisc,
                           {.policer = PolicerType::DUMB,
                            .initialCodePt = 0,
                            .downgrade1 = 0,
                            .downgrade2 = 0,
                            .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});

    // L2 framing overhead on the meters used by the edge. Must come
    // AFTER AddPolicerEntry so the meters have been lazy-instantiated
    // by GetMeter.
    if (Ptr<Meter> tbm = edgeDisc->GetMeter(MeterType::TOKEN_BUCKET))
    {
        tbm->SetL2OverheadBytes(l2OverheadBytes);
    }
    if (Ptr<Meter> dm = edgeDisc->GetMeter(MeterType::DUMB))
    {
        dm->SetL2OverheadBytes(l2OverheadBytes);
    }

    // PHB table
    helper.AddPhbEntry(edgeInner, 46, 0, 0); // EF in-profile -> queue 0, prec 0
    helper.AddPhbEntry(edgeInner, 48, 0,
                       1);                  // EF out-of-profile -> queue 0, prec 1
    helper.AddPhbEntry(edgeInner, 0, 1, 0); // BE -> queue 1, prec 0

    // Step 2: Install the pre-configured edge disc directly on the
    // TrafficControlLayer. PointToPointHelper does NOT install a default
    // queue disc, so there's nothing to Uninstall.
    Ptr<NetDevice> e1Dev = devE1Core.Get(0);
    stratum::InstallRoot(e1Dev, edgeDisc);
    edgeDisc->Initialize();

    // MRED mode + RED thresholds — AFTER Initialize (sub-queues now exist).
    // SetMredMode is a no-op when called before Initialize because sub-queues
    // do not yet exist; the helper's NS_LOG_WARN makes this silent in normal
    // runs. Calling here ensures both sub-queues get DROP_TAIL semantics
    // before ConfigQueue configures per-precedence thresholds.
    // Yellow EF (DSCP 48 → queue 0 / prec 1) uses thMin = -1, the
    // force-drop sentinel both ns-2 and ns-3 honour under DROP_TAIL
    // (thMin < 0 → PKT_EDROPPED). Matches the thesis Tcl
    // `configQ 0 1 -1` and Cisco MQC `violate-action drop`.
    edgeInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 30.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 1, .thMin = -1.0, .thMax = -1.0, .maxP = 0.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});

    // Store global reference for metric recording
    g_edgeDisc = edgeDisc;

    // ====================================================================
    // DiffServ Core configuration (core -> e1)
    // Same pre-configure-then-install pattern as the edge disc.
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

    // Install on core->e1 device
    Ptr<NetDevice> coreDev = devE1Core.Get(1);
    stratum::InstallRoot(coreDev, coreDisc);
    coreDisc->Initialize();
    coreInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(coreInner,
                       {.queue = 0, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});

    // ====================================================================
    // Traffic: EF flow (1 CBR, 300 kbps)
    // ====================================================================
    uint16_t efPort = 9;

    // EF sink on d0
    PacketSinkHelper efSinkHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), efPort));
    ApplicationContainer efSinkApp = efSinkHelper.Install(destinations.Get(0));
    efSinkApp.Start(Seconds(0.0));
    efSinkApp.Stop(Seconds(simTime));

    // Connect Rx callback for OWD/IPDV measurement
    efSinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeCallback(&EfRxCallback));

    // EF source: random source node, CBR at 300 kbps
    // Uses DiffServCbrApplication to attach SendTimeTag BEFORE Send()
    auto efSourceIdx = static_cast<uint32_t>(rndSourceNode->GetInteger(0, 4));
    double efStartTime = rndStartTime->GetValue();

    Ptr<DiffServCbrApplication> efCbr = CreateObject<DiffServCbrApplication>();
    efCbr->Setup(InetSocketAddress(destAddr0, efPort), packetSize, static_cast<uint64_t>(cirEfBps));
    sources.Get(efSourceIdx)->AddApplication(efCbr);
    efCbr->SetStartTime(Seconds(efStartTime));
    efCbr->SetStopTime(Seconds(simTime));

    std::string efRateStr = std::to_string(static_cast<int>(cirEfBps)) + "bps";
    std::cout << "EF : s(" << efSourceIdx << ")->d(0)"
              << " - Traffic: CBR - PktSize: " << packetSize << " - Rate: " << efRateStr
              << " - Start " << efStartTime << "\n";

    // ====================================================================
    // Traffic: BE flows (20 CBR, 100 kbps each)
    // ====================================================================
    uint16_t bePort = 10;

    // BE sink on d1
    PacketSinkHelper beSinkHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), bePort));
    ApplicationContainer beSinkApp = beSinkHelper.Install(destinations.Get(1));
    beSinkApp.Start(Seconds(0.0));
    beSinkApp.Stop(Seconds(simTime));

    // Connect Rx callback for per-class BE OWD measurement
    beSinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeCallback(&BeRxCallback));

    // BE sources use DiffServCbrApplication so each packet carries
    // SendTimeTag, enabling per-class OWD measurement at the sink.
    uint32_t bgPktSize = (bePktSize > 0) ? bePktSize : 64;
    for (uint32_t i = 0; i < beFlows; i++)
    {
        double startTime = rndStartTime->GetValue();
        auto srcIdx = static_cast<uint32_t>(rndSourceNode->GetInteger(0, 4));

        Ptr<DiffServCbrApplication> beCbr = CreateObject<DiffServCbrApplication>();
        beCbr->Setup(InetSocketAddress(destAddr1, bePort), bgPktSize, 100000);
        sources.Get(srcIdx)->AddApplication(beCbr);
        beCbr->SetStartTime(Seconds(startTime));
        beCbr->SetStopTime(Seconds(simTime));

        std::cout << "BE: s(" << srcIdx << ")->d(1)"
                  << " - Traffic: CBR - PktSize: " << bgPktSize << " - Rate: 100000bps"
                  << " - Start " << startTime << "\n";

        if (bePktSize == 0)
        {
            bgPktSize += 64; // varying: 64, 128, ..., (matching ns-2)
        }
    }

    // ====================================================================
    // Open trace files
    // ====================================================================
    g_serviceRateFile.open(runDir + "/ServiceRate.tr");
    g_efQueueLenFile.open(runDir + "/EFQueueLen.tr");
    g_beQueueLenFile.open(runDir + "/BEQueueLen.tr");
    g_owdFile.open(runDir + "/OWD.tr");
    g_ipdvFile.open(runDir + "/IPDV.tr");
    // Per-packet sample traces for (KS test vs ns-2).
    g_owdSamplesFile.open(runDir + "/OWD-samples.tr");
    g_ipdvSamplesFile.open(runDir + "/IPDV-samples.tr");
    g_owdEfFile.open(runDir + "/OWD-ef.tr");
    g_owdBeFile.open(runDir + "/OWD-be.tr");
    g_flowRateFile.open(runDir + "/FlowRate.csv");
    g_flowRateFile << "time,classId,rate_kbps\n"; // header row
    g_meterColourFile.open(runDir + "/MeterColour.csv");
    g_meterColourFile << "time,classId,green,yellow,red\n"; // header row

    // ====================================================================
    // Connect per-class colour trace (TokenBucket meter on EF, fires with
    // classId=1=MeterType::TOKEN_BUCKET; remapped to CSV classId=0=EF inside
    // OnMeterColour). The DUMB meter on BE does not vary colour so its trace
    // is not connected.
    // ====================================================================
    if (Ptr<Meter> tbm = edgeDisc->GetMeter(MeterType::TOKEN_BUCKET))
    {
        tbm->TraceConnectWithoutContext("MeterColour", MakeCallback(&OnMeterColour));
    }

    // ====================================================================
    // Schedule periodic metric recording
    // ====================================================================
    Simulator::Schedule(Seconds(0.0), &RecordDepartureRate);
    Simulator::Schedule(Seconds(6.0),
                        &RecordQueueLength);         // Start at t=6 like ns-2
    Simulator::Schedule(Seconds(6.0), &RecordDelay); // Start at t=6 like ns-2
    Simulator::Schedule(Seconds(kSampleWindowSec), &RecordPerWindow);

    // ====================================================================
    // Print configuration (matching ns-2 output format)
    // ====================================================================
    std::cout << "EF packet size: " << packetSize << "\n";
    if (star > 0.0)
    {
        std::cout << "STAR: " << star << " (wEF=" << wEF << " wBE=" << wBE << ")\n";
    }

    // ====================================================================
    // Run simulation
    // ====================================================================
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ====================================================================
    // Print final statistics (matching ns-2 printStats)
    // ====================================================================
    edgeDisc->PrintStats();

    // Close trace files
    g_serviceRateFile.close();
    g_efQueueLenFile.close();
    g_beQueueLenFile.close();
    g_owdFile.close();
    g_ipdvFile.close();
    g_owdEfFile.close();
    g_owdBeFile.close();
    g_flowRateFile.close();
    g_meterColourFile.close();

    Simulator::Destroy();

    return 0;
}
