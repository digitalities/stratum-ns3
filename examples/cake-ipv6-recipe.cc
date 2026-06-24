/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * CAKE diffserv4 over IPv6 — minimal dual-stack recipe.
 *
 * Topology:
 *
 *   sender ──100 Mbps/1ms── router ──10 Mbps/5ms── sink
 *                           (CAKE diffserv4 EdgeQueueDisc)
 *     fd00:1::/64                fd00:2::/64
 *
 * Four UDP CBR flows from sender to sink over IPv6. Each flow sets its
 * IPv6 traffic-class byte so that the DS field carries a distinct DSCP:
 *
 *   Flow 0: DSCP  8 (CS1)  — maps to CAKE Bulk  tin (slot 0, share 6.25%)
 *   Flow 1: DSCP  0 (CS0)  — maps to CAKE BE    tin (slot 1, share 100%)
 *   Flow 2: DSCP 18 (AF21) — maps to CAKE Video tin (slot 2, share 50%)
 *   Flow 3: DSCP 46 (EF)   — maps to CAKE Voice tin (slot 3, share 25%)
 *
 * CAKE classifies each packet by reading the DS field from the IPv6
 * traffic-class byte (the DSCP-to-tin path is family-agnostic).
 * The diffserv4 DSCP map follows the sch_cake diffserv4[] lookup table.
 *
 * All four flows together saturate the 10 Mbps bottleneck (each offered
 * at 5 Mbps). The per-flow received byte counts show that DSCP-to-tin
 * classification works over IPv6: flows land in the expected tins and the
 * DRR-across-tin dispatcher shares the bottleneck according to tin weights.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv6-static-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/traffic-control-module.h"

#include <array>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::EdgeQueueDisc;

NS_LOG_COMPONENT_DEFINE("CakeIpv6Recipe");

// ---------------------------------------------------------------------------
// Minimal CBR sender for IPv6: sets the traffic-class byte (DSCP<<2) on
// the socket so that CAKE's DS-field classifier sees the correct DSCP.
// ---------------------------------------------------------------------------

class CbrV6App : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("CakeIpv6RecipeCbrV6App").SetParent<Application>().AddConstructor<CbrV6App>();
        return tid;
    }

    /// @param remote   IPv6 destination address + port.
    /// @param pktSize  UDP payload bytes per packet.
    /// @param bps      Constant bit rate in bits per second.
    /// @param tclass   IPv6 traffic-class byte (DSCP<<2).
    void Setup(Inet6SocketAddress remote, uint32_t pktSize, uint64_t bps, uint8_t tclass)
    {
        m_remote = remote;
        m_pktSize = pktSize;
        m_bps = bps;
        m_tclass = tclass;
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), TypeId::LookupByName("ns3::UdpSocketFactory"));
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
        m_socket->Send(Create<Packet>(m_pktSize));
        double gap = static_cast<double>(m_pktSize * 8) / static_cast<double>(m_bps);
        m_event = Simulator::Schedule(Seconds(gap), &CbrV6App::SendOne, this);
    }

    Inet6SocketAddress m_remote{Ipv6Address::GetAny(), 0};
    Ptr<Socket> m_socket;
    uint32_t m_pktSize{1000};
    uint64_t m_bps{1000000};
    uint8_t m_tclass{0};
    bool m_running{false};
    EventId m_event;
};

NS_OBJECT_ENSURE_REGISTERED(CbrV6App);

// ---------------------------------------------------------------------------
// Per-flow Rx accounting
// ---------------------------------------------------------------------------

struct FlowRx
{
    uint64_t bytes{0};
    uint64_t pkts{0};
};

static std::array<FlowRx, 4> g_rx;

static void
RxCb(uint32_t idx, Ptr<const Packet> pkt, const Address& /* addr */)
{
    g_rx[idx].bytes += pkt->GetSize();
    ++g_rx[idx].pkts;
}

