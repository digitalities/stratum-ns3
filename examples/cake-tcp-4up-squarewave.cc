/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Square-wave 4-flow TCP fairness: four BulkSendApplications start at
 * t=0/5/10/15 s and stop at t=45/50/55/60 s, traversing a single
 * bottleneck shaped by cake::Helper rate-based mode. The FlentCsvSink
 * emits a per-flow CSV bundle compatible with the tcp_4up_squarewave
 * Flent schema.
 *
 *   senders[0..3] -- 1Gbps/1ms -- r1 -- bw/rtt -- r2 -- 1Gbps/1ms -- sinks[0..3]
 *                                  |
 *                                  +-- cake::Helper RateBased shaper on r1 egress
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-flent-csv-sink.h"
#include "ns3/stratum-helper.h"
#include "ns3/traffic-control-module.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::FlentCsvSink;

NS_LOG_COMPONENT_DEFINE("CakeTcp4UpSquarewave");

namespace
{

void
EnsureDirLocal(const std::string& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        NS_ABORT_MSG("create_directories(" << path << ") failed: " << ec.message());
    }
}

std::string
FormatFloat3(double v)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << v;
    return oss.str();
}

} // namespace

int
main(int argc, char* argv[])
{
    DataRate bandwidth("100Mbps");
    Time rtt = MilliSeconds(40);
    double length = 60.0;
    std::string outDir = "output/ns3/cake-tcp-4up-squarewave/100mbps-40ms/";
    std::string ns3Sha;

    CommandLine cmd(__FILE__);
    cmd.AddValue("bw", "Bottleneck bandwidth (DataRate)", bandwidth);
    cmd.AddValue("rtt", "End-to-end RTT (Time)", rtt);
    cmd.AddValue("length", "Simulation length in seconds", length);
    cmd.AddValue("output", "Output directory for the CSV bundle", outDir);
    cmd.AddValue("ns3Sha", "Optional ns-3 build SHA for metadata.json", ns3Sha);
    cmd.Parse(argc, argv);

    EnsureDirLocal(outDir);

    const Time halfRtt = rtt / 2;
    const uint32_t nFlows = 4;
    const uint16_t baseTcpUpPort = 5100;

    NodeContainer senders;
    senders.Create(nFlows);
    NodeContainer receivers;
    receivers.Create(nFlows);
    NodeContainer routers;
    routers.Create(2);
    NodeContainer client;
    client.Create(1);
    NodeContainer pingPeer;
    pingPeer.Create(1);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    accessLink.SetChannelAttribute("Delay", StringValue("1ms"));
    PointToPointHelper bottleneck;
    bottleneck.SetDeviceAttribute("DataRate", DataRateValue(bandwidth));
    bottleneck.SetChannelAttribute("Delay", TimeValue(halfRtt));

    InternetStackHelper stack;
    stack.Install(senders);
    stack.Install(receivers);
    stack.Install(routers);
    stack.Install(client);
    stack.Install(pingPeer);

    Ipv4AddressHelper addr;
    std::vector<Ipv4InterfaceContainer> senderIfs(nFlows);
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        NetDeviceContainer dev = accessLink.Install(senders.Get(i), routers.Get(0));
        std::ostringstream net;
        net << "10.1." << (i + 1) << ".0";
        addr.SetBase(net.str().c_str(), "255.255.255.0");
        senderIfs[i] = addr.Assign(dev);
    }

    NetDeviceContainer clientDev = accessLink.Install(client.Get(0), routers.Get(0));
    addr.SetBase("10.1.10.0", "255.255.255.0");
    addr.Assign(clientDev);

    NetDeviceContainer bottleneckDev = bottleneck.Install(routers.Get(0), routers.Get(1));
    addr.SetBase("10.2.1.0", "255.255.255.0");
    addr.Assign(bottleneckDev);

    std::vector<Ipv4InterfaceContainer> receiverIfs(nFlows);
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        NetDeviceContainer dev = accessLink.Install(routers.Get(1), receivers.Get(i));
        std::ostringstream net;
        net << "10.3." << (i + 1) << ".0";
        addr.SetBase(net.str().c_str(), "255.255.255.0");
        receiverIfs[i] = addr.Assign(dev);
    }

    NetDeviceContainer pingPeerDev = accessLink.Install(routers.Get(1), pingPeer.Get(0));
    addr.SetBase("10.3.10.0", "255.255.255.0");
    Ipv4InterfaceContainer pingPeerIfs = addr.Assign(pingPeerDev);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // --- Install cake::Helper RateBased shaper on r1 egress toward r2 ---
    cake::Helper helper;
    helper.SetShaperMode(cake::Helper::ShaperMode::RateBased);
    helper.SetGlobalRateBps(bandwidth.GetBitRate());
    helper.SetTinRateBpsAll(bandwidth.GetBitRate());
    helper.SetTinCount(nFlows);
    helper.BuildAndInstall(bottleneckDev.Get(0));

    // --- Workload: 4 TCP up, staggered start/stop, ICMP ping ---
    ApplicationContainer upSinks;
    ApplicationContainer upSources;

    const double starts[4] = {0.0, 5.0, 10.0, 15.0};
    const double stops[4] = {45.0, 50.0, 55.0, 60.0};

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        const uint16_t port = baseTcpUpPort + i;
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer s = sinkHelper.Install(receivers.Get(i));
        s.Start(Seconds(0.0));
        s.Stop(Seconds(length + 1.0));
        upSinks.Add(s);

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(receiverIfs[i].GetAddress(1), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        ApplicationContainer src = bulk.Install(senders.Get(i));
        src.Start(Seconds(starts[i]));
        src.Stop(Seconds(stops[i]));
        upSources.Add(src);
    }

    // ICMP ping from client to pingPeer. Cadence matches the export step
    // size so the ping series populates one sample per x_values bin.
    PingHelper pingHelper(pingPeerIfs.GetAddress(1));
    pingHelper.SetAttribute("Interval", TimeValue(MilliSeconds(200)));
    pingHelper.SetAttribute("VerboseMode", EnumValue(Ping::SILENT));
    ApplicationContainer pingApps = pingHelper.Install(client.Get(0));
    pingApps.Start(Seconds(0.5));
    pingApps.Stop(Seconds(length));
    Ptr<Ping> pingApp = DynamicCast<Ping>(pingApps.Get(0));

    // --- Wire FlentCsvSink ---
    FlentCsvSink sink;
    sink.SetTestName("tcp_4up_squarewave");
    sink.SetStepSize(MilliSeconds(200));
    sink.SetLength(Seconds(length));
    sink.SetOutputDir(outDir);

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        sink.AddTcpUpFlow(i, DynamicCast<PacketSink>(upSinks.Get(i)));
    }
    if (pingApp)
    {
        sink.AddIcmpProbe(pingApp);
    }

    std::map<std::string, std::string> meta = {
        {"test_name", "tcp_4up_squarewave"},
        {"length_s", FormatFloat3(length)},
        {"step_size_s", FormatFloat3(0.200)},
        {"bandwidth_bps", std::to_string(bandwidth.GetBitRate())},
        {"rtt_ms", FormatFloat3(rtt.GetSeconds() * 1000.0)},
        {"topology_class", "dumbbell, single bottleneck"},
        {"aqm", "RateBasedShaperDispatcher"},
        {"dscp_map", "{}"},
        {"ns3_build_sha", ns3Sha},
    };
    sink.StampMetadata(meta);

    Simulator::Schedule(Seconds(length + 0.5), &FlentCsvSink::Finalize, &sink);
    Simulator::Stop(Seconds(length + 1.0));
    Simulator::Run();
    sink.Finalize();

    std::cout << "[cake-tcp-4up-squarewave] wrote bundle to " << outDir << std::endl;

    Simulator::Destroy();
    return 0;
}
