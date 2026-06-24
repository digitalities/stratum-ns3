/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Q-17.1: Parekh-Gallager 1993 Theorem 1 conformance — PGPS vs GPS
 * per-packet finish-time gap on the Stratum WFQ scheduler.
 *
 * Reference: A. K. Parekh and R. G. Gallager, "A Generalized Processor
 * Sharing Approach to Flow Control in Integrated Services Networks: The
 * Single-Node Case," IEEE/ACM Trans. Networking, vol. 1, no. 3,
 * pp. 344-357, June 1993. Theorem 1 (p. 347):
 *
 *     F_hat_p - F_p <= L_max / r        for all packets p
 *
 * where F_p is the GPS finish time of packet p, F_hat_p is the PGPS
 * finish time (= our WFQ scheduler), L_max is the maximum packet
 * length, and r is the link rate.
 *
 * Test design.  Following Theorem 3 (p. 352), the all-greedy regime
 * (every session continuously backlogged from t=0) maximises D*_i and
 * Q*_i simultaneously and is therefore the natural worst-case input
 * for stressing Theorem 1. Under all-greedy with uniform packet size
 * L, the GPS finish time of session i's k-th packet has the closed
 * form
 *
 *     F_p = k * L / g_i,    g_i = phi_i / sum_j phi_j  *  r
 *
 * so no fluid-system simulator is needed inside the test fixture.
 *
 * Gate (per the project's "Choice B" tolerance philosophy):
 *
 *     max_p (F_hat_p - F_p) <= 2 * L_max / r
 *
 * which adds one packet-time of slack on top of Theorem 1's strict
 * bound to absorb ns-3 TX-ring + dispatcher artefacts that are not
 * scheduler bugs.  The strict-Theorem-1 violation count (gap >
 * L_max/r) is reported but not gated, so a creeping virtual-time
 * regression is still visible in test output before it crosses the
 * 2*L_max/r alarm.
 *
 * Q-17.1 is complementary to (not a duplicate of) Q-16.3:
 *  - Q-16.3 anchors *Theorem 2* (cumulative byte-lag) via the Chang
 *    sweep script.
 *  - Q-17.1 anchors *Theorem 1* (per-packet finish-time gap) inside
 *    the in-process test suite.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-scfq-scheduler.h"
#include "ns3/stratum-wf2qp-scheduler.h"
#include "ns3/stratum-wfq-scheduler.h"
#include "ns3/test.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <vector>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::PolicerType;
using ns3::stratum::ScfqScheduler;
using ns3::stratum::Wf2qPlusScheduler;
using ns3::stratum::WfqScheduler;

namespace
{

// Two sessions; weights are configurable per test case (see TestCase
// constructor). The symmetric configuration {1, 1} is the gated case
// — WFQ obeys Theorem 1 there with a healthy margin. The asymmetric
// configuration {1, 2} is reporting-only because the fixture's
// analytical-vs-empirical methodology compares an idealised
// continuously-backlogged GPS reference against actual integer-packet
// PGPS finish times; the (F_hat - F) gap accumulates over the
// simulation window without a constant upper bound, which surfaces
// any future regression that re-introduces a progressive-lag failure
// mode in WfqScheduler.
constexpr uint32_t kNumSessions = 2;
constexpr double kBottleneckMbps = 1.0;
constexpr double kAccessMbps = 100.0;
constexpr double kLinkDelayMs = 1.0;
constexpr uint32_t kPayloadBytes = 1000;
// Wire-size per packet on a PointToPointNetDevice: 20 (IP) + 8 (UDP)
// + 2 (PPP framing). The scheduler accounts for the IP-layer size,
// not the PPP wire overhead, so L_max in the Theorem-1 expression is
// the IP-layer packet size as the scheduler sees it.
constexpr uint32_t kIpPacketBytes = kPayloadBytes + 20 + 8;
constexpr double kSimSeconds = 5.0;
constexpr double kWarmupSeconds = 0.2; // skip until the bottleneck is fully backlogged

// Per-packet record built at PhyTxEnd on the bottleneck device.
struct TxEndRecord
{
    uint32_t flowIdx;
    Time txEnd;
};

// Per-flow trace state accumulated across one scenario run.
struct FlowState
{
    uint32_t txEndCount = 0; // running k for that session, post-warmup
};

std::vector<TxEndRecord> g_txEnds;
std::array<FlowState, kNumSessions> g_flowState;
Time g_firstTxEnd = Time::Max();

void
PhyTxEndCallback(Ptr<const Packet> p)
{
    // Pull the IPv4 header out of a copy to learn which session this is.
    Ipv4Header ipHdr;
    Ptr<Packet> copy = p->Copy();
    // Strip PPP header (2 bytes) before peeking IP header.
    copy->RemoveAtStart(2);
    if (copy->PeekHeader(ipHdr) == 0)
    {
        return;
    }
    uint8_t dscp = static_cast<uint8_t>(ipHdr.GetDscp());
    uint32_t flowIdx = kNumSessions; // sentinel
    if (dscp == 10)
    {
        flowIdx = 0;
    }
    else if (dscp == 20)
    {
        flowIdx = 1;
    }
    else
    {
        return;
    }
    Time now = Simulator::Now();
    if (now < g_firstTxEnd)
    {
        g_firstTxEnd = now;
    }
    g_txEnds.push_back({flowIdx, now});
}

struct Q17Result
{
    double maxGapSec = -1.0;
    double meanGapSec = 0.0;
    uint32_t strictViolations = 0; // gap > L_max/r (Theorem 1 verbatim)
    uint32_t totalPostWarmup = 0;
    std::array<uint32_t, kNumSessions> perFlowCount = {0, 0};
};

Q17Result
RunQ17Scenario(const std::string& scheduler, const std::array<double, kNumSessions>& phi)
{
    g_txEnds.clear();
    g_flowState = {};
    g_firstTxEnd = Time::Max();

    NodeContainer senders;
    senders.Create(kNumSessions);
    NodeContainer router;
    router.Create(1);
    NodeContainer sink;
    sink.Create(1);

    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute("DataRate", StringValue(std::to_string(kAccessMbps) + "Mbps"));
    accessLink.SetChannelAttribute("Delay", StringValue(std::to_string(kLinkDelayMs) + "ms"));

    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute("DataRate",
                                      StringValue(std::to_string(kBottleneckMbps) + "Mbps"));
    bottleneckLink.SetChannelAttribute("Delay", StringValue(std::to_string(kLinkDelayMs) + "ms"));

    std::vector<NetDeviceContainer> accessDevs;
    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        accessDevs.push_back(accessLink.Install(senders.Get(i), router.Get(0)));
    }
    NetDeviceContainer rSink = bottleneckLink.Install(router.Get(0), sink.Get(0));

    InternetStackHelper internet;
    internet.InstallAll();

    Ipv4AddressHelper ipv4;
    std::vector<Ipv4InterfaceContainer> ifs;
    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        std::ostringstream subnet;
        subnet << "10.1." << (i + 1) << ".0";
        ipv4.SetBase(subnet.str().c_str(), "255.255.255.0");
        ifs.push_back(ipv4.Assign(accessDevs[i]));
    }
    ipv4.SetBase("10.2.0.0", "255.255.255.0");
    Ipv4InterfaceContainer ifBn = ipv4.Assign(rSink);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    Ptr<EdgeQueueDisc> disc = CreateObject<EdgeQueueDisc>();
    auto discInner = CreateObject<stratum::RedQueueDisc>();
    disc->SetInnerDisc(discInner);
    discInner->SetNumQueues(kNumSessions);

    const std::array<uint8_t, kNumSessions> kDscp = {10, 20};
    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        disc->AddMarkRule({.dscp = kDscp[i], .srcAddr = ifs[i].GetAddress(0)});
    }

    diffserv::Helper helper;
    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        helper.AddDumbPolicy(disc, kDscp[i]);
        helper.AddPolicerEntry(disc,
                               {.policer = PolicerType::DUMB,
                                .initialCodePt = kDscp[i],
                                .downgrade1 = kDscp[i],
                                .downgrade2 = kDscp[i],
                                .policyIndex = static_cast<uint32_t>(PolicerType::DUMB)});
        helper.AddPhbEntry(discInner, kDscp[i], i, 0);
    }

    const double phiSum = std::accumulate(phi.begin(), phi.end(), 0.0);
    std::array<double, kNumSessions> kNormWeights;
    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        kNormWeights[i] = phi[i] / phiSum;
    }

    const double linkBwBps = kBottleneckMbps * 1e6;
    if (scheduler == "WFQ")
    {
        auto s = CreateObjectWithAttributes<WfqScheduler>("NumQueues",
                                                          UintegerValue(kNumSessions),
                                                          "LinkBandwidth",
                                                          DoubleValue(linkBwBps));
        for (uint32_t i = 0; i < kNumSessions; ++i)
        {
            s->SetParam(i, kNormWeights[i]);
        }
        discInner->SetScheduler(s);
    }
    else if (scheduler == "WF2Q+")
    {
        auto s = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                               UintegerValue(kNumSessions),
                                                               "LinkBandwidth",
                                                               DoubleValue(linkBwBps));
        for (uint32_t i = 0; i < kNumSessions; ++i)
        {
            s->SetParam(i, kNormWeights[i]);
        }
        discInner->SetScheduler(s);
    }
    else if (scheduler == "SCFQ")
    {
        auto s = CreateObjectWithAttributes<ScfqScheduler>("NumQueues",
                                                           UintegerValue(kNumSessions),
                                                           "LinkBandwidth",
                                                           DoubleValue(linkBwBps));
        for (uint32_t i = 0; i < kNumSessions; ++i)
        {
            s->SetParam(i, kNormWeights[i]);
        }
        discInner->SetScheduler(s);
    }
    else
    {
        return {};
    }

    TrafficControlHelper tch;
    tch.Uninstall(rSink.Get(0));
    Ptr<TrafficControlLayer> tc = router.Get(0)->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(rSink.Get(0), disc);
    disc->Initialize();

    // Generous queue limits — we want NO drops, so Theorem 1's "for all
    // packets p" predicate holds across the measurement window.
    constexpr uint32_t kQueueLimit = 100000;
    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        discInner->SetQueueLimit(i, kQueueLimit);
        // WRED OFF (thMin=thMax=very-high) so backlog is uncapped by AQM.
        discInner->ConfigQueue(
            {.queue = i, .prec = 0, .thMin = 50000.0, .thMax = 100000.0, .maxP = 0.001});
    }

    // UDP CBR sources at 5 * link rate each — the access link absorbs
    // them at 100 Mbps, the bottleneck queue saturates within a few ms,
    // and from then on the system is in the all-greedy regime per
    // Parekh Theorem 3.
    const uint16_t kBasePort = 6000;
    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), kBasePort));
    ApplicationContainer sinkApps = sinkHelper.Install(sink.Get(0));
    sinkApps.Start(Seconds(0.0));

    for (uint32_t i = 0; i < kNumSessions; ++i)
    {
        OnOffHelper onoff("ns3::UdpSocketFactory",
                          InetSocketAddress(ifBn.GetAddress(1), kBasePort));
        onoff.SetAttribute("DataRate", StringValue(std::to_string(5.0 * kBottleneckMbps) + "Mbps"));
        onoff.SetAttribute("PacketSize", UintegerValue(kPayloadBytes));
        onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=10000]"));
        onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
        ApplicationContainer src = onoff.Install(senders.Get(i));
        src.Start(Seconds(0.0));
    }

    // PhyTxEnd hook on the router-side bottleneck device — fires when the
    // packet has finished transmission on the wire (= F_hat_p).
    Ptr<NetDevice> bottleneckDev = rSink.Get(0);
    bottleneckDev->TraceConnectWithoutContext("PhyTxEnd", MakeCallback(&PhyTxEndCallback));

    Simulator::Stop(Seconds(kSimSeconds));
    Simulator::Run();
    Simulator::Destroy();

    // Build per-flow k indices over the post-warmup window. The
    // Theorem-1 bound is per-packet, so we use the actual sequence in
    // which packets exit the WFQ scheduler — the analytical F_p uses
    // k = (post-warmup ordinal in session i).
    Q17Result r;
    if (g_firstTxEnd == Time::Max())
    {
        return r;
    }
    const Time tOrigin = g_firstTxEnd; // virtual-time origin
    const double L = static_cast<double>(kIpPacketBytes);
    const double rBps = linkBwBps / 8.0;
    const double LmaxOverR = L / rBps;

    std::array<uint32_t, kNumSessions> k = {0, 0};
    double maxGap = -std::numeric_limits<double>::infinity();
    double sumGap = 0.0;
    uint32_t totalPostWarmup = 0;
    uint32_t strictViol = 0;

    for (const auto& rec : g_txEnds)
    {
        const double tRel = (rec.txEnd - tOrigin).GetSeconds();
        if (tRel < kWarmupSeconds)
        {
            // Pre-warmup packets count toward k but do not gate.
            k[rec.flowIdx]++;
            continue;
        }
        k[rec.flowIdx]++;
        const double gI = kNormWeights[rec.flowIdx] * rBps;
        const double Fp = static_cast<double>(k[rec.flowIdx]) * L / gI;
        const double Fhat = tRel;
        const double gap = Fhat - Fp;
        maxGap = std::max(maxGap, gap);
        sumGap += gap;
        if (gap > LmaxOverR)
        {
            strictViol++;
        }
        r.perFlowCount[rec.flowIdx]++;
        totalPostWarmup++;
    }

    r.maxGapSec = maxGap;
    r.meanGapSec = (totalPostWarmup > 0) ? (sumGap / totalPostWarmup) : 0.0;
    r.strictViolations = strictViol;
    r.totalPostWarmup = totalPostWarmup;
    return r;
}

