// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2026 Sergio Andreozzi
//
// CAKE diffserv4 + L4S composition over IPv6.
//
// Configures a CAKE diffserv4 edge with a DualPI2 instance as the per-tin
// inner queue. A scalable (DCTCP, ECT(1)) and a classic (Cubic) TCP flow
// share the same DSCP tin. The DualPI2 inner CE-marks the scalable flow
// while leaving the classic flow unaffected, demonstrating that the CAKE
// + L4S composition works over IPv6.
//
// Topology:
//
//   src-dctcp ─┐
//              ├─ 1 Gbps/1 ms ─ router ─[CAKE 40 Mbps/24 ms + DualPI2]─ sink
//   src-cubic ─┘
//
// The bottleneck is the CAKE shaped rate (40 Mbps); both links are 1 Gbps.
//
//   fd00:1::/64 (dctcp link)   fd00:3::/64   fd00:4::/64 (sink link)
//   fd00:2::/64 (cubic link)
//
// Both TCP flows carry DSCP 0 (CS0, Best Effort tin, 100% weight) so
// they land in the same tin. The DualPI2 inner drives coupled marking on
// the DCTCP flow.

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/tcp-dctcp.h"
#include "ns3/tcp-l4-protocol.h"
#include "ns3/traffic-control-module.h"

#include <iomanip>
#include <iostream>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::EdgeQueueDisc;

NS_LOG_COMPONENT_DEFINE("CakeL4sCompositionIpv6");

// ---------------------------------------------------------------------------
// Per-flow receive statistics
// ---------------------------------------------------------------------------

struct FlowStats
{
    uint64_t rxBytes{0};
    uint64_t rxPkts{0};
};

static FlowStats g_statsScalable;
static FlowStats g_statsClassic;

static void
RxCallback(FlowStats* s, Ptr<const Packet> packet, const Address& /* addr */)
{
    s->rxBytes += packet->GetSize();
    ++s->rxPkts;
}

// ---------------------------------------------------------------------------
// TCP variant resolver
// ---------------------------------------------------------------------------

namespace
{

TypeId
TcpVariantTypeId(const std::string& v)
{
    if (v == "cubic")
    {
        return TcpCubic::GetTypeId();
    }
    if (v == "dctcp")
    {
        return TcpDctcp::GetTypeId();
    }
    if (v == "newreno" || v == "reno")
    {
        return TcpNewReno::GetTypeId();
    }
    NS_FATAL_ERROR("unknown TCP variant: " << v);
    return TypeId();
}

} // namespace

// ---------------------------------------------------------------------------
// Install one TCP flow (BulkSend → PacketSink) over IPv6.
//
// The TCP congestion control is set directly on the source node's
// TcpL4Protocol (not via Config::SetDefault). Each source node hosts
// exactly one flow, so there is no order-sensitivity between calls.
// ---------------------------------------------------------------------------

