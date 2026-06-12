/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Lower-Effort (LE) PHB demonstration, per RFC 8622 (2019).
 *
 * LE is a post-2001 PHB not covered by the original DiffServ4NS module; this
 * example shows that Stratum ports it trivially because LE is just "a PHB with
 * strict priority below Best-Effort". No new code is needed beyond the
 * existing queue disc + PQ scheduler: we simply wire a 2-queue PQ router
 * with BE at priority 0 and LE at priority 1.
 *
 * RFC 8622 §5 specifies DSCP 1 (binary 000001) for LE. Behaviour: LE flows
 * yield bandwidth to BE under congestion — the "less than best effort"
 * class, intended for opportunistic bulk traffic (backups, software
 * updates, telemetry uploads).
 *
 * Topology:
 *
 *   sBE ---\
 *           \
 *            e1 ----[ 1 Mbps, 10 ms ]---- e2 ---- sink
 *           /
 *   sLE ---/
 *
 *   sBE, sLE: two UDP CBR senders at 800 kbps each (together 160 % of link).
 *   e1:       DiffServ edge with 2-queue PQ scheduler (BE prio 0, LE prio 1).
 *   e2:       plain router.
 *   sink:     single UDP sink on port 9.
 *
 * Expected outcome:
 *   - BE throughput: ~800 kbps (full offered load; fits in link budget).
 *   - LE throughput: ~0–200 kbps (starved by BE; only fills idle slots).
 *
 * Output: one ServiceRate.tr with (time, BE_kbps, LE_kbps) columns.
 *
 * @see RFC 8622 — A Lower-Effort Per-Hop Behavior (LE PHB)
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <fstream>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::MredMode;
using ns3::stratum::PriorityScheduler;

NS_LOG_COMPONENT_DEFINE("DiffServExampleLE");

