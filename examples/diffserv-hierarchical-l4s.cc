/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Hierarchical composition of two heterogeneous inner queueing
 * disciplines on a single DiffServ edge, dispatched by DSCP
 * (Briscoe draft-briscoe-tsvwg-l4s-diffserv-02 Figure 1, "gap 1").
 *
 * sender ─── 100 Mbps / 1 ms ─── edge ─── 10 Mbps / 5 ms ─── receiver
 * └── EdgeQueueDisc with:
 * slot 0 = l4s::QueueDisc (EF/DSCP
 * 46) slot 1 = stratum::RedQueueDisc (BE/DSCP 0)
 *
 * Traffic:
 * - EF flow: DSCP 46 + ECT(1), 5 Mbps UDP CBR (within capacity). The
 * DSCP-to-slot map routes it to slot 0 → L4S inner; L4S sub-classifies
 * by ECN, sends it to the L-queue for low-latency scheduling.
 * - BE flow: DSCP 0 (NotECT), 5 Mbps UDP CBR. Routed to slot 1 → Red
 * inner → WRED sub-queue.
 *
 * Under the default strict-priority-by-slot dequeue, slot 0 drains
 * before slot 1, so EF traffic is protected from BE queuing.
 * Starvation of slot 1 under overload is the intended behaviour for
 * this composition.
 *
 * Outputs (in --outDir):
 * - efowd.csv — per-EF-packet OWD + IPDV (ms)
 * - beowd.csv — per-BE-packet OWD + IPDV (ms)
 * - slot-occupancy.csv — periodic (20 ms) per-slot packet counts
 * - stdout summary — sent/received bytes, drops, marks
 *
 * Caveat mirrors diffserv-l4s-s1-latency.cc: UDP CBR is non-responsive,
 * so L4S's *throughput* benefit (which requires Scalable congestion
 * control) is not visible. What this example shows is the mechanical
 * composition: DSCP-keyed routing places heterogeneous disciplines on
 * one edge and strict-priority dequeue preserves the gap-1 latency
 * contract for EF.
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-mark-rule.h"
#include "ns3/stratum-policy-classifier.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::MeterType;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerEntry;
using ns3::stratum::PolicerType;
using ns3::stratum::PolicyEntry;
using ns3::stratum::PriorityScheduler;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("DiffServHierarchicalL4s");

namespace
{

// ---------------------------------------------------------------------------
// Global state for metric recording.
// ---------------------------------------------------------------------------
std::ofstream g_efFile;
std::ofstream g_beFile;
std::ofstream g_slotFile;
Ptr<EdgeQueueDisc> g_edge;
double g_lastOwdEf = -1.0;
double g_lastOwdBe = -1.0;

// ---------------------------------------------------------------------------
// CBR app (copy-of pattern from diffserv-l4s-s1-latency.cc): stamps
// SendTimeTag before Send and sets IP TOS for the requested
// DSCP + ECN bits.
// ---------------------------------------------------------------------------
class TaggedCbrApp : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("DiffServHierarchicalL4sCbrApp")
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

// ---------------------------------------------------------------------------
// Rx callbacks: OWD + IPDV to CSV.
// ---------------------------------------------------------------------------
void
RxEf(Ptr<const Packet> p, const Address& /* addr */)
{
    SendTimeTag tag;
    if (!p->PeekPacketTag(tag))
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    double owd = now - tag.GetSendTime();
    double ipdv = (g_lastOwdEf >= 0.0) ? std::abs(owd - g_lastOwdEf) : 0.0;
    g_lastOwdEf = owd;
    g_efFile << now << "," << owd * 1000.0 << "," << ipdv * 1000.0 << "\n";
}

void
RxBe(Ptr<const Packet> p, const Address& /* addr */)
{
    SendTimeTag tag;
    if (!p->PeekPacketTag(tag))
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    double owd = now - tag.GetSendTime();
    double ipdv = (g_lastOwdBe >= 0.0) ? std::abs(owd - g_lastOwdBe) : 0.0;
    g_lastOwdBe = owd;
    g_beFile << now << "," << owd * 1000.0 << "," << ipdv * 1000.0 << "\n";
}

// ---------------------------------------------------------------------------
// Periodic slot-occupancy sampler (20 ms).
// ---------------------------------------------------------------------------
void
SampleSlots()
{
    double now = Simulator::Now().GetSeconds();
    uint32_t s0 = 0;
    uint32_t s1 = 0;
    if (g_edge)
    {
        Ptr<QueueDisc> q0 = g_edge->GetInnerDiscAt(0);
        Ptr<QueueDisc> q1 = g_edge->GetInnerDiscAt(1);
        if (q0)
        {
            s0 = q0->GetNPackets();
        }
        if (q1)
        {
            s1 = q1->GetNPackets();
        }
    }
    g_slotFile << now << "," << s0 << "," << s1 << "\n";
    Simulator::Schedule(MilliSeconds(20), &SampleSlots);
}

// ---------------------------------------------------------------------------
// Edge composition: L4S at slot 0, Red at slot 1.
// ---------------------------------------------------------------------------
Ptr<EdgeQueueDisc>
BuildEdge()
{
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

    // Slot 0: L4S inner. EF packets (DSCP 46 + ECT(1)) go here; the L4S
    // composer sub-classifies by ECN into the L-queue for low-latency
    // scheduling (RFC 9331 §5.1). NotECT packets would go to the classic
    // sub-queue — but in this scenario no NotECT traffic is routed to
    // slot 0.
    Ptr<l4s::QueueDisc> l4s = CreateObject<l4s::QueueDisc>();
    l4s->SetNumQueues(2);
    l4s->SetL4sQueueIdx(0);
    l4s->SetQueueLimit(0, 200);
    l4s->SetQueueLimit(1, 200);
    l4s->AddPhbEntry(46, 0, 0); // EF → L-queue (fast lane under L4S)
    l4s->AddPhbEntry(0, 1, 0);  // NotECT fallback → classic sub-queue
    Ptr<l4s::CoupledScheduler> l4sSched =
        CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                          UintegerValue(2),
                                                          "L4sQueueIdx",
                                                          UintegerValue(0),
                                                          "BurstCap",
                                                          UintegerValue(8));
    l4s->SetScheduler(l4sSched);
    edge->SetInnerDiscAt(0, l4s);

