/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * CAKE substrate demonstrator: drives one UDP CBR per CAKE tin through
 * a EdgeQueueDisc configured via cake::Helper::SetAsCakeDiffserv4.
 * Each tin carries a saturating CBR (above its own rate-cap) so the
 * per-tin TBF + DRR-across-tin dispatcher determine how the bottleneck
 * is shared.
 *
 *   sender ─── 100 Mbps / 1 ms ─── edge ─── 10 Mbps / 5 ms ─── receiver
 *                                    └── EdgeQueueDisc with:
 *                                         tin 0 = TBF(0.0625x) -> FqCobalt (Bulk,    DSCP CS1)
 *                                         tin 1 = TBF(1.00x)   -> FqCobalt (BE,      DSCP CS0)
 *                                         tin 2 = TBF(0.50x)   -> FqCobalt (Video,   DSCP AF41)
 *                                         tin 3 = TBF(0.25x)   -> FqCobalt (Voice,   DSCP EF)
 *
 * Outputs in --outDir:
 *   - per-tin-rates.csv    — per-tin received bytes/s, vs configured share
 *   - summary.txt          — totals + Jain fairness across tins
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/traffic-control-module.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/stat.h>

using namespace ns3;
namespace cake = ns3::stratum::cake;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;

NS_LOG_COMPONENT_DEFINE("DiffServCake");

namespace
{

void
EnsureDirLocal(const std::string& path)
{
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    {
        NS_ABORT_MSG("mkdir(" << path << ") failed: " << std::strerror(errno));
    }
}

struct TinSpec
{
    const char* name;
    uint8_t dscp;
    double share;
};

} // namespace

