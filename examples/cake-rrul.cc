/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * RRUL-style workload (4 saturating TCP downloads + 4 saturating TCP
 * uploads + 4 UDP probes + 1 ICMP ping) over a dumbbell with a single
 * bottleneck shaped by the rate-based CAKE dispatcher (cake::Helper
 * RateBased mode). Hosts: 4 senders + 4 receivers + 1 client + 1 ping
 * peer + 2 routers (12 nodes total). The FlentCsvSink emits a per-
 * flow CSV bundle whose schema is documented in
 * scripts/flent-export/SCHEMA.md.
 *
 *   senders[0..3] -- 1Gbps/1ms -- r1 -- bw/rtt -- r2 -- 1Gbps/1ms -- sinks[0..3]
 *                                  |
 *                                  +-- cake::Helper RateBased shaper on r1 egress
 *
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
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace ns3;
namespace cake = ns3::stratum::cake;
using ns3::stratum::FlentCsvSink;
using ns3::stratum::FlentUdpProbeClient;
using ns3::stratum::FlentUdpProbeServer;

NS_LOG_COMPONENT_DEFINE("CakeRrul");

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
    DataRate bandwidth("50Mbps");
    Time rtt = MilliSeconds(80);
    double length = 60.0;
    std::string outDir = "output/cake-rrul-50mbps-80ms/";
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
    const uint16_t baseTcpDownPort = 5000;
    const uint16_t baseTcpUpPort = 5100;
    const uint16_t baseUdpPort = 9000;

    // --- Topology ---
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
    Ipv4InterfaceContainer clientIfs = addr.Assign(clientDev);

    NetDeviceContainer bottleneckDev = bottleneck.Install(routers.Get(0), routers.Get(1));
    addr.SetBase("10.2.1.0", "255.255.255.0");
    Ipv4InterfaceContainer bottleneckIfs = addr.Assign(bottleneckDev);

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

    // --- Workload: 4 TCP down, 4 TCP up, 4 UDP probes, 1 ICMP ping ---
    ApplicationContainer downSinks;
    ApplicationContainer downSources;
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        const uint16_t port = baseTcpDownPort + i;
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        downSinks.Add(sinkHelper.Install(receivers.Get(i)));

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(receiverIfs[i].GetAddress(1), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        downSources.Add(bulk.Install(senders.Get(i)));
    }

    ApplicationContainer upSinks;
    ApplicationContainer upSources;
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        const uint16_t port = baseTcpUpPort + i;
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        upSinks.Add(sinkHelper.Install(senders.Get(i)));

        BulkSendHelper bulk("ns3::TcpSocketFactory",
                            InetSocketAddress(senderIfs[i].GetAddress(0), port));
        bulk.SetAttribute("MaxBytes", UintegerValue(0));
        upSources.Add(bulk.Install(receivers.Get(i)));
    }

    // UDP probes: client -> pingPeer node, four ports.
    std::vector<Ptr<FlentUdpProbeClient>> udpClients;
    udpClients.reserve(nFlows);
    ApplicationContainer udpServers;
    for (uint32_t i = 0; i < nFlows; ++i)
    {
        const uint16_t port = baseUdpPort + i;

        Ptr<FlentUdpProbeServer> server = CreateObject<FlentUdpProbeServer>();
        server->SetAttribute("Port", UintegerValue(port));
        pingPeer.Get(0)->AddApplication(server);
        server->SetStartTime(Seconds(0.0));
        server->SetStopTime(Seconds(length + 1.0));
        udpServers.Add(server);

        Ptr<FlentUdpProbeClient> probe = CreateObject<FlentUdpProbeClient>();
        probe->SetAttribute("RemoteAddress", AddressValue(pingPeerIfs.GetAddress(1)));
        probe->SetAttribute("RemotePort", UintegerValue(port));
        probe->SetAttribute("Interval", TimeValue(MilliSeconds(50)));
        probe->SetAttribute("PacketSize", UintegerValue(100));
        client.Get(0)->AddApplication(probe);
        probe->SetStartTime(Seconds(0.5));
        probe->SetStopTime(Seconds(length));
        udpClients.push_back(probe);
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

    downSinks.Start(Seconds(0.0));
    downSinks.Stop(Seconds(length + 1.0));
    upSinks.Start(Seconds(0.0));
    upSinks.Stop(Seconds(length + 1.0));
    downSources.Start(Seconds(0.5));
    downSources.Stop(Seconds(length));
    upSources.Start(Seconds(0.5));
    upSources.Stop(Seconds(length));

    // --- Wire FlentCsvSink ---
    FlentCsvSink sink;
    sink.SetTestName("rrul");
    sink.SetStepSize(MilliSeconds(200));
    sink.SetLength(Seconds(length));
    sink.SetOutputDir(outDir);

    for (uint32_t i = 0; i < nFlows; ++i)
    {
        sink.AddTcpDownFlow(i, DynamicCast<PacketSink>(downSinks.Get(i)));
        sink.AddTcpUpFlow(i, DynamicCast<PacketSink>(upSinks.Get(i)));
        sink.AddUdpProbe(i, udpClients[i]);
    }
    if (pingApp)
    {
        sink.AddIcmpProbe(pingApp);
    }

    std::map<std::string, std::string> meta = {
        {"test_name", "rrul"},
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

    std::cout << "[cake-rrul] wrote bundle to " << outDir << std::endl;

    Simulator::Destroy();
    return 0;
}
