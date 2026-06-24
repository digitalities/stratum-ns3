/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Throughput equivalence (RFC 9332 §2.1, App. C.2) between responsive L4S and
 * responsive classic flows on a shared DualPI2 bottleneck.
 *
 * Two BulkSend TCP flows share a 10 Mbps bottleneck:
 *   - Flow L: TcpDctcp with UseEct0=false (emits ECT(1)); Scalable CC
 *     per RFC 9331 §4; routes to the L4S sub-queue.
 *   - Flow C: TcpCubic with ECN enabled (emits ECT(0)); classic CC;
 *     routes to the classic sub-queue.
 *
 * The coupled marker (p_C = p'², p_L = min(k·p', 1) per
 * RFC 9332 §2.1 eq. (1)) drives both congestion controllers toward the
 * same loss/mark-equivalent signal. Throughput equivalence emerges
 * because both flows respond to the signal proportionally.
 *
 * Metrics captured:
 *   - Per-flow throughput at 50 ms granularity → flent-per-flow.csv
 *   - Periodic p', p_C, p_L → coupling.csv (existing pattern)
 *   - cwnd evolution per sender → cwnd-L.csv, cwnd-C.csv
 *
 * What this demonstrates: responsive flows under the RFC 9332 coupling
 * formula converge to throughput equivalence within ~25 %.
 *
 * What this does NOT demonstrate: latency advantage (see
 * diffserv-l4s-s1-advantage for that story).
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

NS_LOG_COMPONENT_DEFINE("DiffServL4sS2Coexistence");

