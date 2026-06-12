/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Phase D S1 — EF (ECT(1)) vs classic under priority, latency probe.
 *
 * Topology:
 *
 *   sender ─── 100 Mbps / 1 ms ─── router ─── 10 Mbps / 5 ms ─── receiver
 *                                   └── bottleneck egress carries the
 *                                       queue disc under test.
 *
 * Two modes selected via --mode:
 *
 *   l4s-on:
 *     l4s::QueueDisc with L4sQueueIdx=1 and l4s::CoupledScheduler.
 *     EF flow marks ECT(1) in the IP TOS byte so it routes to the L4S
 *     sub-queue (idx 1, served first under the coupled scheduler).
 *     AF flow is NotECT and lands on the classic sub-queue (idx 0).
 *
 *   l4s-off:
 *     Plain stratum::RedQueueDisc with PriorityScheduler (queue 1 served
 *     first strictly by priority). EF flow is NotECT but carries DSCP
 *     EF (46), which the PHB maps to queue 1. AF flow is NotECT + DSCP
 *     0 (BE), maps to queue 0.
 *
 * Both modes give EF priority access to the bottleneck; l4s-on does it
 * via ECN classification, l4s-off via DSCP classification. This
 * comparison isolates the L4S mechanism from the DSCP mechanism for the
 * same traffic mix.
 *
 * Flows (one of each, UDP CBR):
 *   - EF flow: 500 kbps (well below bottleneck, serves as the
 *     latency-sensitive probe).
 *   - AF flow: 9.5 Mbps (fills the remainder of the bottleneck to
 *     exercise the queue).
 *
 * Metrics captured per mode:
 *   - Per-packet OWD / IPDV for the EF flow (tag-based) → CSV.
 *   - Per-packet OWD / IPDV for the AF flow (tag-based) → CSV.
 *   - Periodic (20 ms) queue lengths + p' + p_C + p_L → CSV (l4s-on
 *     only; l4s-off leaves L4S columns blank).
 *   - End-of-run summary (throughput, mark + drop totals) → stdout.
 *
 * Stand-in caveat: UDP CBR is non-responsive. Because CBR ignores both
 * drops and CE marks, L4S's *throughput* benefit (which requires
 * Scalable congestion control) is not visible here. What S1 shows is
 * that ECN-based L4S classification delivers the same latency
 * envelope as DSCP-based priority for the EF probe, i.e. the two
 * classification mechanisms are interchangeable for the latency
 * objective.
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::PriorityScheduler;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("DiffServL4sS1Latency");

