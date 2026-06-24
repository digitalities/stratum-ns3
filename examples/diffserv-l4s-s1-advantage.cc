/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * L4S latency advantage over non-L4S baselines.
 *
 * A UDP CBR probe (ECT(1), ~500 kbps) competes with a TcpCubic bulk flow on
 * a 10 Mbps bottleneck. Three qdisc modes:
 *   --mode=l4s     : l4s::QueueDisc (DualPI2). ECT(1) probe routes to the
 *                    L4S sub-queue; TCP bulk routes to the classic sub-queue.
 *   --mode=fqcodel : stock FqCoDelQueueDisc. Probe and bulk flow-separated
 *                    by 5-tuple hash; no L4S coupling. Isolates the
 *                    L4S-specific contribution from the generic-AQM
 *                    contribution.
 *   --mode=fifo    : stock FifoQueueDisc / drop-tail. No AQM, no priority.
 *                    Baseline floor showing any-AQM benefit.
 *
 * The probe uses UDP (not TCP) so that packet boundaries are preserved end-
 * to-end and SendTimeTag survives to the receiver's Rx callback.
 * This replicates the TaggedCbrApp pattern from diffserv-l4s-s1-latency.cc
 * verbatim. The bulk flow uses TcpCubic to load the bottleneck realistically.
 *
 * Per-mode subdirectory layout: outDir/<mode>/probe-owd.csv, so the
 * plot recipe can split via series_col: __file__.
 *
 * Topology (dumbbell):
 *
 *   probe-sender  ─── 100 Mbps/1 ms ─── edge ─── 10 Mbps/5 ms ─── core ─── 100 Mbps/1 ms ───
 * probe-rx bulk-sender   ─── 100 Mbps/1 ms ─┘                                  └─── 100 Mbps/1 ms
 * ─── bulk-rx
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
#include "ns3/tcp-cubic.h"
#include "ns3/tcp-dctcp.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("DiffServL4sS1Advantage");