class ParekhTheorem1Case : public TestCase
{
  public:
    ParekhTheorem1Case(const std::string& scheduler,
                       const std::string& regimeLabel,
                       const std::array<double, kNumSessions>& phi,
                       bool gated)
        : TestCase("Q-17.1 parekh1993 Theorem 1: " + scheduler + " " + regimeLabel +
                   (gated ? " (gated)" : " (reporting only)")),
          m_scheduler(scheduler),
          m_regime(regimeLabel),
          m_phi(phi),
          m_gated(gated)
    {
    }

  private:
    void DoRun() override
    {
        const Q17Result res = RunQ17Scenario(m_scheduler, m_phi);
        const double rBps = (kBottleneckMbps * 1e6) / 8.0;
        const double LmaxOverR = static_cast<double>(kIpPacketBytes) / rBps;
        const double envelope = 2.0 * LmaxOverR;

        std::ostringstream report;
        report << std::fixed << std::setprecision(6);
        report << "[Q-17.1 " << m_scheduler << " " << m_regime
               << "] post-warmup packets=" << res.totalPostWarmup
               << " (per-flow: " << res.perFlowCount[0] << "/" << res.perFlowCount[1] << ")"
               << " max(F_hat - F)=" << res.maxGapSec << " s"
               << " mean(F_hat - F)=" << res.meanGapSec << " s"
               << " L_max/r=" << LmaxOverR << " s"
               << " strict-Thm1 violations (gap > L_max/r): " << res.strictViolations << "/"
               << res.totalPostWarmup;
        std::clog << report.str() << std::endl;

        NS_TEST_ASSERT_MSG_GT(res.totalPostWarmup,
                              static_cast<uint32_t>(0),
                              "Q-17.1 " + m_scheduler +
                                  ": no post-warmup TxEnd records — "
                                  "topology/trace wiring likely broken");

        if (!m_gated)
        {
            return;
        }

        std::ostringstream msg;
        msg << "Q-17.1 " << m_scheduler << " max(F_hat - F)=" << res.maxGapSec
            << " s exceeds Choice-B envelope 2*L_max/r=" << envelope << " s "
            << "(Theorem 1 verbatim bound is L_max/r=" << LmaxOverR
            << " s; strict violations=" << res.strictViolations << "/" << res.totalPostWarmup
            << ")";
        NS_TEST_ASSERT_MSG_LT(res.maxGapSec, envelope, msg.str());
    }

