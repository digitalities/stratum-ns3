/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * EXTENSIVE fixture for cake-host-iso-jitter-floor: asserts that injecting
 * shared bottleneck rate-jitter (both directions) drives the (4, 1) CUBIC
 * host-isolation share from its deterministic baseline down to a host-fair
 * floor bracketing the matched-Linux value, and that per-host backlog
 * occupancy is far less asymmetric than the byte-share, so the deterministic
 * excess is a within-contention allocation effect rather than a per-host
 * occupancy asymmetry.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-cake-helper.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/test.h"
#include "ns3/traffic-control-module.h"

#include <vector>

namespace ns3::stratum
{

namespace
{
constexpr double kBandwidthBps = 100e6;
constexpr double kDurationSeconds = 30.0;

// Deterministic baseline band (matches the cake-host-fairness smoke fixture):
// the buffer-adequate (4 MB) host-isolation share sits between per-flow-fair
// (0.80) and per-host-fair (0.50).
constexpr double kBaselineLow = 0.625;
constexpr double kBaselineHigh = 0.665;
// Jitter-floor band over the mean of kReplicas rate-jitter replicas. Rate
// jitter drives the (4, 1) share down from the deterministic baseline (~0.647)
// toward host-fair, closing most of the gap to the matched-Linux offload-off
// value (0.533). A single replica of this chaotic CUBIC-over-jittered-
// bottleneck scenario is too noisy to pin portably -- the per-replica share
// spans roughly 0.555..0.588 here -- so the assertion is on the replica mean,
// whose seed-to-seed standard error is about 0.005. The measured 5-replica
// mean is 0.568; the band brackets it tightly (+/-0.025).
constexpr uint32_t kReplicas = 5;
constexpr double kFloorLow = 0.54;
constexpr double kFloorHigh = 0.59;

// Per-run scenario state (reset before each run).
Ptr<NetDevice> g_devTx;
Ptr<NetDevice> g_devRx;
Ptr<UniformRandomVariable> g_jitterRv;
double g_jitterPct = 0.0;
double g_jitterPeriodNs = 5e5;
Ipv4Address g_hostAAddr;
Ipv4Address g_hostBAddr;
int64_t g_qA = 0;
int64_t g_qB = 0;
double g_busyA = 0.0;
double g_busyB = 0.0;
uint32_t g_occSamples = 0;

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

/// Aggregate result of one scenario run.
struct ScenarioResult
{
    double shareA;   ///< many-flow-host byte share
    double occRatio; ///< per-host backlog-occupancy ratio (A/B)
    double occA;     ///< host A backlogged-time fraction
    double occB;     ///< host B backlogged-time fraction
};

/// Build and run the (4, 1) split-destination scenario once.
ScenarioResult
RunScenario(double jitterPct, uint32_t run)
{
    g_jitterPct = jitterPct;
    g_jitterPeriodNs = 5e5; // 0.5 ms
    g_qA = g_qB = 0;
    g_busyA = g_busyB = 0.0;
    g_occSamples = 0;

    RngSeedManager::SetRun(run);
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       TypeIdValue(TypeId::LookupByName("ns3::TcpCubic")));
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(4194304));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(4194304));
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(536));

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

    g_hostAAddr = iA.GetAddress(0);
    g_hostBAddr = iB.GetAddress(0);

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

    edge->TraceConnectWithoutContext("Enqueue", MakeCallback(&EnqCb));
    edge->TraceConnectWithoutContext("Dequeue", MakeCallback(&DeqCb));

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

    if (g_jitterPct > 0.0)
    {
        g_devTx = dBN.Get(0);
        g_devRx = dBN.Get(1);
        g_jitterRv = CreateObject<UniformRandomVariable>();
        Simulator::Schedule(Seconds(0.1), &JitterRate);
    }
    Simulator::Schedule(Seconds(5.0), &OccSample, 0.001);

    Simulator::Stop(Seconds(kDurationSeconds + 1.0));
    Simulator::Run();

    double bytesA = 0;
    for (auto& s : sinkAppsA)
    {
        bytesA += static_cast<double>(s->GetTotalRx());
    }
    double bytesB = static_cast<double>(sinkAppsB[0]->GetTotalRx());

    double sampled = g_occSamples * 0.001;
    ScenarioResult r;
    r.shareA = (bytesA + bytesB > 0) ? bytesA / (bytesA + bytesB) : 0.0;
    r.occA = (sampled > 0) ? g_busyA / sampled : 0.0;
    r.occB = (sampled > 0) ? g_busyB / sampled : 0.0;
    r.occRatio = (r.occB > 0) ? r.occA / r.occB : 0.0;

    Simulator::Destroy();
    return r;
}
} // namespace

