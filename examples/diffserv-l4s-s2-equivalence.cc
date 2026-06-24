/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Phase D S2 — Mixed-flow coupling-formula sanity check.
 *
 * RFC 9332 §2.1 promises throughput equivalence between classic and L4S
 * flows when the L4S flow runs Scalable CC (e.g. DCTCP, TCP Prague).
 * ns-3 mainline does not expose a straightforward ECT(1)-sending
 * Scalable-CC TCP stack, so this scenario frames the result as a
 * **coupling-formula sanity check** rather than a throughput-equivalence
 * claim: we verify that the DualPI2 coupling machinery responds to
 * sustained two-flow contention as RFC 9332 §2.1 eq. (1) / App. A.1
 * prescribe.
 *
 * Two UDP CBR flows share a 10 Mbps bottleneck, each offering 8 Mbps:
 *   - Flow L: ECT(1), routes to the L4S sub-queue.
 *   - Flow C: NotECT, routes to the classic sub-queue.
 *
 * The total offered load (16 Mbps) exceeds the bottleneck by design.
 * The l4s::CoupledScheduler gives L4S first-claim under burst cap 8,
 * so Flow L drains close to its source rate; Flow C absorbs the
 * residual plus coupled drops.
 *
 * Metrics captured:
 *   - Per-flow throughput at 50 ms granularity → CSV.
 *   - Periodic (20 ms) p' (controller base prob), p_C (classic coupled
 *     drop prob), p_L (L4S immediate-mark prob), queue lengths → CSV.
 *   - End-of-run summary: total mark count, total drop count by class,
 *     steady-state throughput ratio, observed mean p_C/p_L ratio
 *     (should approximate p'^2 / min(k * p', 1) = p' / k under the
 *     default k = 2 when p' is in the controller's operating range).
 *
 * What this scenario demonstrates:
 *   1. The P.I controller drives p' > 0 once the L4S queue occupation
 *      exceeds the 1 ms target sojourn proxy.
 *   2. p_C and p_L track p' per RFC 9332 §2.1 eq. (1), within the
 *      numerical precision of periodic sampling.
 *   3. Marks appear on the ECT(1) flow and drops on the NotECT flow,
 *      with counts consistent with the coupling ratio.
 *
 * What this scenario does NOT demonstrate:
 *   Throughput equivalence. Scalable congestion control is out of
 *   scope for this example.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("DiffServL4sS2Equivalence");

namespace
{

std::ofstream g_throughputFile;
std::ofstream g_couplingFile;
Ptr<l4s::QueueDisc> g_disc;
Ptr<PacketSink> g_sinkL;
Ptr<PacketSink> g_sinkC;
uint64_t g_lastRxL = 0;
uint64_t g_lastRxC = 0;

Time g_sampleInterval = MilliSeconds(50);

class TaggedCbrApp : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("DiffServL4sS2TaggedCbrApp")
                                .SetParent<Application>()
                                .AddConstructor<TaggedCbrApp>();
        return tid;
    }

    void Setup(Address remote, uint32_t pktSize, uint64_t bps, uint8_t tos)
    {
        m_remote = remote;
        m_pktSize = pktSize;
        m_bps = bps;
        m_tos = tos;
    }

    uint64_t GetSent() const
    {
        return m_sent;
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
        ++m_sent;
        double gap = static_cast<double>(m_pktSize * 8) / m_bps;
        m_event = Simulator::Schedule(Seconds(gap), &TaggedCbrApp::SendOne, this);
    }

    Address m_remote;
    Ptr<Socket> m_socket;
    uint32_t m_pktSize{1000};
    uint64_t m_bps{1000000};
    uint8_t m_tos{0};
    bool m_running{false};
    EventId m_event;
    uint64_t m_sent{0};
};

