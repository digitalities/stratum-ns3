/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * EXTENSIVE smoke fixture for cake-host-fairness-sweep: asserts the
 * probe reproduces the (4, 1) CUBIC baseline share_A within a
 * single-replica band, with socket buffers sized above the path
 * bandwidth-delay product so the share is not receive-window-limited.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-module.h"
#include "ns3/test.h"
#include "ns3/traffic-control-module.h"

#include <vector>

namespace ns3::stratum
{

namespace
{
constexpr double kBandwidthBps = 100e6; // 100 Mbit/s
constexpr double kDurationSeconds = 30.0;
// The socket buffers are sized to the path bandwidth-delay product (below),
// so the (4, 1) byte-share is measured apples-to-apples with a transport that
// autotunes its receive window. At the default 128 KB socket buffers the lone
// host-B flow is receive-window-limited (its window pins ~4x below the
// 100 Mbit/s x 44 ms ~ 550 KB BDP) and the share is inflated to ~0.76; with a
// window that covers the BDP it settles at the buffer-adequate ~0.647.
constexpr double kShareABandLow = 0.625;
constexpr double kShareABandHigh = 0.665;
} // namespace

class CakeHostFairnessSmokeTestCase : public TestCase
{
  public:
    CakeHostFairnessSmokeTestCase()
        : TestCase("S-17.55 probe reproduces (4, 1) nested CUBIC baseline")
    {
    }

  private:
    void DoRun() override;
};

void
CakeHostFairnessSmokeTestCase::DoRun()
{
    RngSeedManager::SetRun(1);
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));

    // Size the socket buffers above the path BDP (100 Mbit/s x 44 ms ~ 550 KB)
    // so neither host is receive-window-limited; the default 128 KB caps the
    // window ~4x below the BDP and inflates the host-isolation byte-share.
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(4194304));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(4194304));

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
    ipv4.Assign(dA);
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    ipv4.Assign(dB);
    ipv4.SetBase("10.2.0.0", "255.255.255.0");
    ipv4.Assign(dBN);
    ipv4.SetBase("10.3.1.0", "255.255.255.0");
    Ipv4InterfaceContainer iSinkA = ipv4.Assign(dSinkA);
    ipv4.SetBase("10.3.2.0", "255.255.255.0");
    Ipv4InterfaceContainer iSinkB = ipv4.Assign(dSinkB);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    cake::Helper::SetAsCakeDiffserv4(edge,
                                     DataRate(kBandwidthBps),
                                     {.tinShaping = true, .hostIsolation = true});

    Ptr<TrafficControlLayer> tc = dBN.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
    NS_ASSERT_MSG(tc, "TrafficControlLayer must be installed on routerA");
    if (tc->GetRootQueueDiscOnDevice(dBN.Get(0)))
    {
        tc->DeleteRootQueueDiscOnDevice(dBN.Get(0));
    }
    tc->SetRootQueueDiscOnDevice(dBN.Get(0), edge);

    std::vector<Ptr<PacketSink>> sinkAppsA;
    std::vector<Ptr<PacketSink>> sinkAppsB;
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
        srcApp.Stop(Seconds(kDurationSeconds));

        PacketSinkHelper snk("ns3::TcpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer snkApp = snk.Install(sinkNode);
        snkApp.Start(Seconds(0.0));
        snkApp.Stop(Seconds(kDurationSeconds + 1.0));
        out.push_back(DynamicCast<PacketSink>(snkApp.Get(0)));
    };

    for (uint32_t i = 0; i < 4; ++i)
    {
        startBulkFlow(hosts.Get(0),
                      iSinkA.GetAddress(1),
                      static_cast<uint16_t>(9000 + i),
                      sinks.Get(0),
                      sinkAppsA);
    }
    startBulkFlow(hosts.Get(1), iSinkB.GetAddress(1), 10000, sinks.Get(1), sinkAppsB);

    Simulator::Stop(Seconds(kDurationSeconds + 1.0));
    Simulator::Run();

    double bytesA = 0;
    for (auto& s : sinkAppsA)
    {
        bytesA += static_cast<double>(s->GetTotalRx());
    }
    double bytesB = static_cast<double>(sinkAppsB[0]->GetTotalRx());

    NS_ASSERT_MSG(bytesA + bytesB > 0, "Both sinks reported zero bytes");
    double shareA = bytesA / (bytesA + bytesB);

    NS_TEST_ASSERT_MSG_GT(shareA,
                          kShareABandLow,
                          "share_A below single-replica baseline band: " << shareA);
    NS_TEST_ASSERT_MSG_LT(shareA,
                          kShareABandHigh,
                          "share_A above single-replica baseline band: " << shareA);

    Simulator::Destroy();
}

class CakeHostFairnessSmokeTestSuite : public TestSuite
{
  public:
    CakeHostFairnessSmokeTestSuite()
        : TestSuite("stratum-cake-host-fairness-smoke", Type::EXAMPLE)
    {
        AddTestCase(new CakeHostFairnessSmokeTestCase, TestCase::Duration::EXTENSIVE);
    }
};

static CakeHostFairnessSmokeTestSuite g_cakeHostFairnessSmokeTestSuite;

} // namespace ns3::stratum