namespace
{

// ---------------------------------------------------------------------------
// Global state for metric recording (scoped to one invocation of main()).
// ---------------------------------------------------------------------------

std::ofstream g_owdEfFile;
std::ofstream g_owdAfFile;
std::ofstream g_queueLenFile;
Ptr<l4s::QueueDisc> g_l4sDisc; // nullptr in l4s-off mode
// Root-disc handle (either stratum::RedQueueDisc in l4s-off or l4s::QueueDisc
// in l4s-on). Typed as the base because l4s::QueueDisc and
// stratum::RedQueueDisc are independent QueueDisc subclasses; neither
// inherits from the other.
Ptr<QueueDisc> g_plainDisc;
double g_lastOwdEf = -1.0;
double g_lastOwdAf = -1.0;

// ---------------------------------------------------------------------------
// CBR application that stamps SendTimeTag BEFORE Send(), mirroring
// src/ns-3/examples/diffserv-example-1.cc. Also sets IP TOS for ECN + DSCP.
// ---------------------------------------------------------------------------

class TaggedCbrApp : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("DiffServL4sS1TaggedCbrApp")
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
// Rx callback: compute OWD + IPDV from SendTimeTag and stream to CSV.
// ---------------------------------------------------------------------------
void
RxCallbackEf(Ptr<const Packet> packet, const Address& /* addr */)
{
    SendTimeTag tag;
    if (!packet->PeekPacketTag(tag))
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    double owd = now - tag.GetSendTime();
    double ipdv = (g_lastOwdEf >= 0.0) ? std::abs(owd - g_lastOwdEf) : 0.0;
    g_lastOwdEf = owd;
    g_owdEfFile << now << "," << owd * 1000.0 << "," << ipdv * 1000.0 << "\n";
}

void
RxCallbackAf(Ptr<const Packet> packet, const Address& /* addr */)
{
    SendTimeTag tag;
    if (!packet->PeekPacketTag(tag))
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    double owd = now - tag.GetSendTime();
    double ipdv = (g_lastOwdAf >= 0.0) ? std::abs(owd - g_lastOwdAf) : 0.0;
    g_lastOwdAf = owd;
    g_owdAfFile << now << "," << owd * 1000.0 << "," << ipdv * 1000.0 << "\n";
}

// ---------------------------------------------------------------------------
// Periodic queue + controller sampler.
// ---------------------------------------------------------------------------
void
SampleQueueState()
{
    double now = Simulator::Now().GetSeconds();
    // Universal columns: queue 0 pkts, queue 1 pkts
    int q0 = 0;
    int q1 = 0;
    if (g_l4sDisc)
    {
        // q0 = L4S FIFO size, q1 = classic AQM sub-queue 1 (BE) virtual
        // length. The composer child count is fixed at 2; q-indexing here
        // is scenario-semantic, not composer-child-slot.
        Ptr<QueueDisc> l4sQd = g_l4sDisc->GetL4sQueueDisc();
        if (l4sQd)
        {
            q0 = static_cast<int>(l4sQd->GetCurrentSize().GetValue());
        }
        Ptr<stratum::RedQueueDisc> red =
            DynamicCast<stratum::RedQueueDisc>(g_l4sDisc->GetClassicAqmDisc());
        if (red)
        {
            q1 = red->GetVirtualQueueLen(1, 0);
        }
    }
    else
    {
        Ptr<stratum::RedQueueDisc> red = DynamicCast<stratum::RedQueueDisc>(g_plainDisc);
        if (red)
        {
            q0 = red->GetVirtualQueueLen(0, 0);
            q1 = red->GetVirtualQueueLen(1, 0);
        }
    }
    g_queueLenFile << now << "," << q0 << "," << q1;
    if (g_l4sDisc)
    {
        // L4S-only columns.
        g_queueLenFile << "," << g_l4sDisc->GetBaseProb() << ","
                       << g_l4sDisc->GetLastClassicCoupledProb() << ","
                       << g_l4sDisc->GetLastL4sMarkProb();
    }
    else
    {
        g_queueLenFile << ",,,";
    }
    g_queueLenFile << "\n";
    Simulator::Schedule(MilliSeconds(20), &SampleQueueState);
}

// ---------------------------------------------------------------------------
// Disc factory.
// ---------------------------------------------------------------------------
enum class Mode
{
    L4sOn,
    L4sOff,
};

Ptr<QueueDisc>
InstallDisc(NetDeviceContainer bottleneck, Mode mode)
{
    // Queue layout used by BOTH modes (priority/L4S at idx 0, classic/BE at idx
    // 1):
    //   - PriorityScheduler (l4s-off): serves queue 0 first by construction.
    //   - l4s::CoupledScheduler (l4s-on): serves the L4S queue (also idx 0) first
    //     under the burst-cap discipline (RFC 9332 §2.5.1).
    //   This keeps the EF flow on the "fast lane" in both modes, isolating the
    //   classification mechanism (ECN vs DSCP) as the only varying input.
    if (mode == Mode::L4sOn)
    {
        Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
        disc->SetNumQueues(2);
        disc->SetL4sQueueIdx(0);
        disc->SetQueueLimit(0, 200);
        disc->SetQueueLimit(1, 200);
        // PHB (used only for NotECT packets in l4s-on; ECT(1) bypasses via
        // classification): DSCP EF (46) → queue 0 (would-be fast lane), DSCP BE (0)
        // → queue 1 (classic).
        disc->AddPhbEntry(46, 0, 0);
        disc->AddPhbEntry(0, 1, 0);

        Ptr<l4s::CoupledScheduler> sched =
            CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                              UintegerValue(2),
                                                              "L4sQueueIdx",
                                                              UintegerValue(0),
                                                              "BurstCap",
                                                              UintegerValue(8));
        disc->SetScheduler(sched);

        // AggregateObject + TrafficControlLayer pattern matches Phase C.
        bottleneck.Get(0)->AggregateObject(disc);
        Ptr<TrafficControlLayer> tcl =
            bottleneck.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
        if (tcl)
        {
            tcl->SetRootQueueDiscOnDevice(bottleneck.Get(0), disc);
        }
        disc->Initialize();

        // L4S queue (idx 0): wide RED thresholds so only the L4S mark-step
        // drives the sub-queue. Classic queue (idx 1): standard WRED.
        disc->ConfigQueue(0, 0, 100.0, 200.0, 0.1);
        disc->ConfigQueue(1, 0, 30.0, 80.0, 0.1);

        g_l4sDisc = disc;
        g_plainDisc = disc;
        return disc;
    }