    // Slot 1: classic Red inner for BE. Single sub-queue, DROP_TAIL semantics
    // with conservative thresholds so the demo is deterministic.
    Ptr<stratum::RedQueueDisc> red = CreateObject<stratum::RedQueueDisc>();
    red->SetNumQueues(1);
    red->SetQueueLimit(0, 200);
    red->AddPhbEntry(0, 0, 0); // BE → queue 0
    Ptr<PriorityScheduler> redSched =
        CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(1));
    red->SetScheduler(redSched);
    edge->SetInnerDiscAt(1, red);

    // DSCP routing: EF → slot 0 (L4S), BE → slot 1 (Red). All other DSCPs
    // stay on the default slot 0 via m_dscpToSlot's default fill.
    edge->SetDscpToSlot(46, 0);
    edge->SetDscpToSlot(0, 1);

    // Minimal classification/policy: mark nothing (input DSCP passes
    // through). DumbMeter ensures no DSCP alteration.
    PolicyEntry pEf;
    pEf.codePoint = 46;
    pEf.meter = MeterType::DUMB;
    pEf.policer = PolicerType::DUMB;
    pEf.policyIndex = 0;
    edge->GetPolicyClassifier()->AddPolicyEntry(pEf);
    PolicerEntry policerEf;
    policerEf.policer = PolicerType::DUMB;
    policerEf.policyIndex = 0;
    policerEf.initialCodePt = 46;
    policerEf.downgrade1 = 46;
    policerEf.downgrade2 = 46;
    edge->GetPolicyClassifier()->AddPolicerEntry(policerEf);

    PolicyEntry pBe;
    pBe.codePoint = 0;
    pBe.meter = MeterType::DUMB;
    pBe.policer = PolicerType::DUMB;
    pBe.policyIndex = 1;
    edge->GetPolicyClassifier()->AddPolicyEntry(pBe);
    PolicerEntry policerBe;
    policerBe.policer = PolicerType::DUMB;
    policerBe.policyIndex = 1;
    policerBe.initialCodePt = 0;
    policerBe.downgrade1 = 0;
    policerBe.downgrade2 = 0;
    edge->GetPolicyClassifier()->AddPolicerEntry(policerBe);

    return edge;
}

} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 10.0;
    uint64_t efBps = 5000000; // 5 Mbps EF
    uint64_t beBps = 5000000; // 5 Mbps BE
    uint32_t pktSize = 1000;
    uint64_t bottleneckBps = 10000000; // 10 Mbps
    std::string outDir = "hierarchical-l4s-out";

    CommandLine cmd;
    cmd.AddValue("simTime", "Duration (s)", simTime);
    cmd.AddValue("efBps", "EF flow rate (bps)", efBps);
    cmd.AddValue("beBps", "BE flow rate (bps)", beBps);
    cmd.AddValue("pktSize", "UDP payload size (bytes)", pktSize);
    cmd.AddValue("bottleneck", "Bottleneck rate (bps)", bottleneckBps);
    cmd.AddValue("outDir", "Output directory (CSV files land here)", outDir);
    cmd.Parse(argc, argv);

    g_efFile.open(outDir + "/efowd.csv");
    g_efFile << "time_s,owd_ms,ipdv_ms\n";
    g_beFile.open(outDir + "/beowd.csv");
    g_beFile << "time_s,owd_ms,ipdv_ms\n";
    g_slotFile.open(outDir + "/slot-occupancy.csv");
    g_slotFile << "time_s,slot0_pkts,slot1_pkts\n";

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
    // Shrink NetDevice queue so the TC layer (our edge disc) owns all
    // queueing — same pattern as diffserv-l4s-s1-latency.cc.
    bottleneckP2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer bottleneckDev = bottleneckP2p.Install(bottleneckLink);

    InternetStackHelper stack;
    stack.Install(nodes);

    // Install edge on the bottleneck egress.
    Ptr<EdgeQueueDisc> edge = BuildEdge();
    bottleneckDev.Get(0)->AggregateObject(edge);
    Ptr<TrafficControlLayer> tcl =
        bottleneckDev.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
    if (tcl)
    {
        tcl->SetRootQueueDiscOnDevice(bottleneckDev.Get(0), edge);
    }
    edge->Initialize();
    g_edge = edge;

    // Post-Initialize sub-queue configuration. Both inners default to
    // RIO_C WRED with thMin=thMax=0, which fires on every packet after
    // the first (a familiar trap — see test notes). Use
    // DROP_TAIL on the BE side and wide WRED on the L4S classic fallback
    // so only L4S's own mark step drives the L-queue behaviour.
    Ptr<l4s::QueueDisc> l4s = DynamicCast<l4s::QueueDisc>(edge->GetInnerDiscAt(0));
    if (l4s)
    {
        l4s->ConfigQueue(0, 0, 100.0, 200.0, 0.1); // L-queue
        l4s->ConfigQueue(1, 0, 30.0, 80.0, 0.1);   // classic sub-queue
    }
    Ptr<stratum::RedQueueDisc> red = DynamicCast<stratum::RedQueueDisc>(edge->GetInnerDiscAt(1));
    if (red)
    {
        red->SetMredMode(MredMode::DROP_TAIL, 0);
        red->ConfigQueue(0, 0, 1000.0, 2000.0, 0.1);
    }

    Ipv4AddressHelper ip;
    ip.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer accessIf = ip.Assign(accessDev);
    ip.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIf = ip.Assign(bottleneckDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Sinks.
    constexpr uint16_t kEfPort = 5001;
    constexpr uint16_t kBePort = 5002;
    PacketSinkHelper sinkEfHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), kEfPort));
    PacketSinkHelper sinkBeHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), kBePort));
    ApplicationContainer sinks;
    sinks.Add(sinkEfHelper.Install(nodes.Get(2)));
    sinks.Add(sinkBeHelper.Install(nodes.Get(2)));
    sinks.Start(Seconds(0.0));

    Ptr<PacketSink> sinkEf = DynamicCast<PacketSink>(sinks.Get(0));
    Ptr<PacketSink> sinkBe = DynamicCast<PacketSink>(sinks.Get(1));
    sinkEf->TraceConnectWithoutContext("Rx", MakeCallback(&RxEf));
    sinkBe->TraceConnectWithoutContext("Rx", MakeCallback(&RxBe));

    // TOS bytes: DSCP in upper 6 bits, ECN in lower 2.
    auto efTos = static_cast<uint8_t>((46 << 2) | static_cast<uint8_t>(Ipv4Header::ECN_ECT1));
    uint8_t beTos = 0; // DSCP 0, NotECT

    Ptr<TaggedCbrApp> appEf = CreateObject<TaggedCbrApp>();
    appEf->Setup(InetSocketAddress(bottleneckIf.GetAddress(1), kEfPort), pktSize, efBps, efTos);
    nodes.Get(0)->AddApplication(appEf);
    appEf->SetStartTime(Seconds(0.1));
    appEf->SetStopTime(Seconds(simTime));

    Ptr<TaggedCbrApp> appBe = CreateObject<TaggedCbrApp>();
    appBe->Setup(InetSocketAddress(bottleneckIf.GetAddress(1), kBePort), pktSize, beBps, beTos);
    nodes.Get(0)->AddApplication(appBe);
    appBe->SetStartTime(Seconds(0.1));
    appBe->SetStopTime(Seconds(simTime));

    Simulator::Schedule(MilliSeconds(120), &SampleSlots);
    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    // Summary.
    QueueDisc::Stats stats = edge->GetStats();
    std::cout << "\n==== diffserv-hierarchical-l4s ====\n";
    std::cout << "SimTime:          " << simTime << " s\n";
    std::cout << "Bottleneck:       " << bottleneckBps / 1e6 << " Mbps\n";
    std::cout << "EF sent:          " << appEf->GetSent() << " pkts\n";
    std::cout << "BE sent:          " << appBe->GetSent() << " pkts\n";
    std::cout << "EF rx bytes:      " << sinkEf->GetTotalRx() << "\n";
    std::cout << "BE rx bytes:      " << sinkBe->GetTotalRx() << "\n";
    std::cout << "Edge dropped:     " << stats.nTotalDroppedPacketsBeforeEnqueue << "\n";
    std::cout << "Edge marked:      " << stats.nTotalMarkedPackets << "\n";
    for (const auto& kv : stats.nDroppedPacketsBeforeEnqueue)
    {
        std::cout << "  drop reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    std::cout << "====================================\n";

    g_efFile.close();
    g_beFile.close();
    g_slotFile.close();

    Simulator::Destroy();
    return 0;
}
