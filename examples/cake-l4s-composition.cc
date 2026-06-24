// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2026 Sergio Andreozzi
//
// Demonstrates the substrate's compositional value: configures CAKE
// diffserv4 with an L4S DualPI2 instance as the per-tin inner. No
// mainline Linux or BSD AQM expresses this composition today; the
// four-slot composer makes it a configuration choice.
//
// Topology: two host pairs crossing a shared bottleneck, with CAKE+L4S
// on the bottleneck egress.
//
//   src-a -- routerA -- [BOTTLENECK CAKE+L4S] -- routerB -- sink-a
//   src-b -- routerA                                       sink-b
//
// CLI controls flow count and TCP variant per host pair, so the example
// can run apples-to-apples against pure-CAKE bridge results at the
// (16,1) host-pair / flow-count cell.

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-module.h"
#include "ns3/traffic-control-module.h"

#include <fstream>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::EdgeQueueDisc;

NS_LOG_COMPONENT_DEFINE("CakeL4sComposition");

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

uint64_t
InstallFlows(Ptr<Node> srcNode,
             Ptr<Node> sinkNode,
             Ipv4Address sinkAddr,
             uint16_t basePort,
             uint32_t nFlows,
             const std::string& variant,
             double startTime,
             double stopTime,
             ApplicationContainer& sinkApps,
             ApplicationContainer& srcApps)
{
    // Set the TCP congestion control TypeId on THIS source node's
    // TcpL4Protocol specifically, not via Config::SetDefault. Setting
    // the default is order-sensitive across multiple InstallFlows calls
    // because BulkSendApplication creates its socket at Start() time,
    // not at Install() time — by then the global default has been
    // overwritten by the most recent InstallFlows call, so all flows
    // end up using whichever variant was set last.
    Ptr<TcpL4Protocol> tcp = srcNode->GetObject<TcpL4Protocol>();
    NS_ABORT_MSG_IF(!tcp, "Source node has no TcpL4Protocol");
    tcp->SetAttribute("SocketType", TypeIdValue(TcpVariantTypeId(variant)));

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        uint16_t port = basePort + i;
        PacketSinkHelper sh("ns3::TcpSocketFactory",
                            InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer s = sh.Install(sinkNode);
        s.Start(Seconds(0.0));
        s.Stop(Seconds(stopTime));
        sinkApps.Add(s);

        BulkSendHelper bh("ns3::TcpSocketFactory", InetSocketAddress(sinkAddr, port));
        bh.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer a = bh.Install(srcNode);
        a.Start(Seconds(startTime));
        a.Stop(Seconds(stopTime));
        srcApps.Add(a);
    }
    return nFlows;
}

