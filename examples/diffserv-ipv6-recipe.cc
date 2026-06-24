/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * DiffServ over IPv6 — minimal dual-stack recipe.
 *
 * Topology:
 *
 *   sender ---- router/edge ---- sink
 *               (EdgeQueueDisc)
 *     fd00:1::/64    fd00:2::/64
 *
 * Three UDP flows from sender to sink carry distinct DSCPs via edge
 * mark rules keyed on destination port:
 *
 *   Port 9  (EF,   DSCP 46) — Expedited Forwarding
 *   Port 10 (AF21, DSCP 18) — Assured Forwarding class 2
 *   Port 11 (BE,   DSCP  0) — Best Effort
 *
 * The EdgeQueueDisc on the router's egress toward the sink classifies
 * packets, assigns DSCPs, and routes them to three sub-queues via a
 * PHB table. A PriorityQueueScheduler serves queue 0 (EF) first,
 * demonstrating IPv6 DSCP-based traffic differentiation.
 *
 * Expected output: per-class served-byte and packet counts printed at
 * the end of the simulation, with EF receiving the highest share of
 * the bottleneck capacity.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv6-static-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/traffic-control-module.h"

#include <array>
#include <iomanip>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::MredMode;
using ns3::stratum::PriorityScheduler;

NS_LOG_COMPONENT_DEFINE("DiffServIpv6Recipe");

// ---------------------------------------------------------------------------
// Per-class byte counters (indexed by class: 0=EF, 1=AF21, 2=BE)
// ---------------------------------------------------------------------------
static uint64_t g_rxBytes[3] = {0, 0, 0};
static uint32_t g_rxPkts[3] = {0, 0, 0};

/// Rx callback — updates the per-class counter for the flow whose
/// destination port determines the class (9=EF, 10=AF21, 11=BE).
static void
RxCallback(uint32_t classId, Ptr<const Packet> packet, const Address& /* addr */)
{
    g_rxBytes[classId] += packet->GetSize();
    ++g_rxPkts[classId];
}