    // L4S-off: plain stratum::RedQueueDisc with strict priority scheduler (queue 0
    // first).
    Ptr<stratum::RedQueueDisc> disc = CreateObject<stratum::RedQueueDisc>();
    disc->SetNumQueues(2);
    disc->SetQueueLimit(0, 200);
    disc->SetQueueLimit(1, 200);
    disc->AddPhbEntry(46, 0, 0); // DSCP EF → priority queue (idx 0)
    disc->AddPhbEntry(0, 1, 0);  // DSCP BE → classic queue (idx 1)

    Ptr<PriorityScheduler> sched =
        CreateObjectWithAttributes<PriorityScheduler>("NumQueues", UintegerValue(2));
    disc->SetScheduler(sched);

    bottleneck.Get(0)->AggregateObject(disc);
    Ptr<TrafficControlLayer> tcl = bottleneck.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
    if (tcl)
    {
        tcl->SetRootQueueDiscOnDevice(bottleneck.Get(0), disc);
    }
    disc->Initialize();

    disc->ConfigQueue(0, 0, 30.0, 80.0, 0.1);
    disc->ConfigQueue(1, 0, 30.0, 80.0, 0.1);

    g_l4sDisc = nullptr;
    g_plainDisc = disc;
    return disc;
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string modeSel = "l4s-on";
    double simTime = 10.0;
    uint64_t efBps = 500000;  // 500 kbps
    uint64_t afBps = 9500000; // 9.5 Mbps
    uint32_t pktSize = 1000;
    uint64_t bottleneckBps = 10000000;
    std::string outDir = "l4s-s1-out";

    CommandLine cmd;
    cmd.AddValue("mode", "l4s-on | l4s-off", modeSel);
    cmd.AddValue("simTime", "Duration in seconds", simTime);
    cmd.AddValue("efBps", "EF flow rate (bps)", efBps);
    cmd.AddValue("afBps", "AF flow rate (bps)", afBps);
    cmd.AddValue("pktSize", "UDP payload size", pktSize);
    cmd.AddValue("bottleneck", "Bottleneck rate (bps)", bottleneckBps);
    cmd.AddValue("outDir", "Output directory (CSV files land here)", outDir);
    cmd.Parse(argc, argv);

    Mode mode = Mode::L4sOn;
    if (modeSel == "l4s-off")
    {
        mode = Mode::L4sOff;
    }
    else if (modeSel != "l4s-on")
    {
        NS_FATAL_ERROR("Unknown --mode: " << modeSel);
    }

    // Open CSV files. Create outDir if missing and fail loud on open-failure.
    diffserv::EnsureDir(outDir);
    const std::string owdEfPath = outDir + "/" + modeSel + "__owd-ef.csv";
    const std::string owdAfPath = outDir + "/" + modeSel + "__owd-af.csv";
    const std::string queueStatePath = outDir + "/" + modeSel + "__queue-state.csv";
    g_owdEfFile.open(owdEfPath);
    g_owdAfFile.open(owdAfPath);
    g_queueLenFile.open(queueStatePath);
    if (!g_owdEfFile.is_open() || !g_owdAfFile.is_open() || !g_queueLenFile.is_open())
    {
        NS_FATAL_ERROR("diffserv-l4s-s1-latency: failed to open CSV output under '"
                       << outDir << "'. Check --outDir=<writable path>.");
    }
    g_owdEfFile << "time_s,owd_ms,ipdv_ms\n";
    g_owdAfFile << "time_s,owd_ms,ipdv_ms\n";
    g_queueLenFile << "time_s,q0_pkts,q1_pkts,pPrime,pC,pL\n";