uint64_t
SumRx(const ApplicationContainer& sinks, uint32_t startIdx, uint32_t count)
{
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        total += DynamicCast<PacketSink>(sinks.Get(startIdx + i))->GetTotalRx();
    }
    return total;
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string bandwidth = "100Mbps";
    double simTime = 30.0;
    uint32_t rngRun = 1;
    uint32_t nA = 1;
    uint32_t nB = 1;
    std::string variantA = "cubic";
    std::string variantB = "dctcp";
    std::string jsonOut;

    CommandLine cmd(__FILE__);
    cmd.AddValue("bandwidth", "Bottleneck bandwidth", bandwidth);
    cmd.AddValue("simTime", "Simulation duration (seconds)", simTime);
    cmd.AddValue("RngRun", "RngSeedManager run", rngRun);
    cmd.AddValue("nA", "Flow count from src-a to sink-a", nA);
    cmd.AddValue("nB", "Flow count from src-b to sink-b", nB);
    cmd.AddValue("variantA", "TCP variant for src-a flows: cubic|dctcp|reno", variantA);
    cmd.AddValue("variantB", "TCP variant for src-b flows: cubic|dctcp|reno", variantB);
    cmd.AddValue("jsonOut", "Per-replica JSON output path", jsonOut);
    cmd.Parse(argc, argv);

    RngSeedManager::SetRun(rngRun);

    // The scalable (DCTCP) host pair must emit ECT(1) so the per-tin DualPI2
    // inner classifies it onto the low-latency L-queue; DCTCP defaults to
    // ECT(0), which the inner treats as Classic, so without this the "L4S"
    // flow never reaches the L4S path.
    Config::SetDefault("ns3::TcpDctcp::UseEct0", BooleanValue(false));

    NodeContainer srcA, srcB, routerA, routerB, sinkA, sinkB;
    srcA.Create(1);
    srcB.Create(1);
    routerA.Create(1);
    routerB.Create(1);
    sinkA.Create(1);
    sinkB.Create(1);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1ms"));
    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    bottleneckLink.SetChannelAttribute("Delay", StringValue("5ms"));

    NetDeviceContainer dSrcA_RA = accessLink.Install(srcA.Get(0), routerA.Get(0));
    NetDeviceContainer dSrcB_RA = accessLink.Install(srcB.Get(0), routerA.Get(0));
    NetDeviceContainer dRA_RB = bottleneckLink.Install(routerA.Get(0), routerB.Get(0));
    NetDeviceContainer dRB_SinkA = accessLink.Install(routerB.Get(0), sinkA.Get(0));
    NetDeviceContainer dRB_SinkB = accessLink.Install(routerB.Get(0), sinkB.Get(0));

    InternetStackHelper internet;
    // This example stays IPv4-only on purpose: installing the IPv6 stack
    // registers per-node random variables (link-local autoconfiguration and
    // duplicate-address detection) that draw from the global random-number
    // stream and shift this example's committed output. The dual-stack
    // composition is demonstrated separately by cake-l4s-composition-ipv6.
    internet.SetIpv6StackInstall(false);
    internet.Install(srcA);
    internet.Install(srcB);
    internet.Install(routerA);
    internet.Install(routerB);
    internet.Install(sinkA);
    internet.Install(sinkB);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    ipv4.Assign(dSrcA_RA);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    ipv4.Assign(dSrcB_RA);
    ipv4.SetBase("10.2.0.0", "255.255.255.0");
    ipv4.Assign(dRA_RB);
    ipv4.SetBase("10.3.1.0", "255.255.255.0");
    Ipv4InterfaceContainer iSinkA = ipv4.Assign(dRB_SinkA);
    ipv4.SetBase("10.3.2.0", "255.255.255.0");
    Ipv4InterfaceContainer iSinkB = ipv4.Assign(dRB_SinkB);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    DataRate bw(bandwidth);
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge, bw, {.tinShaping = true, .dualPi2Inner = true});
    stratum::InstallRoot(dRA_RB.Get(0), edge);

    ApplicationContainer sinkApps, srcApps;
    InstallFlows(srcA.Get(0),
                 sinkA.Get(0),
                 iSinkA.GetAddress(1),
                 9000,
                 nA,
                 variantA,
                 1.0,
                 simTime,
                 sinkApps,
                 srcApps);
    uint32_t bStartIdx = sinkApps.GetN();
    InstallFlows(srcB.Get(0),
                 sinkB.Get(0),
                 iSinkB.GetAddress(1),
                 10000,
                 nB,
                 variantB,
                 1.0,
                 simTime,
                 sinkApps,
                 srcApps);

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    uint64_t aBytes = SumRx(sinkApps, 0, nA);
    uint64_t bBytes = SumRx(sinkApps, bStartIdx, nB);
    double duration = simTime - 1.0;
    double aMbps = (aBytes * 8.0) / (duration * 1e6);
    double bMbps = (bBytes * 8.0) / (duration * 1e6);
    double totalBytes = static_cast<double>(aBytes + bBytes);
    double aShare = totalBytes > 0 ? aBytes / totalBytes : 0.0;

    std::cout << "a_bytes=" << aBytes << " (variant=" << variantA << " n=" << nA << ")\n"
              << "b_bytes=" << bBytes << " (variant=" << variantB << " n=" << nB << ")\n"
              << "a_mbps=" << aMbps << "\n"
              << "b_mbps=" << bMbps << "\n"
              << "a_share=" << aShare << "\n";

    if (!jsonOut.empty())
    {
        std::ofstream f(jsonOut);
        f << "{\n"
          << "  \"rng_run\": " << rngRun << ",\n"
          << "  \"sim_time_s\": " << simTime << ",\n"
          << "  \"bandwidth\": \"" << bandwidth << "\",\n"
          << "  \"n_a\": " << nA << ", \"variant_a\": \"" << variantA << "\",\n"
          << "  \"n_b\": " << nB << ", \"variant_b\": \"" << variantB << "\",\n"
          << "  \"a_bytes\": " << aBytes << ",\n"
          << "  \"b_bytes\": " << bBytes << ",\n"
          << "  \"a_mbps\": " << aMbps << ",\n"
          << "  \"b_mbps\": " << bMbps << ",\n"
          << "  \"a_share\": " << aShare << ",\n"
          << "  \"total_mbps\": " << (aMbps + bMbps) << "\n"
          << "}\n";
    }

    Simulator::Destroy();
    return 0;
}
