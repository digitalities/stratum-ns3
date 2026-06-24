/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * CAKE host-isolation jitter-floor demonstrator.
 *
 * Four bulk TCP flows from host A and one from host B contend at a single
 * CAKE instance (diffserv4, triple host isolation) on a 100 Mbit bottleneck.
 * Host A's flows and host B's flow are distinct (source, destination) host
 * pairs, the regime in which destination and triple isolation take effect.
 * Host isolation targets an equal split between the two hosts (many-flow-host
 * share 0.50); a per-flow scheduler would instead give host A 0.80.
 *
 *   host A (nA flows) --1Gb/1ms-- rA ==100Mb/20ms== rB --1Gb/1ms-- sink A
 *   host B (nB flows) --1Gb/1ms--/                       \--1Gb/1ms-- sink B
 *                                  ^ CAKE diffserv4, triple-isolate
 *
 * A deterministic simulation of this scenario gives host A more than the
 * host-fair 0.50: with socket buffers above the path bandwidth-delay product
 * (so neither host is receive-window-limited) the many-flow host settles near
 * 0.65. A real Linux bottleneck running the same scenario settles lower,
 * because its hardware-interrupt and software-interrupt dispatch timing
 * varies from packet to packet, whereas the simulator's event schedule is
 * exact.
 *
 * This example reintroduces that missing timing variation as a measurement
 * device: every jitterPeriodMs it redraws the bottleneck serialization rate
 * as nominal * (1 + U[-jitterPct, +jitterPct]), on BOTH bottleneck directions
 * (the forward data path and the reverse acknowledgement path), preserving the
 * mean link rate. Perturbing the acknowledgement return is what breaks the
 * deterministic TCP feedback loop. With jitter the many-flow-host share falls
 * toward the host-fair value.
 *
 * The example also reports per-host backlog occupancy: the fraction of time
 * each host has at least one packet queued at the bottleneck, sampled at 1 kHz
 * from the queue disc's enqueue and dequeue trace sources (offload is off, so
 * enqueue and dequeue are one-to-one). It quantifies whether the share split
 * is driven by one host being backlogged more than the other.
 *
 * Run with no arguments to see the jitter case; pass --jitterPct=0 for the
 * deterministic baseline. Sweep --run to characterise the seed ensemble.
 *
 *   ./ns3 run "cake-host-iso-jitter-floor"                  # jitter case
 *   ./ns3 run "cake-host-iso-jitter-floor --jitterPct=0"    # deterministic
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-install-helper.h"
#include "ns3/traffic-control-module.h"

#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;
using namespace ns3::stratum;

namespace
{
constexpr double kBandwidthBps = 100e6; // 100 Mbit/s bottleneck

// Sinks, indexed per source host, for the byte-share measurement.
std::vector<Ptr<PacketSink>> g_sinksA;
std::vector<Ptr<PacketSink>> g_sinksB;

// Shared bottleneck rate-jitter. Periodically redraws the link rate as
// nominal * (1 + U[-pct, +pct]) on both bottleneck directions, floored so the
// link never stalls entirely. This breaks the deterministic dispatch phase a
// fixed-rate link imposes.
Ptr<NetDevice> g_devTx; // forward (data) bottleneck device
Ptr<NetDevice> g_devRx; // reverse (acknowledgement) bottleneck device
Ptr<UniformRandomVariable> g_jitterRv;
double g_jitterPct = 0.5;
double g_jitterPeriodNs = 5e5; // 0.5 ms

void
JitterRate()
{
    double mult = 1.0 + g_jitterRv->GetValue(-g_jitterPct, g_jitterPct);
    if (mult < 0.05)
    {
        mult = 0.05;
    }
    DataRateValue drv(DataRate(static_cast<uint64_t>(kBandwidthBps * mult)));
    g_devTx->SetAttribute("DataRate", drv);
    g_devRx->SetAttribute("DataRate", drv);
    Simulator::Schedule(NanoSeconds(g_jitterPeriodNs), &JitterRate);
}

// Per-host backlog occupancy. Classify each item at the root queue disc by
// source host, keep a per-host in-queue count, and 1 kHz-sample whether each
// host has at least one packet queued.
Ipv4Address g_hostAAddr;
Ipv4Address g_hostBAddr;
int64_t g_qA = 0;
int64_t g_qB = 0;
double g_busyA = 0.0;
double g_busyB = 0.0;
uint32_t g_occSamples = 0;

void
EnqCb(Ptr<const QueueDiscItem> item)
{
    Ptr<const Ipv4QueueDiscItem> ip = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ip)
    {
        return;
    }
    Ipv4Address s = ip->GetHeader().GetSource();
    if (s == g_hostAAddr)
    {
        ++g_qA;
    }
    else if (s == g_hostBAddr)
    {
        ++g_qB;
    }
}