int
main(int argc, char* argv[])
{
    double simTime = 10.0;
    uint64_t bottleneckBps = 10'000'000; // 10 Mbps
    uint64_t flowBps = 5'000'000;        // 5 Mbps per flow — saturating

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
    p2pAccess.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("200p"));
    NetDeviceContainer devSenderRouter = p2pAccess.Install(sender, router);

    // Shrink the driver queue so all queueing happens in the CAKE disc.
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bottleneckBps)));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devRouterSink = p2pBottleneck.Install(router, sink);

    // ---- Internet stack (IPv4 + IPv6) ----
    InternetStackHelper internet;
    internet.Install(allNodes);

    // ---- IPv6 addressing + routing (ULA, fd00::/8) ----
    // Link 1: sender (::1) <-> router (::2)
    Ipv6AddressHelper ipv6;
    ipv6.SetBase(Ipv6Address("fd00:1::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if1 = ipv6.Assign(devSenderRouter);
    if1.SetForwarding(1, true);
    if1.SetDefaultRouteInAllNodes(1);

    // Link 2: router (::1) <-> sink (::2)
    ipv6.SetBase(Ipv6Address("fd00:2::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if2 = ipv6.Assign(devRouterSink);
    if2.SetForwarding(0, true);
    if2.SetDefaultRouteInAllNodes(0);

    // Sink global unicast address on the fd00:2::/64 link.
    Ipv6Address sinkAddr = if2.GetAddress(1, 1);

    // ---- CAKE diffserv4 EdgeQueueDisc on router egress toward sink ----
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge, DataRate(bottleneckBps));
    stratum::InstallRoot(devRouterSink.Get(0), edge);

    // ---- Flows: four IPv6 UDP CBR senders, each with a distinct DSCP ----
    // CAKE diffserv4 routing (from the sch_cake diffserv4[] table):
    //   DSCP  8 (CS1)  -> Bulk  (slot 0, 6.25%)
    //   DSCP  0 (CS0)  -> BE    (slot 1, 100%)
    //   DSCP 18 (AF21) -> Video (slot 2, 50%)
    //   DSCP 46 (EF)   -> Voice (slot 3, 25%)

    struct FlowSpec
    {
        uint8_t dscp;
        uint16_t port;
        const char* tin;
        const char* share;
    };

    static const std::array<FlowSpec, 4> kFlows = {{
        {8, 5010, "Bulk  ", "6.25%"},
        {0, 5011, "BE    ", "100% "},
        {18, 5012, "Video ", "50%  "},
        {46, 5013, "Voice ", "25%  "},
    }};

    std::cout << "CAKE diffserv4 over IPv6 — four flows to distinct tins\n";
    std::cout << "Bottleneck: " << bottleneckBps / 1e6 << " Mbps  |  "
              << "Each flow offered: " << flowBps / 1e6 << " Mbps\n";
    std::cout << "Sink: " << sinkAddr << "\n\n";

    for (uint32_t i = 0; i < kFlows.size(); ++i)
    {
        const auto& f = kFlows[i];
        uint8_t tclass = static_cast<uint8_t>(f.dscp << 2);

        // Sink
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    Inet6SocketAddress(Ipv6Address::GetAny(), f.port));
        ApplicationContainer sinkApp = sinkHelper.Install(sink);
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(simTime + 0.5));
        sinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxCb, i));

        // Source
        Ptr<CbrV6App> src = CreateObject<CbrV6App>();
        src->Setup(Inet6SocketAddress(sinkAddr, f.port), 1000, flowBps, tclass);
        sender->AddApplication(src);
        src->SetStartTime(Seconds(0.5));
        src->SetStopTime(Seconds(simTime));

        std::cout << "Flow " << i << ": DSCP " << std::setw(2) << static_cast<uint32_t>(f.dscp)
                  << " -> tin " << f.tin << " (share " << f.share << "), port " << f.port << "\n";
    }

    // ---- Run ----
    Simulator::Stop(Seconds(simTime + 0.5));
    Simulator::Run();

    // ---- Per-flow results ----
    // Traffic is active from t=0.5s to t=simTime; the window is (simTime - 0.5) seconds.
    double dur = simTime - 0.5;
    std::cout << "\n=== Per-flow results (CAKE diffserv4 over IPv6) ===\n";
    std::cout << std::left << std::setw(8) << "DSCP" << std::setw(8) << "Tin" << std::setw(8)
              << "Share" << std::setw(14) << "Rx bytes" << std::setw(10) << "Rx pkts"
              << "Served Mbps\n";
    std::cout << std::string(62, '-') << "\n";

    for (uint32_t i = 0; i < kFlows.size(); ++i)
    {
        const auto& f = kFlows[i];
        double servedMbps = static_cast<double>(g_rx[i].bytes) * 8.0 / dur / 1e6;
        std::cout << std::left << std::setw(8) << static_cast<uint32_t>(f.dscp) << std::setw(8)
                  << f.tin << std::setw(8) << f.share << std::setw(14) << g_rx[i].bytes
                  << std::setw(10) << g_rx[i].pkts << std::fixed << std::setprecision(3)
                  << servedMbps << "\n";
    }

    // ---- QueueDisc aggregate stats ----
    // nTotalDequeuedPackets double-counts on hierarchical discs (each inner
    // re-dequeue is tallied again), so it is not printed here.  The
    // delivered figure (enqueued - dropped) is the reconcilable net count.
    QueueDisc::Stats stats = edge->GetStats();
    uint64_t delivered = stats.nTotalEnqueuedPackets - stats.nTotalDroppedPackets;
    std::cout << "\nDisc stats:\n";
    std::cout << "  total enqueued:               " << stats.nTotalEnqueuedPackets << " pkts\n";
    std::cout << "  total dropped:                " << stats.nTotalDroppedPackets << " pkts\n";
    std::cout << "  delivered (enqueued-dropped): " << delivered << " pkts\n";

    Simulator::Destroy();
    return 0;
}