int
main(int argc, char* argv[])
{
    double simTime = 15.0;

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
    // Access link: sender <-> router (100 Mbps, 1 ms)
    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pAccess.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("100p"));
    NetDeviceContainer devSenderRouter = p2pAccess.Install(sender, router);

    // Bottleneck link: router <-> sink (2 Mbps, 5 ms)
    // NetDevice queue set to 1 packet so all queueing is in the
    // DiffServ EdgeQueueDisc.
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("2Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));
    NetDeviceContainer devRouterSink = p2pBottleneck.Install(router, sink);

    // ---- Internet stack (IPv4 + IPv6) ----
    InternetStackHelper internet;
    internet.Install(allNodes);

    // ---- IPv6 addresses and routing ----
    // Link 1: sender (::1) <-> router (::2)
    Ipv6AddressHelper ipv6;
    ipv6.SetBase(Ipv6Address("fd00:1::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if1 = ipv6.Assign(devSenderRouter);
    // Enable forwarding on router's interface (index 1 = router side).
    // SetDefaultRouteInAllNodes installs a default route in sender via
    // the router's link-1 address.
    if1.SetForwarding(1, true);
    if1.SetDefaultRouteInAllNodes(1);

    // Link 2: router (::1) <-> sink (::2)
    ipv6.SetBase(Ipv6Address("fd00:2::"), Ipv6Prefix(64));
    Ipv6InterfaceContainer if2 = ipv6.Assign(devRouterSink);
    // Enable forwarding on router's interface (index 0 = router side).
    // SetDefaultRouteInAllNodes installs a default route in sink via
    // the router's link-2 address, enabling return packets.
    if2.SetForwarding(0, true);
    if2.SetDefaultRouteInAllNodes(0);

    // Sink address on link 2: if2.GetAddress(1, 1) is the global unicast
    // address assigned to the sink (node index 1, address index 1,
    // skipping the link-local at index 0).
    Ipv6Address sinkAddr = if2.GetAddress(1, 1);

    // ---- DiffServ EdgeQueueDisc on router egress toward sink ----

    // Configure before installation (Initialize() locks the sub-queue count).
    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();

    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    // 3 queues: EF (0), AF21 (1), BE (2); one precedence level each
    edgeInner->SetNumQueues(3);
    edgeInner->SetNumPrec(0, 1); // EF
    edgeInner->SetNumPrec(1, 1); // AF21
    edgeInner->SetNumPrec(2, 1); // BE

    edgeInner->SetQueueLimit(0, 20); // EF
    edgeInner->SetQueueLimit(1, 30); // AF21
    edgeInner->SetQueueLimit(2, 50); // BE

    // Scheduler: strict priority — queue 0 (EF) served first
    Ptr<PriorityScheduler> sched = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                 UintegerValue(3),
                                                                                 "WinLen",
                                                                                 DoubleValue(1.0));
    edgeInner->SetScheduler(sched);

    // Mark rules: classify by destination port → assign DSCP
    edgeDisc->AddMarkRule({.dscp = 46, .dstPort = 9});  // EF
    edgeDisc->AddMarkRule({.dscp = 18, .dstPort = 10}); // AF21
    edgeDisc->AddMarkRule({.dscp = 0, .dstPort = 11});  // BE

    // Dumb policers (pass-through; no metering in this recipe)
    helper.AddDumbPolicy(edgeDisc, 46);
    helper.AddDumbPolicy(edgeDisc, 18);
    helper.AddDumbPolicy(edgeDisc, 0);

    // PHB table: DSCP → (queue, precedence)
    helper.AddPhbEntry(edgeInner, 46, 0, 0); // EF  → queue 0
    helper.AddPhbEntry(edgeInner, 18, 1, 0); // AF21 → queue 1
    helper.AddPhbEntry(edgeInner, 0, 2, 0);  // BE  → queue 2

    // Install on router's egress device toward sink
    Ptr<NetDevice> routerEgressDev = devRouterSink.Get(0); // router side
    stratum::InstallRoot(routerEgressDev, edgeDisc);
    edgeDisc->Initialize();

    // Configure DROP_TAIL per sub-queue (after Initialize)
    edgeInner->SetMredModeAllQueues(MredMode::DROP_TAIL);
    helper.ConfigQueue(edgeInner,
                       {.queue = 0, .prec = 0, .thMin = 20.0, .thMax = 20.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 1, .prec = 0, .thMin = 30.0, .thMax = 30.0, .maxP = 1.0});
    helper.ConfigQueue(edgeInner,
                       {.queue = 2, .prec = 0, .thMin = 50.0, .thMax = 50.0, .maxP = 1.0});

    // ---- Traffic: three UDP flows, all sender -> sink ----
    // EF: 1 Mbps (port 9) — over-subscribes the 2 Mbps bottleneck together
    // AF21: 800 kbps (port 10)
    // BE: 600 kbps (port 11)
    // Total offered: 2.4 Mbps > 2 Mbps bottleneck, so BE gets squeezed.

    struct FlowSpec
    {
        uint16_t port;
        uint32_t classId;
        uint64_t rateBps;
        uint32_t pktSize;
        const char* label;
    };

    std::array<FlowSpec, 3> flows = {{{9, 0, 1000000, 512, "EF   (DSCP 46)"},
                                      {10, 1, 800000, 512, "AF21 (DSCP 18)"},
                                      {11, 2, 600000, 512, "BE   (DSCP  0)"}}};

    for (auto& f : flows)
    {
        // Sink
        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    Inet6SocketAddress(Ipv6Address::GetAny(), f.port));
        ApplicationContainer sinkApp = sinkHelper.Install(sink);
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(simTime));

        // Wire per-class Rx callback (captures classId by value)
        uint32_t cid = f.classId;
        sinkApp.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxCallback, cid));

        // Source: OnOff at constant rate (headless — no ramp-up)
        OnOffHelper src("ns3::UdpSocketFactory", Inet6SocketAddress(sinkAddr, f.port));
        src.SetConstantRate(DataRate(f.rateBps), f.pktSize);
        ApplicationContainer srcApp = src.Install(sender);
        srcApp.Start(Seconds(1.0));
        srcApp.Stop(Seconds(simTime));

        double rateMbps = static_cast<double>(f.rateBps) / 1e6;
        std::cout << "Flow " << f.label << ": port " << f.port << ", offered " << rateMbps
                  << " Mbps\n";
    }

    // ---- Run ----
    std::cout << "Bottleneck: 2 Mbps; simulation time: " << simTime << " s\n";
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ---- Per-class results ----
    double dur = simTime - 1.0; // traffic active for (simTime - 1) seconds
    std::cout << "\n=== Per-class results (IPv6 DiffServ) ===\n";
    std::cout << "Class       Rx bytes    Rx pkts  Served Mbps\n";
    std::cout << "--------- ----------  --------- -----------\n";
    const char* labels[] = {"EF   (DSCP 46)", "AF21 (DSCP 18)", "BE   (DSCP  0)"};
    for (uint32_t i = 0; i < 3; ++i)
    {
        double servedMbps = static_cast<double>(g_rxBytes[i]) * 8.0 / dur / 1e6;
        std::cout << labels[i] << " " << std::setw(10) << g_rxBytes[i] << "  " << std::setw(9)
                  << g_rxPkts[i] << "  " << std::fixed << std::setprecision(3) << servedMbps
                  << "\n";
    }

    // Built-in per-code-point stats from the EdgeQueueDisc
    edgeDisc->PrintStats();

    Simulator::Destroy();
    return 0;
}