int
main(int argc, char* argv[])
{
    std::string outDir = "/tmp/diffserv-cake";
    double simTime = 8.0;
    uint64_t totalRateBps = 10'000'000;
    uint64_t flowRateBps = 5'000'000; // each tin is offered 5 Mbps; saturating

    CommandLine cmd(__FILE__);
    cmd.AddValue("outDir", "Directory to write CSV outputs", outDir);
    cmd.AddValue("simTime", "Simulation duration in seconds", simTime);
    cmd.AddValue("totalRateBps", "Aggregate bottleneck rate", totalRateBps);
    cmd.AddValue("flowRateBps", "Per-tin offered CBR rate", flowRateBps);
    cmd.Parse(argc, argv);

    EnsureDirLocal(outDir);

    static const std::array<TinSpec, 4> kTins = {{
        {"Bulk", 8, 0.0625}, // CS1
        {"BE", 0, 1.0},      // CS0 / default
        {"Video", 34, 0.5},  // AF41
        {"Voice", 46, 0.25}, // EF
    }};

    // --- Topology ---
    NodeContainer senders;
    senders.Create(kTins.size());
    NodeContainer routers;
    routers.Create(2); // src-side edge + dst-side router
    NodeContainer sinks;
    sinks.Create(1);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1ms"));
    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", DataRateValue(DataRate(totalRateBps)));
    bottleneck.SetChannelAttribute("Delay", StringValue("5ms"));
    PointToPointHelper sinkLink;
    sinkLink.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    sinkLink.SetChannelAttribute("Delay", StringValue("1ms"));

    InternetStackHelper stack;
    stack.Install(senders);
    stack.Install(routers);
    stack.Install(sinks);

    Ipv4AddressHelper addr;
    std::vector<Ipv4InterfaceContainer> senderIfs(kTins.size());
    for (uint32_t i = 0; i < kTins.size(); ++i)
    {
        NetDeviceContainer dev = accessLink.Install(senders.Get(i), routers.Get(0));
        std::ostringstream ssNet;
        ssNet << "10.1." << (i + 1) << ".0";
        addr.SetBase(ssNet.str().c_str(), "255.255.255.0");
        senderIfs[i] = addr.Assign(dev);
    }

    NetDeviceContainer bottleneckDev = bottleneck.Install(routers.Get(0), routers.Get(1));
    addr.SetBase("10.2.1.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIfs = addr.Assign(bottleneckDev);

    NetDeviceContainer sinkDev = sinkLink.Install(routers.Get(1), sinks.Get(0));
    addr.SetBase("10.3.1.0", "255.255.255.0");
    Ipv4InterfaceContainer sinkIfs = addr.Assign(sinkDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // --- Install DiffServ edge with CAKE on the bottleneck egress ---
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge, DataRate(totalRateBps));

    // Mark rules: each sender's source IP -> tin's DSCP
    diffserv::Helper helper;
    for (uint32_t i = 0; i < kTins.size(); ++i)
    {
        edge->AddMarkRule({.dscp = kTins[i].dscp, .srcAddr = senderIfs[i].GetAddress(0)});
        helper.AddDumbPolicy(edge, kTins[i].dscp);
    }

    Ptr<NetDevice> bottleneckEgress = bottleneckDev.Get(0);
    stratum::InstallRoot(bottleneckEgress, edge);

    // --- Apps: one CBR per tin, saturating ---
    const uint16_t basePort = 5000;
    ApplicationContainer sinkApps;
    ApplicationContainer sourceApps;
    for (uint32_t i = 0; i < kTins.size(); ++i)
    {
        const uint16_t port = basePort + i;

        PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        sinkApps.Add(sinkHelper.Install(sinks.Get(0)));

        OnOffHelper onOff("ns3::UdpSocketFactory", InetSocketAddress(sinkIfs.GetAddress(1), port));
        onOff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1]"));
        onOff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        onOff.SetAttribute("DataRate", DataRateValue(DataRate(flowRateBps)));
        onOff.SetAttribute("PacketSize", UintegerValue(1000));
        onOff.SetAttribute("Tos", UintegerValue(static_cast<uint8_t>(kTins[i].dscp << 2)));
        sourceApps.Add(onOff.Install(senders.Get(i)));
    }

    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(simTime + 1.0));
    sourceApps.Start(Seconds(0.5));
    sourceApps.Stop(Seconds(simTime));

    FlowMonitorHelper fmh;
    Ptr<FlowMonitor> mon = fmh.InstallAll();

    Simulator::Stop(Seconds(simTime + 1.0));
    Simulator::Run();

    // --- Per-tin rate accounting ---
    mon->CheckForLostPackets();
    auto classifier = DynamicCast<Ipv4FlowClassifier>(fmh.GetClassifier());
    std::array<uint64_t, 4> rxBytes{};
    for (const auto& kv : mon->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(kv.first);
        if (t.protocol != 17)
        {
            continue;
        }
        for (uint32_t i = 0; i < kTins.size(); ++i)
        {
            if (t.destinationPort == basePort + i)
            {
                rxBytes[i] += kv.second.rxBytes;
            }
        }
    }

    std::ofstream csv(outDir + "/per-tin-rates.csv");
    csv << "tin,dscp,share,rxBytes,rxBps\n";
    const double measureSpan = simTime - 0.5;
    double sumBps = 0.0;
    double sumBpsSq = 0.0;
    for (uint32_t i = 0; i < kTins.size(); ++i)
    {
        const double bps = (rxBytes[i] * 8.0) / measureSpan;
        sumBps += bps;
        sumBpsSq += bps * bps;
        csv << kTins[i].name << "," << static_cast<uint32_t>(kTins[i].dscp) << "," << kTins[i].share
            << "," << rxBytes[i] << "," << std::fixed << std::setprecision(0) << bps << "\n";
    }
    csv.close();

    const double jain = (sumBps * sumBps) / (kTins.size() * sumBpsSq);

    std::ofstream summary(outDir + "/summary.txt");
    summary << "CAKE diffserv4 substrate run\n"
            << "totalRate=" << totalRateBps << " bps\n"
            << "simTime=" << simTime << " s\n";
    for (uint32_t i = 0; i < kTins.size(); ++i)
    {
        const double bps = (rxBytes[i] * 8.0) / measureSpan;
        summary << "tin " << i << " (" << kTins[i].name << ", DSCP "
                << static_cast<uint32_t>(kTins[i].dscp) << ", share " << kTins[i].share
                << "): rx=" << std::fixed << std::setprecision(2) << (bps / 1.0e6) << " Mbps\n";
    }
    summary << "aggregate: " << std::fixed << std::setprecision(2) << (sumBps / 1.0e6) << " Mbps\n"
            << "Jain fairness across tins: " << std::fixed << std::setprecision(3) << jain << "\n";
    summary.close();

    std::cout << "[diffserv-cake] wrote " << outDir << "/{per-tin-rates.csv,summary.txt}\n"
              << "  aggregate=" << std::fixed << std::setprecision(2) << (sumBps / 1.0e6)
              << " Mbps  Jain=" << std::fixed << std::setprecision(3) << jain << "\n";

    Simulator::Destroy();
    return 0;
}