namespace
{
constexpr uint8_t kDscpBE = 0; // Best Effort (RFC 2474)
constexpr uint8_t kDscpLE = 1; // Lower Effort (RFC 8622 §5)

Ptr<EdgeQueueDisc> g_edgeDisc;
std::ofstream g_serviceRateFile;

void
RecordDepartureRate()
{
    double now = Simulator::Now().GetSeconds();
    Ptr<stratum::Scheduler> sched = g_edgeDisc->GetScheduler();
    double beKbps = sched->GetDepartureRate(0, -1) / 1000.0;
    double leKbps = sched->GetDepartureRate(1, -1) / 1000.0;
    g_serviceRateFile << now << " " << beKbps << " " << leKbps << "\n";
    Simulator::Schedule(Seconds(1.0), &RecordDepartureRate);
}
} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 30.0;
    uint32_t seed = 42;
    std::string outputDir = "output/ns3/example-le";

    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation time (s)", simTime);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("outputDir", "Output directory for traces", outputDir);
    cmd.Parse(argc, argv);

    diffserv::EnsureDir(outputDir);
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    // --- Topology ----------------------------------------------------------
    NodeContainer senders;
    senders.Create(2); // sBE, sLE
    NodeContainer routers;
    routers.Create(2); // e1, e2
    NodeContainer sinks;
    sinks.Create(1); // sink
    Ptr<Node> sBE = senders.Get(0);
    Ptr<Node> sLE = senders.Get(1);
    Ptr<Node> e1 = routers.Get(0);
    Ptr<Node> e2 = routers.Get(1);
    Ptr<Node> sink = sinks.Get(0);

    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pAccess.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("1Mbps"));
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("10ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    NetDeviceContainer devBE = p2pAccess.Install(sBE, e1);
    NetDeviceContainer devLE = p2pAccess.Install(sLE, e1);
    NetDeviceContainer devE1E2 = p2pBottleneck.Install(e1, e2);
    NetDeviceContainer devSink = p2pAccess.Install(e2, sink);

    InternetStackHelper internet;
    NodeContainer allNodes;
    allNodes.Add(senders);
    allNodes.Add(routers);
    allNodes.Add(sinks);
    internet.Install(allNodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer ifBE = addr.Assign(devBE);
    addr.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer ifLE = addr.Assign(devLE);
    addr.SetBase("10.0.3.0", "255.255.255.0");
    Ipv4InterfaceContainer ifE1E2 = addr.Assign(devE1E2);
    addr.SetBase("10.0.4.0", "255.255.255.0");
    Ipv4InterfaceContainer ifSink = addr.Assign(devSink);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ipv4Address sinkAddr = ifSink.GetAddress(1);

    // --- DiffServ edge on e1 -> e2 bottleneck ------------------------------
    TrafficControlHelper tchUninstall;
    tchUninstall.Uninstall(devE1E2.Get(0));
    tchUninstall.Uninstall(devE1E2.Get(1));

    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    auto inner = CreateObject<stratum::RedQueueDisc>();
    edge->SetInnerDisc(inner);
    diffserv::Helper helper;

    inner->SetNumQueues(2);
    inner->SetNumPrec(0, 1); // BE queue (single precedence)
    inner->SetNumPrec(1, 1); // LE queue (single precedence)
    inner->SetQueueLimit(0, 50);
    inner->SetQueueLimit(1, 50);
    inner->SetMredMode(MredMode::DROP_TAIL);

    // Strict-priority scheduler: index 0 highest. BE above LE — the whole
    // point of LE (RFC 8622): starve when BE has packets ready.
    Ptr<PriorityScheduler> sched = CreateObjectWithAttributes<PriorityScheduler>("NumQueues",
                                                                                 UintegerValue(2),
                                                                                 "WinLen",
                                                                                 DoubleValue(1.0));
    inner->SetScheduler(sched);

    // Classify incoming DSCPs into the right queue.
    helper.AddPhbEntry(inner, kDscpBE, 0, 0); // DSCP 0 -> queue 0, prec 0
    helper.AddPhbEntry(inner, kDscpLE, 1, 0); // DSCP 1 -> queue 1, prec 0

    // Marking is done at the sender via socket Tos attribute (see below);
    // the edge just enforces the PHB. No mark rules installed here.

    Ptr<TrafficControlLayer> tc = e1->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(devE1E2.Get(0), edge);
    edge->Initialize();

    g_edgeDisc = edge;

    // --- UDP sink on 'sink' node -------------------------------------------
    uint16_t port = 9;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApps = sinkHelper.Install(sink);
    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(simTime));

    // --- UDP CBR senders (both 800 kbps, together 1.6× link capacity) -----
    OnOffHelper beOnOff("ns3::UdpSocketFactory", InetSocketAddress(sinkAddr, port));
    beOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
    beOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    beOnOff.SetAttribute("DataRate", StringValue("800kb/s"));
    beOnOff.SetAttribute("PacketSize", UintegerValue(500));
    beOnOff.SetAttribute("Tos", UintegerValue(kDscpBE << 2)); // DSCP in top 6 bits of ToS
    ApplicationContainer beApps = beOnOff.Install(sBE);
    beApps.Start(Seconds(1.0));
    beApps.Stop(Seconds(simTime));

    OnOffHelper leOnOff("ns3::UdpSocketFactory", InetSocketAddress(sinkAddr, port));
    leOnOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
    leOnOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
    leOnOff.SetAttribute("DataRate", StringValue("800kb/s"));
    leOnOff.SetAttribute("PacketSize", UintegerValue(500));
    leOnOff.SetAttribute("Tos", UintegerValue(kDscpLE << 2)); // DSCP in top 6 bits of ToS
    ApplicationContainer leApps = leOnOff.Install(sLE);
    leApps.Start(Seconds(1.0));
    leApps.Stop(Seconds(simTime));

    // --- Trace output ------------------------------------------------------
    g_serviceRateFile.open(outputDir + "/ServiceRate.tr");
    Simulator::Schedule(Seconds(1.0), &RecordDepartureRate);

    // --- Run ---------------------------------------------------------------
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();
    g_serviceRateFile.close();

    std::cout << "\nLE PHB demo complete. Traces: " << outputDir << "/ServiceRate.tr\n"
              << "Expected: BE ~800 kbps, LE ~0-200 kbps (starved under PQ).\n";
    return 0;
}