void
DeqCb(Ptr<const QueueDiscItem> item)
{
    Ptr<const Ipv4QueueDiscItem> ip = DynamicCast<const Ipv4QueueDiscItem>(item);
    if (!ip)
    {
        return;
    }
    Ipv4Address s = ip->GetHeader().GetSource();
    if (s == g_hostAAddr && g_qA > 0)
    {
        --g_qA;
    }
    else if (s == g_hostBAddr && g_qB > 0)
    {
        --g_qB;
    }
}

void
OccSample(double dt)
{
    if (g_qA > 0)
    {
        g_busyA += dt;
    }
    if (g_qB > 0)
    {
        g_busyB += dt;
    }
    ++g_occSamples;
    Simulator::Schedule(Seconds(dt), &OccSample, dt);
}
} // namespace

int
main(int argc, char* argv[])
{
    bool hostIso = true;
    uint32_t nA = 4;
    uint32_t nB = 1;
    double dur = 30.0;
    uint32_t run = 1;
    double jitterPeriodMs = 0.5;
    uint32_t buf = 4194304; // 4 MB, above the path bandwidth-delay product
    uint32_t mss = 536;

    CommandLine cmd;
    cmd.AddValue("jitterPct", "bottleneck rate-jitter amplitude in [0, 0.95]; 0 = deterministic",
                 g_jitterPct);
    cmd.AddValue("jitterPeriodMs", "rate-jitter redraw period in ms", jitterPeriodMs);
    cmd.AddValue("run", "RNG run number (seed)", run);
    cmd.AddValue("hostIso", "enable CAKE host isolation", hostIso);
    cmd.AddValue("nA", "number of bulk flows from host A", nA);
    cmd.AddValue("nB", "number of bulk flows from host B", nB);
    cmd.AddValue("buf", "TCP send/receive socket buffer size in bytes", buf);
    cmd.AddValue("mss", "TCP maximum segment size in bytes", mss);
    cmd.AddValue("dur", "run duration in seconds", dur);
    cmd.Parse(argc, argv);

    g_jitterPeriodNs = jitterPeriodMs * 1e6;

    RngSeedManager::SetRun(run);
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(buf));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(buf));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(mss));

    NodeContainer hosts;
    hosts.Create(2);
    NodeContainer sinks;
    sinks.Create(2);
    NodeContainer routerA;
    routerA.Create(1);
    NodeContainer routerB;
    routerB.Create(1);

    PointToPointHelper hostToRouter;
    hostToRouter.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    hostToRouter.SetChannelAttribute("Delay", StringValue("1ms"));

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", DataRateValue(DataRate(kBandwidthBps)));
    bottleneck.SetChannelAttribute("Delay", StringValue("20ms"));

    PointToPointHelper routerToSink;
    routerToSink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    routerToSink.SetChannelAttribute("Delay", StringValue("1ms"));

    NetDeviceContainer dA = hostToRouter.Install(hosts.Get(0), routerA.Get(0));
    NetDeviceContainer dB = hostToRouter.Install(hosts.Get(1), routerA.Get(0));
    NetDeviceContainer dBN = bottleneck.Install(routerA.Get(0), routerB.Get(0));
    NetDeviceContainer dSinkA = routerToSink.Install(routerB.Get(0), sinks.Get(0));
    NetDeviceContainer dSinkB = routerToSink.Install(routerB.Get(0), sinks.Get(1));

    InternetStackHelper internet;
    internet.InstallAll();

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer iA = ipv4.Assign(dA);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer iB = ipv4.Assign(dB);
    ipv4.SetBase("10.2.0.0", "255.255.255.0");
    ipv4.Assign(dBN);
    ipv4.SetBase("10.3.1.0", "255.255.255.0");
    Ipv4InterfaceContainer iSinkA = ipv4.Assign(dSinkA);
    ipv4.SetBase("10.3.2.0", "255.255.255.0");
    Ipv4InterfaceContainer iSinkB = ipv4.Assign(dSinkB);

    g_hostAAddr = iA.GetAddress(0); // host A source address
    g_hostBAddr = iB.GetAddress(0); // host B source address

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge,
                                     DataRate(kBandwidthBps),
                                     {.tinShaping = true, .hostIsolation = hostIso});

    stratum::InstallRoot(dBN.Get(0), edge);

    edge->TraceConnectWithoutContext("Enqueue", MakeCallback(&EnqCb));
    edge->TraceConnectWithoutContext("Dequeue", MakeCallback(&DeqCb));

    auto startBulkFlow = [&](Ptr<Node> srcNode,
                             Ipv4Address sinkAddr,
                             uint16_t port,
                             Ptr<Node> sinkNode,
                             std::vector<Ptr<PacketSink>>& out) {
        BulkSendHelper src("ns3::TcpSocketFactory", InetSocketAddress(sinkAddr, port));
        src.SetAttribute("MaxBytes", UintegerValue(0));
        src.SetAttribute("SendSize", UintegerValue(1448));
        ApplicationContainer srcApp = src.Install(srcNode);
        srcApp.Start(Seconds(0.1));
        srcApp.Stop(Seconds(dur));

        PacketSinkHelper snk("ns3::TcpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer snkApp = snk.Install(sinkNode);
        snkApp.Start(Seconds(0.0));
        snkApp.Stop(Seconds(dur + 1.0));
        out.push_back(DynamicCast<PacketSink>(snkApp.Get(0)));
    };

    for (uint32_t i = 0; i < nA; ++i)
    {
        startBulkFlow(hosts.Get(0),
                      iSinkA.GetAddress(1),
                      static_cast<uint16_t>(9000 + i),
                      sinks.Get(0),
                      g_sinksA);
    }
    for (uint32_t i = 0; i < nB; ++i)
    {
        startBulkFlow(hosts.Get(1),
                      iSinkB.GetAddress(1),
                      static_cast<uint16_t>(10000 + i),
                      sinks.Get(1),
                      g_sinksB);
    }

    if (g_jitterPct > 0.0)
    {
        g_devTx = dBN.Get(0);
        g_devRx = dBN.Get(1);
        g_jitterRv = CreateObject<UniformRandomVariable>();
        Simulator::Schedule(Seconds(0.1), &JitterRate);
    }

    // 1 kHz per-host backlog occupancy sampler, started after a 5 s warmup.
    Simulator::Schedule(Seconds(5.0), &OccSample, 0.001);

    Simulator::Stop(Seconds(dur + 1.0));
    Simulator::Run();

    double bytesA = 0;
    for (auto& s : g_sinksA)
    {
        bytesA += static_cast<double>(s->GetTotalRx());
    }
    double bytesB = 0;
    for (auto& s : g_sinksB)
    {
        bytesB += static_cast<double>(s->GetTotalRx());
    }
    double shareA = (bytesA + bytesB > 0) ? bytesA / (bytesA + bytesB) : 0.0;
    double goodputMbps = (bytesA + bytesB) * 8.0 / (dur - 0.1) / 1e6;

    double sampled = g_occSamples * 0.001;
    double occA = (sampled > 0) ? g_busyA / sampled : 0.0;
    double occB = (sampled > 0) ? g_busyB / sampled : 0.0;
    double occRatio = (occB > 0) ? occA / occB : 0.0;

    std::cout << std::fixed;
    std::cout << "=== CAKE host-isolation jitter-floor demonstrator ===\n";
    std::cout << "config: hostIso=" << hostIso << " jitterPct=" << std::setprecision(2)
              << g_jitterPct << " jitterPeriodMs=" << jitterPeriodMs << " buf=" << buf
              << " mss=" << mss << " nA=" << nA << " nB=" << nB << " run=" << run << "\n";
    std::cout << "many-flow-host share_A = " << std::setprecision(4) << shareA
              << "   [host-fair 0.50, per-flow-fair 0.80]\n";
    std::cout << "aggregate goodput = " << std::setprecision(1) << goodputMbps << " Mbit/s of 100\n";
    std::cout << "per-host backlog occupancy (1 kHz): A=" << std::setprecision(1) << occA * 100.0
              << "% B=" << occB * 100.0 << "% ratio=" << std::setprecision(2) << occRatio << "\n";

    Simulator::Destroy();
    return 0;
}
