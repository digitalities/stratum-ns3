/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * QUICK smoke fixture for the CAKE host-fairness probe under UDP CBR
 * load. Asserts that with one unresponsive 50 Mbit/s flow per host across
 * a 100 Mbit/s bottleneck, the per-host share converges near the
 * host-fair midpoint (share_A in [0.45, 0.55]) and that the bottleneck
 * is approximately saturated.
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
constexpr double kDurationSeconds = 5.0;
constexpr double kShareALow = 0.45;
constexpr double kShareAHigh = 0.55;
constexpr double kMinTotalBytes = 50.0 * 1024 * 1024; // > 50 MB across both sinks
} // namespace

class CakeHostFairnessUdpSmokeTestCase : public TestCase
{
  public:
    CakeHostFairnessUdpSmokeTestCase()
        : TestCase("UDP CBR at (1, 1) yields near-host-fair share")
    {
    }

  private:
    void DoRun() override;
};

void
CakeHostFairnessUdpSmokeTestCase::DoRun()
{
    RngSeedManager::SetRun(1);

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
                                     /*enableAckFilter=*/false,
                                     /*enableLlq=*/false,
                                     /*enableTinShaping=*/true,
                                     /*enableHostIsolation=*/true,
                                     /*useInnerTbfShaping=*/false);

    Ptr<TrafficControlLayer> tc = dBN.Get(0)->GetNode()->GetObject<TrafficControlLayer>();
    NS_ASSERT_MSG(tc, "TrafficControlLayer must be installed on routerA");
    if (tc->GetRootQueueDiscOnDevice(dBN.Get(0)))
    {
        tc->DeleteRootQueueDiscOnDevice(dBN.Get(0));
    }
    tc->SetRootQueueDiscOnDevice(dBN.Get(0), edge);

    auto startUdpFlow = [&](Ptr<Node> srcNode,
                            Ipv4Address sinkAddr,
                            uint16_t port,
                            Ptr<Node> sinkNode,
                            std::vector<Ptr<PacketSink>>& out) {
        ns3::OnOffHelper src("ns3::UdpSocketFactory", InetSocketAddress(sinkAddr, port));
        src.SetAttribute("DataRate", DataRateValue(DataRate("50Mbps")));
        src.SetAttribute("PacketSize", UintegerValue(1400));
        src.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1.0]"));
        src.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0.0]"));
        ApplicationContainer srcApp = src.Install(srcNode);
        srcApp.Start(Seconds(0.1));
        srcApp.Stop(Seconds(kDurationSeconds));

        PacketSinkHelper snk("ns3::UdpSocketFactory",
                             InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer snkApp = snk.Install(sinkNode);
        snkApp.Start(Seconds(0.0));
        snkApp.Stop(Seconds(kDurationSeconds + 1.0));
        out.push_back(DynamicCast<PacketSink>(snkApp.Get(0)));
    };

    std::vector<Ptr<PacketSink>> sinkAppsA;
    std::vector<Ptr<PacketSink>> sinkAppsB;
    startUdpFlow(hosts.Get(0), iSinkA.GetAddress(1), 11000, sinks.Get(0), sinkAppsA);
    startUdpFlow(hosts.Get(1), iSinkB.GetAddress(1), 12000, sinks.Get(1), sinkAppsB);

    Simulator::Stop(Seconds(kDurationSeconds + 1.0));
    Simulator::Run();

    double bytesA = static_cast<double>(sinkAppsA[0]->GetTotalRx());
    double bytesB = static_cast<double>(sinkAppsB[0]->GetTotalRx());
    double total = bytesA + bytesB;

    NS_TEST_ASSERT_MSG_GT(total,
                          kMinTotalBytes,
                          "Total received bytes below saturation floor: " << total);

    double shareA = bytesA / total;
    NS_TEST_ASSERT_MSG_GT(shareA, kShareALow, "share_A below host-fair band: " << shareA);
    NS_TEST_ASSERT_MSG_LT(shareA, kShareAHigh, "share_A above host-fair band: " << shareA);

    Simulator::Destroy();
}

class CakeHostFairnessUdpSmokeTestSuite : public TestSuite
{
  public:
    CakeHostFairnessUdpSmokeTestSuite()
        : TestSuite("stratum-cake-host-fairness-udp-smoke", Type::EXAMPLE)
    {
        AddTestCase(new CakeHostFairnessUdpSmokeTestCase, TestCase::Duration::QUICK);
    }
};

static CakeHostFairnessUdpSmokeTestSuite g_cakeHostFairnessUdpSmokeTestSuite;

} // namespace ns3::stratum