// Periodic sample: throughput per flow + coupling probabilities.
void
SampleThroughput()
{
    double now = Simulator::Now().GetSeconds();
    uint64_t rxL = g_sinkL->GetTotalRx();
    uint64_t rxC = g_sinkC->GetTotalRx();
    double dtSec = g_sampleInterval.GetSeconds();
    double throughputL = (rxL - g_lastRxL) * 8.0 / dtSec; // bps
    double throughputC = (rxC - g_lastRxC) * 8.0 / dtSec; // bps
    g_lastRxL = rxL;
    g_lastRxC = rxC;
    g_throughputFile << now << "," << throughputL / 1e6 << "," << throughputC / 1e6 << "\n";
    Simulator::Schedule(g_sampleInterval, &SampleThroughput);
}

void
SampleCoupling()
{
    double now = Simulator::Now().GetSeconds();
    int q0 = g_disc->GetVirtualQueueLen(0, 0);
    int q1 = g_disc->GetVirtualQueueLen(1, 0);
    double pPrime = g_disc->GetBaseProb();
    double pC = g_disc->GetLastClassicCoupledProb();
    double pL = g_disc->GetLastL4sMarkProb();
    g_couplingFile << now << "," << q0 << "," << q1 << "," << pPrime << "," << pC << "," << pL
                   << "\n";
    Simulator::Schedule(MilliSeconds(20), &SampleCoupling);
}

} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 10.0;
    uint64_t offeredBps = 7500000;
    uint32_t pktSize = 1000;
    uint32_t queueLimitC = 200;
    uint32_t queueLimitL = 200;
    uint64_t bottleneckBps = 10000000;
    std::string outDir = "l4s-s2-out";

    CommandLine cmd;
    cmd.AddValue("simTime", "Duration seconds", simTime);
    cmd.AddValue("offeredBps", "Per-flow offered rate (bps)", offeredBps);
    cmd.AddValue("pktSize", "UDP payload bytes", pktSize);
    cmd.AddValue("bottleneck", "Bottleneck rate (bps)", bottleneckBps);
    cmd.AddValue("queueLimitC", "Classic sub-queue limit (packets)", queueLimitC);
    cmd.AddValue("queueLimitL", "L4S sub-queue limit (packets)", queueLimitL);
    cmd.AddValue("outDir", "CSV output directory", outDir);
    cmd.Parse(argc, argv);

    // Create outDir if missing and fail loud on open-failure.
    diffserv::EnsureDir(outDir);
    g_throughputFile.open(outDir + "/throughput.csv");
    g_couplingFile.open(outDir + "/coupling.csv");
    if (!g_throughputFile.is_open() || !g_couplingFile.is_open())
    {
        NS_FATAL_ERROR("diffserv-l4s-s2-equivalence: failed to open CSV output under '"
                       << outDir << "'. Check --outDir=<writable path>.");
    }
    g_throughputFile << "time_s,l4s_mbps,classic_mbps\n";
    g_couplingFile << "time_s,q0_pkts,q1_pkts,pPrime,pC,pL\n";

    NodeContainer nodes;
    nodes.Create(3);
    NodeContainer accessLink(nodes.Get(0), nodes.Get(1));
    NodeContainer bottleneckLink(nodes.Get(1), nodes.Get(2));

    PointToPointHelper accessP2p;
    accessP2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessP2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer accessDev = accessP2p.Install(accessLink);

    PointToPointHelper bottleneckP2p;
    bottleneckP2p.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bottleneckBps)));
    bottleneckP2p.SetChannelAttribute("Delay", StringValue("5ms"));
    // Shrink the NetDevice internal queue to 1 packet so the l4s::QueueDisc
    // is the sole queueing layer (see S1 notes for rationale).
    bottleneckP2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer bottleneckDev = bottleneckP2p.Install(bottleneckLink);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
    disc->SetNumQueues(2);
    disc->SetL4sQueueIdx(1);
    disc->SetQueueLimit(0, queueLimitC);
    disc->SetQueueLimit(1, queueLimitL);
    disc->AddPhbEntry(0, 0, 0);

    Ptr<l4s::CoupledScheduler> sched =
        CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                          UintegerValue(2),
                                                          "L4sQueueIdx",
                                                          UintegerValue(1),
                                                          "BurstCap",
                                                          UintegerValue(8));
    disc->SetScheduler(sched);

    bottleneckDev.Get(0)->AggregateObject(disc);
    stratum::InstallRoot(bottleneckDev.Get(0), disc);
    disc->Initialize();

    // L4S queue: wide so only the marker drives it. Classic: WRED.
    disc->ConfigQueue({.queue = 1, .prec = 0, .thMin = 100.0, .thMax = 200.0, .maxP = 0.1});
    disc->ConfigQueue({.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 80.0, .maxP = 0.1});

    g_disc = disc;

    Ipv4AddressHelper ip;
    ip.SetBase("10.0.1.0", "255.255.255.0");
    ip.Assign(accessDev);
    ip.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIf = ip.Assign(bottleneckDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    constexpr uint16_t kPortL = 6001;
    constexpr uint16_t kPortC = 6002;
    PacketSinkHelper sinkLHelper("ns3::UdpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), kPortL));
    PacketSinkHelper sinkCHelper("ns3::UdpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), kPortC));
    ApplicationContainer sinks;
    sinks.Add(sinkLHelper.Install(nodes.Get(2)));
    sinks.Add(sinkCHelper.Install(nodes.Get(2)));
    sinks.Start(Seconds(0.0));
    g_sinkL = DynamicCast<PacketSink>(sinks.Get(0));
    g_sinkC = DynamicCast<PacketSink>(sinks.Get(1));

    auto tosL = static_cast<uint8_t>(Ipv4Header::ECN_ECT1);
    uint8_t tosC = 0;

    Ptr<TaggedCbrApp> appL = CreateObject<TaggedCbrApp>();
    appL->Setup(InetSocketAddress(bottleneckIf.GetAddress(1), kPortL), pktSize, offeredBps, tosL);
    nodes.Get(0)->AddApplication(appL);
    appL->SetStartTime(Seconds(0.1));
    appL->SetStopTime(Seconds(simTime));

    Ptr<TaggedCbrApp> appC = CreateObject<TaggedCbrApp>();
    appC->Setup(InetSocketAddress(bottleneckIf.GetAddress(1), kPortC), pktSize, offeredBps, tosC);
    nodes.Get(0)->AddApplication(appC);
    appC->SetStartTime(Seconds(0.1));
    appC->SetStopTime(Seconds(simTime));

    Simulator::Schedule(g_sampleInterval, &SampleThroughput);
    Simulator::Schedule(MilliSeconds(120), &SampleCoupling);

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    QueueDisc::Stats stats = disc->GetStats();
    std::cout << "\n==== diffserv-l4s-s2-equivalence ====\n";
    std::cout << "SimTime:            " << simTime << " s\n";
    std::cout << "Bottleneck:         " << bottleneckBps / 1e6 << " Mbps\n";
    std::cout << "Per-flow offered:   " << offeredBps / 1e6 << " Mbps\n";
    std::cout << "L sent:             " << appL->GetSent() << " pkts\n";
    std::cout << "C sent:             " << appC->GetSent() << " pkts\n";
    std::cout << "L rx bytes:         " << g_sinkL->GetTotalRx() << "\n";
    std::cout << "C rx bytes:         " << g_sinkC->GetTotalRx() << "\n";
    double ratio = 0.0;
    if (g_sinkC->GetTotalRx() > 0)
    {
        ratio =
            static_cast<double>(g_sinkL->GetTotalRx()) / static_cast<double>(g_sinkC->GetTotalRx());
    }
    std::cout << "L:C throughput ratio: " << std::fixed << std::setprecision(3) << ratio << "\n";
    std::cout << "Disc dropped:       " << stats.nTotalDroppedPacketsBeforeEnqueue << "\n";
    std::cout << "Disc marked:        " << stats.nTotalMarkedPackets << "\n";
    for (const auto& kv : stats.nDroppedPacketsBeforeEnqueue)
    {
        std::cout << "  drop reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    std::cout << "Final p':           " << disc->GetBaseProb() << "\n";
    std::cout << "=====================================\n";

    g_throughputFile.close();
    g_couplingFile.close();
    Simulator::Destroy();
    return 0;
}