static void
InstallFlow(Ptr<Node> srcNode,
            Ptr<Node> sinkNode,
            Ipv6Address sinkAddr,
            uint16_t port,
            const std::string& variant,
            double startTime,
            double stopTime,
            FlowStats* stats)
{
    Ptr<TcpL4Protocol> tcp = srcNode->GetObject<TcpL4Protocol>();
    NS_ABORT_MSG_IF(!tcp, "Source node has no TcpL4Protocol");
    tcp->SetAttribute("SocketType", TypeIdValue(TcpVariantTypeId(variant)));

    // Sink
    PacketSinkHelper sh("ns3::TcpSocketFactory", Inet6SocketAddress(Ipv6Address::GetAny(), port));
    ApplicationContainer sinkApp = sh.Install(sinkNode);
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(stopTime));
    sinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxCallback, stats));

    // Source
    BulkSendHelper bh("ns3::TcpSocketFactory", Inet6SocketAddress(sinkAddr, port));
    bh.SetAttribute("MaxBytes", UintegerValue(0));
    bh.SetAttribute("SendSize", UintegerValue(1448));
    ApplicationContainer srcApp = bh.Install(srcNode);
    srcApp.Start(Seconds(startTime));
    srcApp.Stop(Seconds(stopTime));
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int
main(int argc, char* argv[])
{
    std::string bandwidth = "40Mbps";
    double simTime = 20.0;
    uint32_t rngRun = 1;

    CommandLine cmd(__FILE__);
    cmd.AddValue("bandwidth", "Bottleneck shaped rate", bandwidth);
    cmd.AddValue("simTime", "Simulation duration (seconds)", simTime);
    cmd.AddValue("RngRun", "RngSeedManager run", rngRun);
    cmd.Parse(argc, argv);

    RngSeedManager::SetRun(rngRun);

    // DCTCP must emit ECT(1) (the scalable identifier). ns-3 TcpDctcp
    // defaults to ECT(0); flip it so the DualPI2 inner routes this flow
    // to the low-latency queue and drives coupled marking.
    Config::SetDefault("ns3::TcpDctcp::UseEct0", BooleanValue(false));

    // Enable ECN on all TCP sockets so the SYN/SYN-ACK handshake
    // negotiates ECN capability.
    Config::SetDefault("ns3::TcpSocketBase::UseEcn", StringValue("On"));

    // ---- Nodes ----
    // Two source nodes give per-node TCP variant isolation: each node's
    // TcpL4Protocol carries exactly one CC algorithm, so the CC set at
    // install time is still in effect when BulkSendApplication creates
    // its socket at Start() time.
    NodeContainer srcDctcp;
    srcDctcp.Create(1);
    NodeContainer srcCubic;
    srcCubic.Create(1);
    NodeContainer router;
    router.Create(1);
    NodeContainer sink;
    sink.Create(1);
    NodeContainer allNodes;
    allNodes.Add(srcDctcp);
    allNodes.Add(srcCubic);
    allNodes.Add(router);
    allNodes.Add(sink);

    // ---- Links ----
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pAccess.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100p"));

    NetDeviceContainer devDctcpRouter = p2pAccess.Install(srcDctcp.Get(0), router.Get(0));
    NetDeviceContainer devCubicRouter = p2pAccess.Install(srcCubic.Get(0), router.Get(0));

    // Bottleneck: shrink the driver queue to 1 packet so that the CAKE
    // disc is the only queuing point and congestion forces the DualPI2
    // inner to mark packets.
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("24ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devRouterSink = p2pBottleneck.Install(router.Get(0), sink.Get(0));

    // ---- Internet stack (IPv4 + IPv6) ----
    InternetStackHelper internet;
    internet.Install(allNodes);

    // ---- IPv6 addressing + routing (ULA, fd00::/8) ----
    Ipv6AddressHelper ipv6;

    // Access links: each source has its own /64.
    ipv6.SetBase(Ipv6Address("fd00:1::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if1 = ipv6.Assign(devDctcpRouter);
    if1.SetForwarding(1, true);
    if1.SetDefaultRouteInAllNodes(1);

    ipv6.SetBase(Ipv6Address("fd00:2::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if2 = ipv6.Assign(devCubicRouter);
    if2.SetForwarding(1, true);
    if2.SetDefaultRouteInAllNodes(1);

    // Bottleneck link.
    ipv6.SetBase(Ipv6Address("fd00:3::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if3 = ipv6.Assign(devRouterSink);
    if3.SetForwarding(0, true);
    if3.SetDefaultRouteInAllNodes(0);

    // Sink global unicast address on the fd00:3::/64 link.
    Ipv6Address sinkAddr = if3.GetAddress(1, 1);

    // ---- CAKE diffserv4 + DualPI2 inner on router egress toward sink ----
    // InstallRoot removes any pre-existing root qdisc before setting the
    // new one, so no explicit TrafficControlHelper::Uninstall is needed.
    DataRate bw(bandwidth);
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge, bw, {.tinShaping = true, .dualPi2Inner = true});
    stratum::InstallRoot(devRouterSink.Get(0), edge);

    // ---- Flows ----
    // Both flows use DSCP 0 (CS0) so they land in the CAKE Best Effort
    // tin (100% weight). Within that tin the DualPI2 inner routes the
    // scalable flow (ECT(1), set by TcpDctcp) to the L-queue and
    // CE-marks it under load; the classic flow (Cubic, Not-ECT or ECT(0))
    // goes to the classic queue.

    InstallFlow(srcDctcp.Get(0),
                sink.Get(0),
                sinkAddr,
                9000,
                "dctcp",
                1.0,
                simTime,
                &g_statsScalable);
    InstallFlow(srcCubic.Get(0),
                sink.Get(0),
                sinkAddr,
                9001,
                "cubic",
                1.0,
                simTime,
                &g_statsClassic);

    // ---- Run ----
    std::cout << "CAKE diffserv4 + DualPI2 composition over IPv6\n";
    std::cout << "Bottleneck: " << bandwidth << "  |  Sim: " << simTime << " s\n";
    std::cout << "Sink: " << sinkAddr << "\n\n";

    Simulator::Stop(Seconds(simTime + 0.5));
    Simulator::Run();

    // ---- Summary ----
    QueueDisc::Stats stats = edge->GetStats();

    // Duration: both flows active from t=1.0 to t=simTime.
    double duration = simTime - 1.0;
    auto mbps = [&](const FlowStats& s) -> double {
        return (duration > 0) ? (static_cast<double>(s.rxBytes) * 8.0 / duration / 1e6) : 0.0;
    };

    std::cout << "=== Results (CAKE diffserv4 + DualPI2 over IPv6) ===\n";
    std::cout << std::left << std::setw(24) << "Flow" << std::setw(12) << "Goodput" << "Rx bytes"
              << "\n";
    std::cout << std::string(50, '-') << "\n";

    std::cout << std::left << std::setw(24) << "Scalable (DCTCP ECT1)" << std::fixed
              << std::setprecision(2) << std::setw(7) << mbps(g_statsScalable) << " Mbps  "
              << g_statsScalable.rxBytes << "\n";

    std::cout << std::left << std::setw(24) << "Classic  (Cubic)" << std::fixed
              << std::setprecision(2) << std::setw(7) << mbps(g_statsClassic) << " Mbps  "
              << g_statsClassic.rxBytes << "\n";

    std::cout << "\n";
    std::cout << "Disc nTotalDroppedPacketsBeforeEnqueue: "
              << stats.nTotalDroppedPacketsBeforeEnqueue << "\n";
    std::cout << "Disc nTotalDroppedPackets:              " << stats.nTotalDroppedPackets << "\n";
    std::cout << "Disc nTotalMarkedPackets:               " << stats.nTotalMarkedPackets << "\n";
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }

    Simulator::Destroy();
    return 0;
}
