/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Cross-validation of two RFC 9332 DualPI2 implementations under
 * identical conditions: the diffserv in-tree l4s::QueueDisc and the
 * upstream-shaped ns3::DualPi2QueueDisc carried via patches/ns3 from
 * the Veras et al. reference port (arXiv:2603.20166v1,
 * github.com/GPRT/l4s-for-ns3).
 *
 * Topology — 4-node dumbbell:
 *
 *   senderL  ──┐                                        ┌── receiverL
 *              edge ── bottleneck (BW, baseRtt/2) ── core
 *   senderC  ──┘                                        └── receiverC
 *
 * Both senders use BulkSendHelper. The L4S sender is TcpDctcp with
 * UseEct0=false (Scalable CC per RFC 9331 §4, emits ECT(1)). The
 * classic sender is TcpCubic with ECN enabled (emits ECT(0)). Same
 * sender/receiver/access configuration regardless of qdisc choice; only
 * the bottleneck root qdisc differs.
 *
 * Output:
 *   - flent-per-flow.csv: per-flow throughput at 50 ms granularity
 *   - summary.txt: post-warmup goodput totals and JFI
 *
 * Pass --rootQdisc=l4s, --rootQdisc=cake, or --rootQdisc=gprt to select
 * the qdisc driving the bottleneck. "l4s" is the bare L4S client (a
 * standalone l4s::QueueDisc / DualPI2); "cake" is the CAKE client
 * composing a per-tin L4S inner; "gprt" is the external reference
 * DualPi2QueueDisc. All other parameters are identical between runs to
 * permit a parity comparison.
 *
 * Jain's Fairness Index (n=2):
 *   JFI = (gL + gC)² / (2 · (gL² + gC²))
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/tcp-cubic.h"
#include "ns3/tcp-dctcp.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>

using namespace ns3;
namespace cake = ns3::stratum::cake;
namespace diffserv = ns3::stratum::diffserv;
namespace l4s = ns3::stratum::l4s;
using ns3::stratum::EdgeQueueDisc;

NS_LOG_COMPONENT_DEFINE("DiffServL4sDualPi2GprtParity");

namespace
{

Ptr<PacketSink> g_sinkL;
Ptr<PacketSink> g_sinkC;
uint64_t g_lastRxL = 0;
uint64_t g_lastRxC = 0;

Time g_sampleInterval = MilliSeconds(50);
Time g_simStop;
double g_warmup = 0.0;

std::ofstream g_throughputFile;

void
SampleThroughput()
{
    if (Simulator::Now() >= g_simStop)
    {
        return;
    }
    if (Simulator::Now() < Seconds(g_warmup))
    {
        g_lastRxL = g_sinkL->GetTotalRx();
        g_lastRxC = g_sinkC->GetTotalRx();
        Simulator::Schedule(g_sampleInterval, &SampleThroughput);
        return;
    }
    const uint64_t rxL = g_sinkL->GetTotalRx();
    const uint64_t rxC = g_sinkC->GetTotalRx();
    const double dtSec = g_sampleInterval.GetSeconds();
    const double bpsL = static_cast<double>(rxL - g_lastRxL) * 8.0 / dtSec;
    const double bpsC = static_cast<double>(rxC - g_lastRxC) * 8.0 / dtSec;
    g_throughputFile << Simulator::Now().GetSeconds() << ',' << bpsL << ',' << bpsC << '\n';
    g_lastRxL = rxL;
    g_lastRxC = rxC;
    Simulator::Schedule(g_sampleInterval, &SampleThroughput);
}

Ptr<l4s::QueueDisc>
ConfigureL4sDualPi2OnDevice(Ptr<NetDevice> bottleneckDev,
                            uint32_t queueLimitC,
                            uint32_t queueLimitL,
                            l4s::QueueDisc::ClassicAqm classicAqm)
{
    Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
    disc->SetClassicAqm(classicAqm);
    disc->SetNumQueues(2);
    disc->SetL4sQueueIdx(1);
    disc->SetQueueLimit(0, queueLimitC);
    disc->SetQueueLimit(1, queueLimitL);
    disc->AddPhbEntry(0, 0, 0);

    Ptr<l4s::CoupledScheduler> sched =
        CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                          UintegerValue(2),
                                                          "L4sQueueIdx",
                                                          UintegerValue(1),
                                                          "BurstCap",
                                                          UintegerValue(8));
    disc->SetScheduler(sched);