namespace
{

std::ofstream g_throughputFile;
std::ofstream g_couplingFile;
std::ofstream g_cwndLFile;
std::ofstream g_cwndCFile;
Ptr<l4s::QueueDisc> g_disc;
Ptr<PacketSink> g_sinkL;
Ptr<PacketSink> g_sinkC;
uint64_t g_lastRxL = 0;
uint64_t g_lastRxC = 0;

Time g_sampleInterval = MilliSeconds(50);
Time g_simStop;
double g_warmup = 0.0;

void
SampleThroughput()
{
    if (Simulator::Now() >= g_simStop)
    {
        return;
    }
    if (Simulator::Now() < Seconds(g_warmup))
    {
        // Update the "last seen" counters so the next sample only reflects post-warmup bytes.
        g_lastRxL = g_sinkL->GetTotalRx();
        g_lastRxC = g_sinkC->GetTotalRx();
        Simulator::Schedule(g_sampleInterval, &SampleThroughput);
        return;
    }
    uint64_t rxL = g_sinkL->GetTotalRx();
    uint64_t rxC = g_sinkC->GetTotalRx();
    double bpsL = (rxL - g_lastRxL) * 8.0 / g_sampleInterval.GetSeconds();
    double bpsC = (rxC - g_lastRxC) * 8.0 / g_sampleInterval.GetSeconds();
    g_throughputFile << Simulator::Now().GetSeconds() << ',' << bpsL << ',' << bpsC << '\n';
    g_lastRxL = rxL;
    g_lastRxC = rxC;
    Simulator::Schedule(g_sampleInterval, &SampleThroughput);
}

void
SampleCoupling()
{
    if (!g_disc)
    {
        return;
    }
    if (Simulator::Now() >= g_simStop)
    {
        return;
    }
    if (Simulator::Now() < Seconds(g_warmup))
    {
        Simulator::Schedule(MilliSeconds(20), &SampleCoupling);
        return;
    }
    g_couplingFile << Simulator::Now().GetSeconds() << ',' << g_disc->GetBaseProb() << ','
                   << g_disc->GetLastClassicCoupledProb() << ',' << g_disc->GetLastL4sMarkProb()
                   << '\n';
    Simulator::Schedule(MilliSeconds(20), &SampleCoupling);
}

void
TraceCwndL(uint32_t /*oldVal*/, uint32_t newVal)
{
    g_cwndLFile << Simulator::Now().GetSeconds() << ',' << newVal << '\n';
}

void
TraceCwndC(uint32_t /*oldVal*/, uint32_t newVal)
{
    g_cwndCFile << Simulator::Now().GetSeconds() << ',' << newVal << '\n';
}

void
ConnectCwndTraces(uint32_t nodeIdL, uint32_t nodeIdC)
{
    std::string pathL = "/NodeList/" + std::to_string(nodeIdL) +
                        "/$ns3::TcpL4Protocol/SocketList/0/CongestionWindow";
    std::string pathC = "/NodeList/" + std::to_string(nodeIdC) +
                        "/$ns3::TcpL4Protocol/SocketList/0/CongestionWindow";
    Config::ConnectWithoutContext(pathL, MakeCallback(&TraceCwndL));
    Config::ConnectWithoutContext(pathC, MakeCallback(&TraceCwndC));
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string outDir = "output/ns3/diffserv-l4s-s2-coexistence";
    double simTime = 60.0;
    double warmup = 10.0;
    uint32_t bottleneckMbps = 10;
    uint32_t queueLimitC = 200;
    uint32_t queueLimitL = 200;
    std::string l4sSenderType = "tcp-dctcp";
    std::string classicSenderType = "tcp-cubic";

    CommandLine cmd(__FILE__);
    cmd.AddValue("outDir", "Output directory", outDir);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("warmup", "Warmup window dropped from analysis (s)", warmup);
    cmd.AddValue("bottleneckMbps", "Bottleneck rate (Mbps)", bottleneckMbps);
    cmd.AddValue("queueLimitC", "Classic sub-queue limit (pkts)", queueLimitC);
    cmd.AddValue("queueLimitL", "L4S sub-queue limit (pkts)", queueLimitL);
    cmd.AddValue("l4sSenderType", "L4S sender TCP variant", l4sSenderType);
    cmd.AddValue("classicSenderType", "Classic sender TCP variant", classicSenderType);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_UNLESS(l4sSenderType == "tcp-dctcp",
                        "Only tcp-dctcp is supported for --l4sSenderType in this cycle");
    NS_ABORT_MSG_UNLESS(classicSenderType == "tcp-cubic",
                        "Only tcp-cubic is supported for --classicSenderType in this cycle");

    g_simStop = Seconds(simTime);
    g_warmup = warmup;

    diffserv::EnsureDir(outDir);
    g_throughputFile.open(outDir + "/flent-per-flow.csv");
    g_couplingFile.open(outDir + "/coupling.csv");
    g_cwndLFile.open(outDir + "/cwnd-L.csv");
    g_cwndCFile.open(outDir + "/cwnd-C.csv");
    if (!g_throughputFile.is_open() || !g_couplingFile.is_open() || !g_cwndLFile.is_open() ||
        !g_cwndCFile.is_open())
    {
        NS_FATAL_ERROR("diffserv-l4s-s2-coexistence: failed to open CSV output under '"
                       << outDir << "'. Check --outDir=<writable path>.");
    }
    g_throughputFile << "time_s,bps_L,bps_C\n";
    g_couplingFile << "time_s,p_prime,p_C,p_L\n";
    g_cwndLFile << "time_s,cwnd_bytes\n";
    g_cwndCFile << "time_s,cwnd_bytes\n";

    // Topology — dumbbell: sender0 → edge → core → receiver0 (L4S flow)
    //                      sender1 → edge → core → receiver1 (classic flow)
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

    // DualPI2 qdisc on the bottleneck (edge → core direction)
    Ptr<l4s::QueueDisc> disc = CreateObject<l4s::QueueDisc>();
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

    ec.Get(0)->AggregateObject(disc);
    stratum::InstallRoot(ec.Get(0), disc);
    disc->Initialize();

    disc->ConfigQueue({.queue = 1, .prec = 0, .thMin = 100.0, .thMax = 200.0, .maxP = 0.1});
    disc->ConfigQueue({.queue = 0, .prec = 0, .thMin = 30.0, .thMax = 80.0, .maxP = 0.1});

    g_disc = disc;

    // L4S sender: TcpDctcp with UseEct0=false (ECT(1) marking, Scalable CC)
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

    // Classic sender: TcpCubic with ECN enabled (ECT(0))
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

    // Cwnd traces: connect after app-start and SYN-ACK exchange (≥ 0.6 s)
    Simulator::Schedule(Seconds(0.6),
                        &ConnectCwndTraces,
                        senders.Get(0)->GetId(),
                        senders.Get(1)->GetId());

    // Periodic samplers
    Simulator::Schedule(g_sampleInterval, &SampleThroughput);
    Simulator::Schedule(MilliSeconds(120), &SampleCoupling);

    Simulator::Stop(Seconds(simTime + 0.1));
    Simulator::Run();

    QueueDisc::Stats stats = disc->GetStats();
    std::cout << "\n==== diffserv-l4s-s2-coexistence ====\n";
    std::cout << "SimTime:              " << simTime << " s\n";
    std::cout << "Bottleneck:           " << bottleneckMbps << " Mbps\n";
    std::cout << "L rx bytes:           " << g_sinkL->GetTotalRx() << "\n";
    std::cout << "C rx bytes:           " << g_sinkC->GetTotalRx() << "\n";
    double ratio = 0.0;
    if (g_sinkC->GetTotalRx() > 0)
    {
        ratio =
            static_cast<double>(g_sinkL->GetTotalRx()) / static_cast<double>(g_sinkC->GetTotalRx());
    }
    std::cout << "L:C throughput ratio: " << std::fixed << std::setprecision(3) << ratio << "\n";
    std::cout << "Disc dropped:         " << stats.nTotalDroppedPacketsBeforeEnqueue << "\n";
    std::cout << "Disc marked:          " << stats.nTotalMarkedPackets << "\n";
    for (const auto& kv : stats.nDroppedPacketsBeforeEnqueue)
    {
        std::cout << "  drop reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    for (const auto& kv : stats.nMarkedPackets)
    {
        std::cout << "  mark reason \"" << kv.first << "\": " << kv.second << "\n";
    }
    std::cout << "Final p':             " << disc->GetBaseProb() << "\n";
    std::cout << "=====================================\n";

    g_throughputFile.close();
    g_couplingFile.close();
    g_cwndLFile.close();
    g_cwndCFile.close();

    Simulator::Destroy();
    return 0;
}
