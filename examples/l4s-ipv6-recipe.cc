/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * L4S over IPv6 — minimal dual-stack recipe.
 *
 * Topology:
 *
 *   sender ──100 Mbps/1ms── router ──10 Mbps/20ms── sink
 *                           (l4s::QueueDisc + CoupledScheduler)
 *     fd00:1::/64                fd00:2::/64
 *
 * Two UDP CBR flows from sender to sink over IPv6:
 *
 *   Port 5001 — L4S flow: ECT(1) (the L4S identifier), routed to the L4S
 *               sub-queue (idx 0) of the DualPI2 disc, coupled-marked to CE
 *               when the queue builds.
 *   Port 5002 — classic flow: Not-ECT, routed to the classic sub-queue
 *               (idx 1) of the DualPI2 disc.
 *
 * Both flows together over-subscribe the 10 Mbps bottleneck, causing the
 * DualPI2 controller to build probability and CE-mark the L4S flow.
 *
 * Expected output: the L4S flow receives CE marks (non-zero) and achieves
 * lower one-way delay than the classic flow, demonstrating that coupled
 * marking works over IPv6.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-static-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-send-time-tag.h"
#include "ns3/traffic-control-module.h"

#include <iomanip>
#include <iostream>

using namespace ns3;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::SendTimeTag;

NS_LOG_COMPONENT_DEFINE("L4sIpv6Recipe");

// ---------------------------------------------------------------------------
// CBR application for IPv6: stamps SendTimeTag and sets the IPv6 traffic
// class (DSCP + ECN) before sending.
// ---------------------------------------------------------------------------

class TaggedCbrV6App : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("L4sIpv6RecipeTaggedCbrV6App")
                                .SetParent<Application>()
                                .AddConstructor<TaggedCbrV6App>();
        return tid;
    }

    /// @param remote  IPv6 destination address + port.
    /// @param pktSize UDP payload size in bytes.
    /// @param bps     Constant bit rate in bits per second.
    /// @param tclass  IPv6 traffic class byte (DSCP<<2 | ECN).
    void Setup(Inet6SocketAddress remote, uint32_t pktSize, uint64_t bps, uint8_t tclass)
    {
        m_remote = remote;
        m_pktSize = pktSize;
        m_bps = bps;
        m_tclass = tclass;
    }

    uint64_t GetSent() const
    {
        return m_sent;
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), TypeId::LookupByName("ns3::UdpSocketFactory"));
        // SetIpv6Tclass stamps the traffic-class byte on every sent packet.
        m_socket->SetIpv6Tclass(m_tclass);
        m_socket->Bind6();
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
        double gap = static_cast<double>(m_pktSize * 8) / static_cast<double>(m_bps);
        m_event = Simulator::Schedule(Seconds(gap), &TaggedCbrV6App::SendOne, this);
    }

    Inet6SocketAddress m_remote{Ipv6Address::GetAny(), 0};
    Ptr<Socket> m_socket;
    uint32_t m_pktSize{1000};
    uint64_t m_bps{1000000};
    uint8_t m_tclass{0};
    bool m_running{false};
    EventId m_event;
    uint64_t m_sent{0};
};

NS_OBJECT_ENSURE_REGISTERED(TaggedCbrV6App);

// ---------------------------------------------------------------------------
// Per-flow Rx callback: measures one-way delay via SendTimeTag.
// ---------------------------------------------------------------------------

struct FlowStats
{
    uint64_t rxBytes{0};
    uint64_t rxPkts{0};
    double sumOwd{0.0};  // accumulated OWD in seconds
    double minOwd{1e9};  // minimum OWD in seconds
    double maxOwd{-1e9}; // maximum OWD in seconds
};

static FlowStats g_statsL4s;
static FlowStats g_statsClassic;

static void
RxCallback(FlowStats* s, Ptr<const Packet> packet, const Address& /* addr */)
{
    s->rxBytes += packet->GetSize();
    ++s->rxPkts;

    SendTimeTag tag;
    if (packet->PeekPacketTag(tag))
    {
        double owd = Simulator::Now().GetSeconds() - tag.GetSendTime();
        s->sumOwd += owd;
        if (owd < s->minOwd)
        {
            s->minOwd = owd;
        }
        if (owd > s->maxOwd)
        {
            s->maxOwd = owd;
        }
    }
}