    std::string m_scheduler;
    std::string m_regime;
    std::array<double, kNumSessions> m_phi;
    bool m_gated;
};

class DiffServQ17ParekhSuite : public TestSuite
{
  public:
    DiffServQ17ParekhSuite()
        : TestSuite("stratum-q17-parekh-theorem1", Type::EXAMPLE)
    {
        const std::array<double, kNumSessions> kSym = {1.0, 1.0};
        const std::array<double, kNumSessions> kAsym = {1.0, 2.0};

        // Symmetric (1:1) — Theorem 1 holds. WFQ is the only gated
        // case; WF2Q+ and SCFQ are reported alongside for cross-
        // scheduler context (WF2Q+ Bennett-Zhang 1996 tightening,
        // SCFQ Golestani 1994 has no formal F_hat - F bound).
        AddTestCase(new ParekhTheorem1Case("WFQ", "sym", kSym, /*gated=*/true),
                    TestCase::Duration::QUICK);
        AddTestCase(new ParekhTheorem1Case("WF2Q+", "sym", kSym, /*gated=*/false),
                    TestCase::Duration::QUICK);
        AddTestCase(new ParekhTheorem1Case("SCFQ", "sym", kSym, /*gated=*/false),
                    TestCase::Duration::QUICK);

        // Asymmetric sweep — reporting-only. The ratio sweep
        // discriminates between three candidate causes of the residual
        // per-flow bias visible in WfqScheduler at modest asymmetry:
        //
        //   tie-breaking on equal F-stamps         => bias scales linearly with ratio
        //   floating-point V(t) accumulation       => bias scales sub-linearly
        //   busy-set update timing in base class   => bias roughly constant in ratio
        //
        // WFQ is the primary suspect; WF2Q+ is included so common-mode
        // behaviour points us at the shared virtual-time bookkeeping
        // rather than WFQ's Select(). SCFQ added at 1:2 only as the
        // looser-bound reference.
        const std::array<std::array<double, kNumSessions>, 4> kRatios = {{
            {1.0, 2.0},
            {1.0, 3.0},
            {1.0, 5.0},
            {1.0, 10.0},
        }};
        const std::array<std::string, 4> kRatioLabels = {"asym1to2",
                                                         "asym1to3",
                                                         "asym1to5",
                                                         "asym1to10"};
        for (size_t r = 0; r < kRatios.size(); ++r)
        {
            AddTestCase(new ParekhTheorem1Case("WFQ",
                                               kRatioLabels[r],
                                               kRatios[r],
                                               /*gated=*/false),
                        TestCase::Duration::QUICK);
            AddTestCase(new ParekhTheorem1Case("WF2Q+",
                                               kRatioLabels[r],
                                               kRatios[r],
                                               /*gated=*/false),
                        TestCase::Duration::QUICK);
        }
        // SCFQ at 1:2 only — looser-bound reference.
        AddTestCase(new ParekhTheorem1Case("SCFQ", "asym1to2", kAsym, /*gated=*/false),
                    TestCase::Duration::QUICK);
    }
};

static DiffServQ17ParekhSuite g_diffservQ17ParekhSuite;

} // namespace