    bottleneckDev->AggregateObject(disc);
    Ptr<TrafficControlLayer> tcl = bottleneckDev->GetNode()->GetObject<TrafficControlLayer>();
    NS_ABORT_MSG_UNLESS(tcl, "TrafficControlLayer must be aggregated on the bottleneck node");
    if (tcl->GetRootQueueDiscOnDevice(bottleneckDev))
    {
        tcl->DeleteRootQueueDiscOnDevice(bottleneckDev);
    }
    tcl->SetRootQueueDiscOnDevice(bottleneckDev, disc);
    disc->Initialize();
    return disc;
}

Ptr<QueueDisc>
ConfigureGprtDualPi2OnDevice(Ptr<NetDevice> bottleneckDev,
                             Time l4sThreshold,
                             uint32_t classicWeight)
{
    Ptr<TrafficControlLayer> tcl = bottleneckDev->GetNode()->GetObject<TrafficControlLayer>();
    if (tcl && tcl->GetRootQueueDiscOnDevice(bottleneckDev))
    {
        tcl->DeleteRootQueueDiscOnDevice(bottleneckDev);
    }
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::DualPi2QueueDisc",
                         "L4SMarkThreshold",
                         TimeValue(l4sThreshold),
                         "ClassicWeight",
                         UintegerValue(classicWeight));
    NetDeviceContainer one;
    one.Add(bottleneckDev);
    tch.Install(one);
    return tcl ? tcl->GetRootQueueDiscOnDevice(bottleneckDev) : nullptr;
}

// Depth-first search for the L4S dual-queue disc nested inside a CAKE
// composition (a per-tin inner reached through the edge's class hierarchy).
Ptr<l4s::QueueDisc>
FindL4sInTree(Ptr<QueueDisc> q)
{
    if (!q)
    {
        return nullptr;
    }
    if (Ptr<l4s::QueueDisc> self = DynamicCast<l4s::QueueDisc>(q))
    {
        return self;
    }
    for (uint32_t i = 0; i < q->GetNQueueDiscClasses(); ++i)
    {
        if (Ptr<l4s::QueueDisc> r = FindL4sInTree(q->GetQueueDiscClass(i)->GetQueueDisc()))
        {
            return r;
        }
    }
    return nullptr;
}