int
main(int argc, char* argv[])
{
    double simTime = 10.0;
    uint64_t l4sBps = 6000000;         // 6 Mbps  — L4S (ECT(1)) flow
    uint64_t classicBps = 6000000;     // 6 Mbps  — classic (Not-ECT) flow
    uint64_t bottleneckBps = 10000000; // 10 Mbps bottleneck
    uint32_t pktSize = 1000;

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration in seconds", simTime);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    // ---- Nodes ----
    Ptr<Node> sender = CreateObject<Node>();
    Ptr<Node> router = CreateObject<Node>();
    Ptr<Node> sink = CreateObject<Node>();
    NodeContainer allNodes(sender, router, sink);

    // ---- Links ----
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pAccess.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100p"));
    NetDeviceContainer devSenderRouter = p2pAccess.Install(sender, router);

    // Bottleneck: shrink driver queue to 1p so the L4S disc is the only
    // queueing layer.
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bottleneckBps)));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("20ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devRouterSink = p2pBottleneck.Install(router, sink);

    // ---- Internet stack (IPv4 + IPv6) ----
    InternetStackHelper internet;
    internet.Install(allNodes);

    // ---- IPv6 addressing + routing (ULA, fd00::/8) ----
    Ipv6AddressHelper ipv6;
    ipv6.SetBase(Ipv6Address("fd00:1::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if1 = ipv6.Assign(devSenderRouter);
    if1.SetForwarding(1, true);
    if1.SetDefaultRouteInAllNodes(1);

    ipv6.SetBase(Ipv6Address("fd00:2::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if2 = ipv6.Assign(devRouterSink);
    if2.SetForwarding(0, true);
    if2.SetDefaultRouteInAllNodes(0);

    // Sink global unicast address on the fd00:2::/64 link.
    Ipv6Address sinkAddr = if2.GetAddress(1, 1);

    // ---- L4S QueueDisc on router egress toward sink ----
    // Remove the default FqCodel disc that PointToPointHelper auto-installs
    // before attaching the L4S disc (otherwise the default disc masks it).
    TrafficControlHelper tchClear;
    tchClear.Uninstall(devRouterSink.Get(0));

    Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
    disc->SetNumQueues(2);
    disc->SetL4sQueueIdx(0); // sub-queue 0 = L4S lane
    disc->SetQueueLimit(0, 300);
    disc->SetQueueLimit(1, 300);

    // PHB table routes the Not-ECT (DSCP-classified) traffic; ECT(1) is
    // classified by its ECN codepoint into the L4S lane and never consults it.
    disc->AddPhbEntry(0, 1, 0); // (codePt=0, queue=1 classic lane, prec=0)

    Ptr<l4s::CoupledScheduler> sched =
        CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                          UintegerValue(2),
                                                          "L4sQueueIdx",
                                                          UintegerValue(0),
                                                          "BurstCap",
                                                          UintegerValue(8));
    disc->SetScheduler(sched);

    Ptr<NetDevice> routerEgressDev = devRouterSink.Get(0);
    stratum::InstallRoot(routerEgressDev, disc);
    disc->Initialize();

    // AQM thresholds: L4S lane — wide so only the PI² step drives marking;
    // classic lane — standard WRED.
    disc->ConfigQueue({.queue = 0, .prec = 0, .thMin = 100.0, .thMax = 300.0, .maxP = 0.1});
    disc->ConfigQueue({.queue = 1, .prec = 0, .thMin = 30.0, .thMax = 100.0, .maxP = 0.1});

    // ---- Traffic ----
    // IPv6 traffic-class byte: upper 6 bits = DSCP (<<2), lower 2 bits = ECN.
    // ECT(1) = 0x01 (the L4S identifier, per RFC 9331 §2).
    constexpr uint16_t kL4sPort = 5001;
    constexpr uint16_t kClassicPort = 5002;
    constexpr uint8_t kEct1 = static_cast<uint8_t>(Ipv6Header::ECN_ECT1); // 0x01

    // L4S flow: ECT(1) tclass, port 5001.
    PacketSinkHelper sinkL4sHelper("ns3::UdpSocketFactory",
                                   Inet6SocketAddress(Ipv6Address::GetAny(), kL4sPort));
    ApplicationContainer sinkL4sApp = sinkL4sHelper.Install(sink);
    sinkL4sApp.Start(Seconds(0.0));
    sinkL4sApp.Stop(Seconds(simTime));
    sinkL4sApp.Get(0)->TraceConnectWithoutContext("Rx",
                                                  MakeBoundCallback(&RxCallback, &g_statsL4s));

    Ptr<TaggedCbrV6App> srcL4s = CreateObject<TaggedCbrV6App>();
    srcL4s->Setup(Inet6SocketAddress(sinkAddr, kL4sPort), pktSize, l4sBps, kEct1);
    sender->AddApplication(srcL4s);
    srcL4s->SetStartTime(Seconds(0.1));
    srcL4s->SetStopTime(Seconds(simTime));

    // Classic flow: Not-ECT tclass (0), port 5002.
    PacketSinkHelper sinkClassicHelper("ns3::UdpSocketFactory",
                                       Inet6SocketAddress(Ipv6Address::GetAny(), kClassicPort));
    ApplicationContainer sinkClassicApp = sinkClassicHelper.Install(sink);
    sinkClassicApp.Start(Seconds(0.0));
    sinkClassicApp.Stop(Seconds(simTime));
    sinkClassicApp.Get(0)->TraceConnectWithoutContext(
        "Rx",
        MakeBoundCallback(&RxCallback, &g_statsClassic));

    Ptr<TaggedCbrV6App> srcClassic = CreateObject<TaggedCbrV6App>();
    srcClassic->Setup(Inet6SocketAddress(sinkAddr, kClassicPort), pktSize, classicBps, 0);
    sender->AddApplication(srcClassic);
    srcClassic->SetStartTime(Seconds(0.1));
    srcClassic->SetStopTime(Seconds(simTime));

    // ---- Run ----
    std::cout << "L4S over IPv6 — DualPI2 coupled marking\n";
    std::cout << "Bottleneck: " << bottleneckBps / 1e6 << " Mbps / 20 ms  |  "
              << "Each flow: " << l4sBps / 1e6 << " Mbps  |  "
              << "Sim: " << simTime << " s\n";
    std::cout << "Sink: " << sinkAddr << "\n\n";

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    // ---- Summary ----
    QueueDisc::Stats stats = disc->GetStats();

    auto avgOwd = [](const FlowStats& s) -> double {
        return (s.rxPkts > 0) ? (s.sumOwd / static_cast<double>(s.rxPkts) * 1000.0) : 0.0;
    };

    std::cout << "=== Results (L4S over IPv6) ===\n";
    std::cout << std::left << std::setw(22) << "Flow" << std::setw(12) << "Rx pkts" << std::setw(14)
              << "Rx bytes" << std::setw(14) << "Avg OWD (ms)" << std::setw(14) << "Min OWD (ms)"
              << "Max OWD (ms)\n";
    std::cout << std::string(80, '-') << "\n";

    std::cout << std::left << std::setw(22) << "L4S ECT(1) port 5001" << std::setw(12)
              << g_statsL4s.rxPkts << std::setw(14) << g_statsL4s.rxBytes << std::setw(14)
              << std::fixed << std::setprecision(2) << avgOwd(g_statsL4s) << std::setw(14)
              << (g_statsL4s.rxPkts > 0 ? g_statsL4s.minOwd * 1000.0 : 0.0)
              << (g_statsL4s.rxPkts > 0 ? g_statsL4s.maxOwd * 1000.0 : 0.0) << "\n";

    std::cout << std::left << std::setw(22) << "Classic Not-ECT  5002" << std::setw(12)
              << g_statsClassic.rxPkts << std::setw(14) << g_statsClassic.rxBytes << std::setw(14)
              << avgOwd(g_statsClassic) << std::setw(14)
              << (g_statsClassic.rxPkts > 0 ? g_statsClassic.minOwd * 1000.0 : 0.0)
              << (g_statsClassic.rxPkts > 0 ? g_statsClassic.maxOwd * 1000.0 : 0.0) << "\n";

    std::cout << "\n";
    std::cout << "Disc nTotalDroppedPacketsBeforeEnqueue: "
              << stats.nTotalDroppedPacketsBeforeEnqueue << "\n";
    std::cout << "Disc nTotalDroppedPackets:              " << stats.nTotalDroppedPackets << "\n";
    std::cout << "Disc nTotalMarkedPackets:               " << stats.nTotalMarkedPackets << "\n";
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    std::cout << "\nL4S flow sent: " << srcL4s->GetSent()
              << " pkts  classic flow sent: " << srcClassic->GetSent() << " pkts\n";

    Simulator::Destroy();
    return 0;
}