    // Topology.
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
    // Shrink the NetDevice internal queue to 1 packet so the traffic-control
    // queue disc is the sole queueing layer. Otherwise the 100-packet default
    // device queue below TC admits AF packets FIFO and defeats priority
    // scheduling at the TC layer.
    bottleneckP2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer bottleneckDev = bottleneckP2p.Install(bottleneckLink);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ptr<QueueDisc> disc = InstallDisc(NetDeviceContainer(bottleneckDev.Get(0)), mode);

    Ipv4AddressHelper ip;
    ip.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer accessIf = ip.Assign(accessDev);
    ip.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIf = ip.Assign(bottleneckDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Sinks.
    constexpr uint16_t kEfPort = 5001;
    constexpr uint16_t kAfPort = 5002;
    PacketSinkHelper sinkEfHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), kEfPort));
    PacketSinkHelper sinkAfHelper("ns3::UdpSocketFactory",
                                  InetSocketAddress(Ipv4Address::GetAny(), kAfPort));
    ApplicationContainer sinks;
    sinks.Add(sinkEfHelper.Install(nodes.Get(2)));
    sinks.Add(sinkAfHelper.Install(nodes.Get(2)));
    sinks.Start(Seconds(0.0));

    Ptr<PacketSink> sinkEf = DynamicCast<PacketSink>(sinks.Get(0));
    Ptr<PacketSink> sinkAf = DynamicCast<PacketSink>(sinks.Get(1));
    sinkEf->TraceConnectWithoutContext("Rx", MakeCallback(&RxCallbackEf));
    sinkAf->TraceConnectWithoutContext("Rx", MakeCallback(&RxCallbackAf));

    // IP TOS byte for each flow. Low 2 bits = ECN; upper 6 bits = DSCP.
    // DSCP is shifted left by 2 (DSCP field occupies bits 2-7 of TOS).
    auto dscpEfByte = static_cast<uint8_t>(46 << 2); // DSCP EF
    uint8_t dscpBeByte = 0;
    auto ect1Bits = static_cast<uint8_t>(Ipv4Header::ECN_ECT1); // = 1

    uint8_t efTos;
    if (mode == Mode::L4sOn)
    {
        efTos = dscpEfByte | ect1Bits;
    }
    else
    {
        efTos = dscpEfByte; // NotECT
    }
    uint8_t afTos = dscpBeByte; // NotECT, DSCP 0 in both modes

    Ptr<TaggedCbrApp> appEf = CreateObject<TaggedCbrApp>();
    appEf->Setup(InetSocketAddress(bottleneckIf.GetAddress(1), kEfPort), pktSize, efBps, efTos);
    nodes.Get(0)->AddApplication(appEf);
    appEf->SetStartTime(Seconds(0.1));
    appEf->SetStopTime(Seconds(simTime));

    Ptr<TaggedCbrApp> appAf = CreateObject<TaggedCbrApp>();
    appAf->Setup(InetSocketAddress(bottleneckIf.GetAddress(1), kAfPort), pktSize, afBps, afTos);
    nodes.Get(0)->AddApplication(appAf);
    appAf->SetStartTime(Seconds(0.1));
    appAf->SetStopTime(Seconds(simTime));

    Simulator::Schedule(MilliSeconds(120), &SampleQueueState);

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    // Summary.
    QueueDisc::Stats stats = disc->GetStats();
    std::cout << "\n==== diffserv-l4s-s1-latency ====\n";
    std::cout << "Mode:            " << modeSel << "\n";
    std::cout << "SimTime:         " << simTime << " s\n";
    std::cout << "Bottleneck:      " << bottleneckBps / 1e6 << " Mbps\n";
    std::cout << "EF sent:         " << appEf->GetSent() << " pkts\n";
    std::cout << "AF sent:         " << appAf->GetSent() << " pkts\n";
    std::cout << "EF rx bytes:     " << sinkEf->GetTotalRx() << "\n";
    std::cout << "AF rx bytes:     " << sinkAf->GetTotalRx() << "\n";
    std::cout << "Disc dropped (before enqueue): " << stats.nTotalDroppedPacketsBeforeEnqueue
              << "\n";
    std::cout << "Disc marked:     " << stats.nTotalMarkedPackets << "\n";
    for (const auto& kv : stats.nDroppedPacketsBeforeEnqueue)
    {
        std::cout << "  drop reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    std::cout << "=================================\n";

    g_owdEfFile.close();
    g_owdAfFile.close();
    g_queueLenFile.close();

    Simulator::Destroy();
    return 0;
}
