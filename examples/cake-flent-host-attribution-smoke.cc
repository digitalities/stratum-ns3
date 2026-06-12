/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Two-host smoke fixture for the diffserv-flent-sink test suite. Five
 * saturating TCP uploads share a CAKE bottleneck with triple-isolation
 * enabled: four flows originate at host A (one outer DRR bucket under
 * triple-isolate keying), one at host B (its own outer bucket). The
 * FlentCsvSink writes flows 0..3 with hostId="A" and flow 4 with
 * hostId="B", so the test can assert the per-row host attribution in
 * tcp_up_flow*.csv.
 *
 *   hostA -- 1Gbps/1ms -- r1 -- bw/rtt -- r2 -- 1Gbps/1ms -- sinkA  (5100..5103)
 *   hostB -- 1Gbps/1ms -- r1                  -- 1Gbps/1ms -- sinkB  (5104)
 *
 * Purpose-built for the diffserv-flent-sink test; not a research scenario
 * or handbook figure source.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-flent-csv-sink.h"
#include "ns3/traffic-control-module.h"

#include <filesystem>
#include <map>
#include <string>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::FlentCsvSink;
namespace fs = std::filesystem;

int
main(int argc, char* argv[])
{
    DataRate bandwidth("100Mbps");
    Time rtt = MilliSeconds(40);
    uint32_t length = 2;
    std::string outDir = "/tmp/cake-flent-host-attribution-smoke/";

    CommandLine cmd(__FILE__);
    cmd.AddValue("bw", "Bottleneck bandwidth (DataRate)", bandwidth);
    cmd.AddValue("rtt", "End-to-end RTT (Time)", rtt);
    cmd.AddValue("length", "Simulation length in seconds", length);
    cmd.AddValue("output", "Output directory for the CSV bundle (must end with /)", outDir);
    cmd.Parse(argc, argv);

    if (!outDir.empty() && outDir.back() != '/')
    {
        outDir += '/';
    }
    fs::create_directories(outDir);

    // ---- 6-node topology, 5 P2P channels ----
    NodeContainer hostA;
    hostA.Create(1);
    NodeContainer hostB;
    hostB.Create(1);
    NodeContainer routers;
    routers.Create(2);
    NodeContainer sinkA;
    sinkA.Create(1);
    NodeContainer sinkB;
    sinkB.Create(1);

    PointToPointHelper edgeLink;
    edgeLink.SetDeviceAttribute("DataRate", DataRateValue(DataRate("1Gbps")));
    edgeLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(1)));

    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", DataRateValue(bandwidth));
    bottleneck.SetChannelAttribute("Delay", TimeValue(rtt / 2));

    NetDeviceContainer hostADev = edgeLink.Install(hostA.Get(0), routers.Get(0));
    NetDeviceContainer hostBDev = edgeLink.Install(hostB.Get(0), routers.Get(0));
    NetDeviceContainer bottleneckDev = bottleneck.Install(routers.Get(0), routers.Get(1));
    NetDeviceContainer sinkADev = edgeLink.Install(routers.Get(1), sinkA.Get(0));
    NetDeviceContainer sinkBDev = edgeLink.Install(routers.Get(1), sinkB.Get(0));

    // ---- IP stack ----
    InternetStackHelper stack;
    stack.InstallAll();

    Ipv4AddressHelper addr;
    addr.SetBase("10.1.1.0", "255.255.255.0");
    addr.Assign(hostADev);
    addr.SetBase("10.1.2.0", "255.255.255.0");
    addr.Assign(hostBDev);
    addr.SetBase("10.2.0.0", "255.255.255.0");
    addr.Assign(bottleneckDev);
    addr.SetBase("10.3.1.0", "255.255.255.0");
    Ipv4InterfaceContainer sinkAIf = addr.Assign(sinkADev);
    addr.SetBase("10.3.2.0", "255.255.255.0");
    Ipv4InterfaceContainer sinkBIf = addr.Assign(sinkBDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- CAKE best-effort with triple-isolation on r1's egress toward r2 ----
    Ptr<TrafficControlLayer> tc = routers.Get(0)->GetObject<TrafficControlLayer>();
    NS_ABORT_MSG_IF(!tc, "TrafficControlLayer missing on r1");
    if (tc->GetRootQueueDiscOnDevice(bottleneckDev.Get(0)))
    {
        tc->DeleteRootQueueDiscOnDevice(bottleneckDev.Get(0));
    }
    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeBestEffort(edge,
                                      /*totalRate=*/bandwidth,
                                      /*enableAckFilter=*/false,
                                      /*enableLlq=*/false,
                                      /*enableTinShaping=*/false,
                                      /*enableHostIsolation=*/true);
    tc->SetRootQueueDiscOnDevice(bottleneckDev.Get(0), edge);

    // ---- 5 TCP uploads: hostA × 4 to sinkA (ports 5100..5103), hostB × 1 to sinkB (5104) ----
    constexpr uint16_t kPortA0 = 5100;
    constexpr uint16_t kPortB0 = 5104;
    ApplicationContainer upSinks;
    ApplicationContainer upSources;

    for (uint16_t i = 0; i < 4; ++i)
    {
        uint16_t port = kPortA0 + i;
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer s = sinkHelper.Install(sinkA.Get(0));
        s.Start(Seconds(0));
        s.Stop(Seconds(length + 1));
        upSinks.Add(s);

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(sinkAIf.GetAddress(1), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(0)); // infinite
        ApplicationContainer src = bulk.Install(hostA.Get(0));
        src.Start(Seconds(0.1));
        src.Stop(Seconds(length));
        upSources.Add(src);
    }
    {
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), kPortB0));
        ApplicationContainer s = sinkHelper.Install(sinkB.Get(0));
        s.Start(Seconds(0));
        s.Stop(Seconds(length + 1));
        upSinks.Add(s);

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(sinkBIf.GetAddress(1), kPortB0));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer src = bulk.Install(hostB.Get(0));
        src.Start(Seconds(0.1));
        src.Stop(Seconds(length));
        upSources.Add(src);
    }

    // ---- FlentCsvSink: flows 0..3 = host A, flow 4 = host B ----
    FlentCsvSink flent;
    flent.SetTestName("cake-flent-host-attribution-smoke");
    flent.SetStepSize(MilliSeconds(200));
    flent.SetLength(Seconds(length));
    flent.SetOutputDir(outDir);
    for (uint32_t i = 0; i < 4; ++i)
    {
        flent.AddTcpUpFlow(i, DynamicCast<PacketSink>(upSinks.Get(i)), "A");
    }
    flent.AddTcpUpFlow(4, DynamicCast<PacketSink>(upSinks.Get(4)), "B");

    std::map<std::string, std::string> meta;
    meta["bw"] = std::to_string(bandwidth.GetBitRate());
    meta["rtt_ms"] = std::to_string(rtt.GetMilliSeconds());
    flent.StampMetadata(meta);

    Simulator::Stop(Seconds(length + 1));
    Simulator::Run();
    flent.Finalize();
    Simulator::Destroy();
    return 0;
}