namespace
{

// ---------------------------------------------------------------------------
// Global metric state.
// ---------------------------------------------------------------------------

std::ofstream g_owdFile;
Time g_simStop;
double g_warmup = 0.0;

// ---------------------------------------------------------------------------
// UDP CBR probe application that stamps SendTimeTag on every packet
// before Send(). Replicates the TaggedCbrApp pattern from
// diffserv-l4s-s1-latency.cc verbatim.  UDP is required so that packet
// boundaries are preserved end-to-end and the tag survives to the receiver's
// Rx callback (TCP segmentation strips per-packet tags before delivery).
// The TOS byte is set to ECT(1) so the L4S classifier in l4s::QueueDisc
// routes the probe to the L4S sub-queue in l4s mode; FqCoDel and FIFO treat
// it as any other best-effort flow.
// ---------------------------------------------------------------------------

class TaggedCbrProbeApp : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("DiffServL4sS1TaggedCbrProbeApp")
                                .SetParent<Application>()
                                .AddConstructor<TaggedCbrProbeApp>();
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
        m_event = Simulator::Schedule(Seconds(gap), &TaggedCbrProbeApp::SendOne, this);
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
// Rx callback: compute OWD from SendTimeTag and stream to CSV.
// Pre-warmup samples are discarded. Mirrors RxCallbackEf from
// diffserv-l4s-s1-latency.cc.
// ---------------------------------------------------------------------------
void
TraceProbeRx(Ptr<const Packet> pkt, const Address& /*from*/)
{
    if (Simulator::Now() < Seconds(g_warmup))
    {
        return;
    }
    SendTimeTag tag;
    if (!pkt->PeekPacketTag(tag))
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    double owd = now - tag.GetSendTime();
    g_owdFile << now << ',' << owd * 1000.0 << '\n';
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string outDir = "output/ns3/diffserv-l4s-s1-advantage";
    std::string mode = "l4s";
    double simTime = 60.0;
    double warmup = 10.0;
    uint32_t bottleneckMbps = 10;
    uint32_t probeKbps = 500;
    uint32_t probePktSize = 1000;
    uint32_t bulkSenders = 1;

    CommandLine cmd(__FILE__);
    cmd.AddValue("outDir", "Output directory base", outDir);
    cmd.AddValue("mode", "Bottleneck qdisc mode (l4s|fqcodel|fifo)", mode);
    cmd.AddValue("simTime", "Sim duration (s)", simTime);
    cmd.AddValue("warmup", "Warmup window (s) dropped from analysis", warmup);
    cmd.AddValue("bottleneckMbps", "Bottleneck rate (Mbps)", bottleneckMbps);
    cmd.AddValue("probeKbps", "Probe rate (kbps)", probeKbps);
    cmd.AddValue("probePktSize", "Probe packet size (bytes)", probePktSize);
    cmd.AddValue("bulkSenders", "Number of bulk senders", bulkSenders);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_UNLESS(mode == "l4s" || mode == "fqcodel" || mode == "fifo",
                        "mode must be l4s|fqcodel|fifo");

    g_simStop = Seconds(simTime);
    g_warmup = warmup;

    std::string modeOutDir = outDir + "/" + mode;
    diffserv::EnsureDir(modeOutDir);
    g_owdFile.open(modeOutDir + "/probe-owd.csv");
    if (!g_owdFile.is_open())
    {
        NS_FATAL_ERROR("diffserv-l4s-s1-advantage: failed to open probe-owd.csv under '"
                       << modeOutDir << "'");
    }
    g_owdFile << "time_s,owd_ms\n";

    // Topology — dumbbell, same shape as diffserv-l4s-s2-coexistence.
    NodeContainer senders;
    senders.Create(2);
    NodeContainer edgeNode;
    edgeNode.Create(1);
    NodeContainer coreNode;
    coreNode.Create(1);
    NodeContainer receivers;
    receivers.Create(2);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1ms"));

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate",
                                      DataRateValue(DataRate(bottleneckMbps * 1000000ULL)));
    bottleneckLink.SetChannelAttribute("Delay", StringValue("5ms"));
    // Shrink device queue to 1 packet so the TC qdisc is the sole queueing
    // layer (same as diffserv-l4s-s1-latency.cc).
    bottleneckLink.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    NetDeviceContainer s0e = accessLink.Install(senders.Get(0), edgeNode.Get(0));
    NetDeviceContainer s1e = accessLink.Install(senders.Get(1), edgeNode.Get(0));
    NetDeviceContainer ec = bottleneckLink.Install(edgeNode.Get(0), coreNode.Get(0));
    NetDeviceContainer c0r = accessLink.Install(coreNode.Get(0), receivers.Get(0));
    NetDeviceContainer c1r = accessLink.Install(coreNode.Get(0), receivers.Get(1));

    InternetStackHelper stack;
    stack.Install(senders);
    stack.Install(edgeNode);
    stack.Install(coreNode);
    stack.Install(receivers);

    Ipv4AddressHelper ip;
    ip.SetBase("10.0.0.0", "255.255.255.0");
    ip.Assign(s0e);
    ip.SetBase("10.0.1.0", "255.255.255.0");
    ip.Assign(s1e);
    ip.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer ifEc = ip.Assign(ec);
    ip.SetBase("10.0.3.0", "255.255.255.0");
    Ipv4InterfaceContainer ifC0r = ip.Assign(c0r);
    ip.SetBase("10.0.4.0", "255.255.255.0");
    Ipv4InterfaceContainer ifC1r = ip.Assign(c1r);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Qdisc selection on bottleneck egress (ec.Get(0) = edge-side device).
    // Delete any default qdisc that InternetStackHelper may have installed
    // (pattern from diffserv-l4s-s2-coexistence.cc).
    if (mode == "l4s")
    {
        Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
        disc->SetNumQueues(2);
        // idx 0 = classic sub-queue, idx 1 = L4S sub-queue (ECT(1) probe).
        disc->SetL4sQueueIdx(1);
        disc->SetQueueLimit(0, 200);
        disc->SetQueueLimit(1, 200);
        disc->AddPhbEntry(0, 0, 0); // DSCP 0 (BE) → classic queue

        Ptr<l4s::CoupledScheduler> sched =
            CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                              UintegerValue(2),
                                                              "L4sQueueIdx",
                                                              UintegerValue(1),
                                                              "BurstCap",
                                                              UintegerValue(8));
        disc->SetScheduler(sched);