/**
 * @ingroup tests
 * @brief Rate-jitter drives the host-isolation share to a host-fair floor;
 *        per-host occupancy is far less asymmetric than the byte-share.
 */
class CakeHostIsoJitterFloorTestCase : public TestCase
{
  public:
    CakeHostIsoJitterFloorTestCase()
        : TestCase("S-17.68/69 rate-jitter drives (4, 1) host-isolation share to a host-fair floor")
    {
    }

  private:
    void DoRun() override;
};

void
CakeHostIsoJitterFloorTestCase::DoRun()
{
    ScenarioResult det = RunScenario(/*jitterPct=*/0.0, /*run=*/1);

    // S-17.68: deterministic baseline reproduces the buffer-adequate share.
    NS_TEST_ASSERT_MSG_GT(det.shareA, kBaselineLow, "baseline share below band: " << det.shareA);
    NS_TEST_ASSERT_MSG_LT(det.shareA, kBaselineHigh, "baseline share above band: " << det.shareA);

    // S-17.68: rate-jitter drives the share down toward host-fair. A single
    // replica of this chaotic CUBIC-over-jittered-bottleneck scenario is too
    // noisy to pin portably, so average kReplicas independent replicas and
    // assert on the mean, which brackets the matched-Linux offload-off value
    // (0.533) and is stable across hosts.
    double jitShareSum = 0.0;
    for (uint32_t run = 1; run <= kReplicas; ++run)
    {
        jitShareSum += RunScenario(/*jitterPct=*/0.5, run).shareA;
    }
    const double jitShareMean = jitShareSum / kReplicas;

    NS_TEST_ASSERT_MSG_GT(jitShareMean,
                          kFloorLow,
                          "jitter-floor mean below band: " << jitShareMean);
    NS_TEST_ASSERT_MSG_LT(jitShareMean,
                          kFloorHigh,
                          "jitter-floor mean above band: " << jitShareMean);
    NS_TEST_ASSERT_MSG_LT(jitShareMean,
                          det.shareA,
                          "jitter must reduce the mean share below the deterministic baseline");

    // S-17.69: the deterministic share excess is not a per-host occupancy
    // asymmetry -- both hosts are backlogged a substantial fraction, and the
    // occupancy ratio is far below the byte-share ratio (~1.83 at 0.647).
    NS_TEST_ASSERT_MSG_GT(det.occA, 0.30, "host A occupancy implausibly low: " << det.occA);
    NS_TEST_ASSERT_MSG_GT(det.occB, 0.30, "host B occupancy implausibly low: " << det.occB);
    NS_TEST_ASSERT_MSG_LT(det.occRatio,
                          1.30,
                          "occupancy ratio not far below the byte-share ratio: " << det.occRatio);
}

/**
 * @ingroup tests
 * @brief Test suite for the cake-host-iso-jitter-floor example.
 */
class CakeHostIsoJitterFloorTestSuite : public TestSuite
{
  public:
    CakeHostIsoJitterFloorTestSuite()
        : TestSuite("stratum-cake-host-iso-jitter-floor", Type::EXAMPLE)
    {
        AddTestCase(new CakeHostIsoJitterFloorTestCase, TestCase::Duration::EXTENSIVE);
    }
};

static CakeHostIsoJitterFloorTestSuite g_cakeHostIsoJitterFloorTestSuite;

} // namespace ns3::stratum