// Install CAKE diffserv4 with a per-tin DualPI2 inner on the bottleneck,
// at the same regime the l4s/gprt paths run. Both flows are DSCP 0,
// so they share the besteffort tin and exercise the per-tin DualPI2
// coupling exactly as the bare L4S path does — isolating the effect
// of the CAKE wrapper on coupling fairness.
Ptr<QueueDisc>
ConfigureCakeDualPi2OnDevice(Ptr<NetDevice> bottleneckDev, DataRate rate, bool tinShaping)
{
    Ptr<TrafficControlLayer> tcl = bottleneckDev->GetNode()->GetObject<TrafficControlLayer>();
    if (tcl && tcl->GetRootQueueDiscOnDevice(bottleneckDev))
    {
        tcl->DeleteRootQueueDiscOnDevice(bottleneckDev);
    }
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge,
                                     rate,
                                     /*enableAckFilter=*/false,
                                     /*enableLlq=*/false,
                                     tinShaping,
                                     /*enableHostIsolation=*/false,
                                     /*useInnerTbfShaping=*/false,
                                     /*enableAckFilterAggressive=*/false,
                                     /*useDualPi2Inner=*/true);
    tcl->SetRootQueueDiscOnDevice(bottleneckDev, edge);
    edge->Initialize();
    return edge;
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string outDir = "output/ns3/diffserv-l4s-dualpi2-gprt-parity";
    std::string rootQdisc = "l4s";
    std::string bottleneckRateStr = "40Mbps";
    double baseRttMs = 50.0;
    double l4sThresholdMs = 1.0;
    double simTime = 60.0;
    double warmup = 10.0;
    uint32_t queueLimitC = 500;
    uint32_t queueLimitL = 500;
    uint32_t rngRun = 1;
    uint32_t gprtClassicWeight = 10;
    std::string l4sClassicAqm = "coupledOnly";
    bool cakeTinShaping = true;

    CommandLine cmd(__FILE__);
    cmd.AddValue("outDir", "Output directory", outDir);
    cmd.AddValue("rootQdisc",
                 "Bottleneck root qdisc: 'l4s' (bare QueueDisc), 'cake' (CAKE "
                 "client composing a per-tin QueueDisc inner), or 'gprt' "
                 "(reference DualPi2QueueDisc)",
                 rootQdisc);
    cmd.AddValue("bottleneckRate", "Bottleneck data rate (e.g. '40Mbps')", bottleneckRateStr);
    cmd.AddValue("baseRttMs", "Base RTT (ms); bottleneck one-way delay = baseRttMs/2", baseRttMs);
    cmd.AddValue("l4sThresholdMs", "L4S step-AQM marking threshold (ms)", l4sThresholdMs);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("warmup", "Warmup window dropped from analysis (s)", warmup);
    cmd.AddValue("queueLimitC", "Classic sub-queue limit (pkts; l4s path only)", queueLimitC);
    cmd.AddValue("queueLimitL", "L4S sub-queue limit (pkts; l4s path only)", queueLimitL);
    cmd.AddValue("rngRun", "RngRun seed", rngRun);
    cmd.AddValue("gprtClassicWeight",
                 "GPRT DualPi2 ClassicWeight (0-100, default 10; only used when rootQdisc=gprt)",
                 gprtClassicWeight);
    cmd.AddValue("l4sClassicAqm",
                 "L4S-path QueueDisc ClassicAqm mode: 'coupledOnly' (default) or 'wred'",
                 l4sClassicAqm);
    cmd.AddValue("cakeTinShaping",
                 "CAKE per-tin token-bucket shaping (only used when rootQdisc=cake)",
                 cakeTinShaping);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_UNLESS(rootQdisc == "l4s" || rootQdisc == "gprt" || rootQdisc == "cake",
                        "--rootQdisc must be 'l4s', 'gprt' or 'cake' (got '" << rootQdisc << "')");

    RngSeedManager::SetRun(rngRun);

    g_simStop = Seconds(simTime);
    g_warmup = warmup;

    diffserv::EnsureDir(outDir);
    g_throughputFile.open(outDir + "/flent-per-flow.csv");
    NS_ABORT_MSG_UNLESS(g_throughputFile.is_open(),
                        "Failed to open " << outDir << "/flent-per-flow.csv for writing");
    g_throughputFile << "time_s,bps_L,bps_C\n";

    NodeContainer senders;
    senders.Create(2);
    NodeContainer edgeNode;
    edgeNode.Create(1);
    NodeContainer coreNode;
    coreNode.Create(1);
    NodeContainer receivers;
    receivers.Create(2);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1us"));

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate", DataRateValue(DataRate(bottleneckRateStr)));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(baseRttMs / 2.0)));
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
    ip.Assign(ec);
    ip.SetBase("10.0.3.0", "255.255.255.0");
    Ipv4InterfaceContainer ifC0r = ip.Assign(c0r);
    ip.SetBase("10.0.4.0", "255.255.255.0");
    Ipv4InterfaceContainer ifC1r = ip.Assign(c1r);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ptr<l4s::QueueDisc> l4sDisc;
    Ptr<QueueDisc> gprtDisc;
    Ptr<QueueDisc> cakeDisc;
    Ptr<l4s::QueueDisc> cakeInner;
    if (rootQdisc == "l4s")
    {
        l4s::QueueDisc::ClassicAqm mode = l4s::QueueDisc::ClassicAqm::CoupledOnly;
        if (l4sClassicAqm == "wred")
        {
            mode = l4s::QueueDisc::ClassicAqm::Wred;
        }
        else if (l4sClassicAqm != "coupledOnly")
        {
            NS_ABORT_MSG("--l4sClassicAqm must be 'coupledOnly' or 'wred'");
        }
        l4sDisc = ConfigureL4sDualPi2OnDevice(ec.Get(0), queueLimitC, queueLimitL, mode);
    }
    else if (rootQdisc == "cake")
    {
        cakeDisc =
            ConfigureCakeDualPi2OnDevice(ec.Get(0), DataRate(bottleneckRateStr), cakeTinShaping);
        cakeInner = FindL4sInTree(cakeDisc);
    }
    else
    {
        gprtDisc = ConfigureGprtDualPi2OnDevice(ec.Get(0),
                                                MilliSeconds(l4sThresholdMs),
                                                gprtClassicWeight);
    }

    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpDctcp"));
    Config::SetDefault("ns3::TcpDctcp::UseEct0", BooleanValue(false));
    Config::SetDefault("ns3::TcpSocketBase::UseEcn", EnumValue(TcpSocketState::On));

    constexpr uint16_t kPortL = 9001;
    BulkSendHelper bulkL("ns3::TcpSocketFactory", InetSocketAddress(ifC0r.GetAddress(1), kPortL));
    bulkL.SetAttribute("MaxBytes", UintegerValue(0));
    ApplicationContainer appL = bulkL.Install(senders.Get(0));
    appL.Start(Seconds(0.5));
    appL.Stop(Seconds(simTime));

    PacketSinkHelper sinkLHelper("ns3::TcpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), kPortL));
    ApplicationContainer sinkLApp = sinkLHelper.Install(receivers.Get(0));
    sinkLApp.Start(Seconds(0.0));
    sinkLApp.Stop(Seconds(simTime));
    g_sinkL = DynamicCast<PacketSink>(sinkLApp.Get(0));

    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));
    Config::SetDefault("ns3::TcpSocketBase::UseEcn", EnumValue(TcpSocketState::On));

    constexpr uint16_t kPortC = 9002;
    BulkSendHelper bulkC("ns3::TcpSocketFactory", InetSocketAddress(ifC1r.GetAddress(1), kPortC));
    bulkC.SetAttribute("MaxBytes", UintegerValue(0));
    ApplicationContainer appC = bulkC.Install(senders.Get(1));
    appC.Start(Seconds(0.5));
    appC.Stop(Seconds(simTime));

    PacketSinkHelper sinkCHelper("ns3::TcpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), kPortC));
    ApplicationContainer sinkCApp = sinkCHelper.Install(receivers.Get(1));
    sinkCApp.Start(Seconds(0.0));
    sinkCApp.Stop(Seconds(simTime));
    g_sinkC = DynamicCast<PacketSink>(sinkCApp.Get(0));

    Simulator::Schedule(g_sampleInterval, &SampleThroughput);

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    std::cout << "\n==== qdisc instrumentation ====\n";
    if (l4sDisc)
    {
        const QueueDisc::Stats st = l4sDisc->GetStats();
        std::cout << "l4s p' (final): " << std::fixed << std::setprecision(6)
                  << l4sDisc->GetBaseProb() << '\n'
                  << "l4s marked:     " << st.nTotalMarkedPackets << '\n'
                  << "l4s dropped:    " << st.nTotalDroppedPacketsBeforeEnqueue << '\n';
    }
    if (gprtDisc)
    {
        const QueueDisc::Stats st = gprtDisc->GetStats();
        std::cout << "gprt marked:        " << st.nTotalMarkedPackets << '\n'
                  << "gprt dropped:       " << st.nTotalDroppedPacketsBeforeEnqueue << '\n';
    }
    if (cakeInner)
    {
        std::cout << "cake inner p' (final): " << std::fixed << std::setprecision(6)
                  << cakeInner->GetBaseProb() << '\n';
    }
    else if (cakeDisc)
    {
        std::cout << "cake inner p': (DualPI2 inner not reachable in tree)\n";
    }

    const uint64_t rxL = g_sinkL->GetTotalRx();
    const uint64_t rxC = g_sinkC->GetTotalRx();
    const double measuredSec = simTime - warmup;
    const double tL = static_cast<double>(rxL);
    const double tC = static_cast<double>(rxC);
    double jfi = 0.0;
    if (tL > 0.0 || tC > 0.0)
    {
        jfi = std::pow(tL + tC, 2) / (2.0 * (tL * tL + tC * tC));
    }

    const double goodputL_mbps = (tL * 8.0) / (measuredSec * 1.0e6);
    const double goodputC_mbps = (tC * 8.0) / (measuredSec * 1.0e6);

    std::ofstream summary(outDir + "/summary.txt");
    NS_ABORT_MSG_UNLESS(summary.is_open(),
                        "Failed to open " << outDir << "/summary.txt for writing");
    summary << "rootQdisc=" << rootQdisc << '\n'
            << "bottleneckRate=" << bottleneckRateStr << '\n'
            << "baseRttMs=" << baseRttMs << '\n'
            << "l4sThresholdMs=" << l4sThresholdMs << '\n'
            << "simTime=" << simTime << '\n'
            << "warmup=" << warmup << '\n'
            << "rngRun=" << rngRun << '\n'
            << "rxBytes_L=" << rxL << '\n'
            << "rxBytes_C=" << rxC << '\n'
            << "goodput_L_mbps=" << std::fixed << std::setprecision(3) << goodputL_mbps << '\n'
            << "goodput_C_mbps=" << goodputC_mbps << '\n'
            << "jfi=" << std::setprecision(4) << jfi << '\n';
    summary.close();

    std::cout << "\n==== diffserv-l4s-dualpi2-gprt-parity ====\n"
              << "rootQdisc:       " << rootQdisc << '\n'
              << "bottleneckRate:  " << bottleneckRateStr << '\n'
              << "baseRttMs:       " << baseRttMs << '\n'
              << "l4sThresholdMs:  " << l4sThresholdMs << '\n'
              << "rngRun:          " << rngRun << '\n'
              << "goodput L (Mbps): " << std::fixed << std::setprecision(3) << goodputL_mbps << '\n'
              << "goodput C (Mbps): " << goodputC_mbps << '\n'
              << "JFI:             " << std::setprecision(4) << jfi << '\n'
              << "==========================================\n";

    g_throughputFile.close();
    Simulator::Destroy();
    return 0;
}