        ec.Get(0)->AggregateObject(disc);
        stratum::InstallRoot(ec.Get(0), disc);
        disc->Initialize();

        disc->ConfigQueue({.queue = 1,
                           .prec = 0,
                           .thMin = 100.0,
                           .thMax = 200.0,
                           .maxP = 0.1}); // L4S sub-queue: wide thresholds
        disc->ConfigQueue({.queue = 0,
                           .prec = 0,
                           .thMin = 30.0,
                           .thMax = 80.0,
                           .maxP = 0.1}); // classic sub-queue: standard WRED
    }
    else if (mode == "fqcodel")
    {
        TrafficControlHelper tch;
        tch.SetRootQueueDisc("ns3::FqCoDelQueueDisc", "UseEcn", BooleanValue(true));
        Ptr<TrafficControlLayer> tcl = ec.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
        if (tcl && tcl->GetRootQueueDiscOnDevice(ec.Get(0)))
        {
            tcl->DeleteRootQueueDiscOnDevice(ec.Get(0));
        }
        tch.Install(ec.Get(0));
    }
    else // fifo
    {
        TrafficControlHelper tch;
        tch.SetRootQueueDisc("ns3::FifoQueueDisc");
        Ptr<TrafficControlLayer> tcl = ec.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
        if (tcl && tcl->GetRootQueueDiscOnDevice(ec.Get(0)))
        {
            tcl->DeleteRootQueueDiscOnDevice(ec.Get(0));
        }
        tch.Install(ec.Get(0));
    }

    // Probe: UDP CBR with ECT(1) TOS marking so l4s::QueueDisc routes it to
    // the L4S sub-queue. FqCoDel and FIFO treat it as best-effort.
    constexpr uint16_t kProbePort = 9001;

    PacketSinkHelper probeSinkHelper("ns3::UdpSocketFactory",
                                     InetSocketAddress(Ipv4Address::GetAny(), kProbePort));
    ApplicationContainer probeSinkApp = probeSinkHelper.Install(receivers.Get(0));
    probeSinkApp.Start(Seconds(0.0));
    probeSinkApp.Stop(Seconds(simTime));
    Ptr<PacketSink> probeSink = DynamicCast<PacketSink>(probeSinkApp.Get(0));
    probeSink->TraceConnectWithoutContext("Rx", MakeCallback(&TraceProbeRx));

    // TOS byte: ECT(1) = 0x01 (low 2 bits), DSCP 0 (BE).
    uint8_t probeTos = static_cast<uint8_t>(Ipv4Header::ECN_ECT1);

    Ptr<TaggedCbrProbeApp> probeApp = CreateObject<TaggedCbrProbeApp>();
    probeApp->Setup(InetSocketAddress(ifC0r.GetAddress(1), kProbePort),
                    probePktSize,
                    probeKbps * 1000ULL,
                    probeTos);
    senders.Get(0)->AddApplication(probeApp);
    probeApp->SetStartTime(Seconds(0.1));
    probeApp->SetStopTime(Seconds(simTime));

    // Bulk sender(s): TcpCubic with ECN enabled (ECT(0)) — classic CC.
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));
    Config::SetDefault("ns3::TcpSocketBase::UseEcn", EnumValue(TcpSocketState::On));

    for (uint32_t i = 0; i < bulkSenders; ++i)
    {
        uint16_t bulkPort = static_cast<uint16_t>(9002 + i);

        PacketSinkHelper bulkSinkHelper("ns3::TcpSocketFactory",
                                        InetSocketAddress(Ipv4Address::GetAny(), bulkPort));
        ApplicationContainer bulkSinkApp = bulkSinkHelper.Install(receivers.Get(1));
        bulkSinkApp.Start(Seconds(0.0));
        bulkSinkApp.Stop(Seconds(simTime));

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(ifC1r.GetAddress(1), bulkPort));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer bulkApp = bulk.Install(senders.Get(1));
        bulkApp.Start(Seconds(0.5));
        bulkApp.Stop(Seconds(simTime));
    }

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    // Close CSV before Destroy() — matches lesson from Task 1.1.
    g_owdFile.close();

    Simulator::Destroy();
    return 0;
}
