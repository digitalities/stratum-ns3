/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * chang-comparison.cc — Reproduce the validation scenario from:
 *
 *   R. Chang, M. Rahimi, V. Pournaghshband, "Differentiated Service
 *   Queuing Disciplines in NS-3," Proc. SIMULTECH 2015, Split-Dubrovnik.
 *
 * PURPOSE
 * -------
 * Chang et al. validated their ns-3 DiffServ scheduling modules (SPQ, WFQ,
 * WRR) by showing that the perceived bandwidth ratio between two competing
 * TCP flows converges to the configured weight ratio.  This example
 * reproduces their exact scenario using the Stratum module, enabling
 * a direct apples-to-apples comparison.
 *
 * SCENARIO (from Chang et al. Section V)
 * ---------------------------------------
 *
 *   Sender 0 ──[T Mbps, 5ms]──┐
 *                               ├── Router ──[0.5T Mbps, 5ms]── Receiver
 *   Sender 1 ──[T Mbps, 5ms]──┘
 *
 *   - Two TCP BulkSend flows, each sending unlimited data at 1000-byte
 *     segments.  Both traverse the same bottleneck link at half the
 *     sender access rate (0.5T), creating sustained congestion.
 *   - The DiffServ edge queue disc on the bottleneck classifies flows
 *     by source IP address and schedules them with configurable weights.
 *   - Metering is set to pass-through (Dumb policy); no RED drops (very
 *     large queue limits and thresholds).
 *   - The perceived bandwidth ratio R0/R1 is sampled periodically and
 *     written to ratio.tr.  The steady-state mean (second half of sim)
 *     is reported as the final result.
 *
 * PARAMETERS
 * ----------
 *   --scheduler   WFQ or WRR (default: WFQ)
 *   --dataRate    Sender data rate T in Mbps (default: 10)
 *   --weightRatio Target weight ratio w1/w2 (default: 2)
 *   --outputDir   Output directory (default: output/chang-comparison)
 *
 * VALIDATION MATRIX (matching Chang et al.)
 * -----------------------------------------
 *   Data rates T:      0.5, 1, 10, 50 Mbps
 *   Weight ratios:     1, 2, 7, 10
 *   Schedulers:        WFQ, WRR
 *   Expected result:   perceived ratio ≈ weight ratio (GPS prediction)
 *
 * HOW TO RUN
 * ----------
 *   # Single run:
 *   ./ns3 run "chang-comparison --scheduler=WRR --dataRate=10 --weightRatio=7"
 *
 *   # Full matrix (16 runs per scheduler):
 *   for S in WFQ WRR; do
 *     for T in 0.5 1 10 50; do
 *       for R in 1 2 7 10; do
 *         ./ns3 run "chang-comparison --scheduler=$S --dataRate=$T
 * --weightRatio=$R" done done done
 *
 * OUTPUT FILES (per run, in outputDir/{SCHED}-T{rate}-R{ratio}/)
 * ---------------------------------------------------------------
 *   ratio.tr    Time-series: <time_seconds> <perceived_ratio_R0/R1>
 *   summary.txt Key-value summary: weights, throughput, ratio, error
 *
 * INTERPRETING RESULTS
 * --------------------
 * The perceived ratio should converge to the weight ratio over time.
 * At low data rates (T=0.5), convergence is slow and noisy because TCP's
 * congestion window dynamics dominate.  At high data rates (T=50),
 * convergence is faster.  WRR produces cleaner convergence than WFQ for
 * uniform packet sizes because WRR does not require virtual-time
 * bookkeeping.  At very high weight ratios (R=7, 10), TCP's AIMD
 * mechanism gives the low-weight flow a minimum throughput floor,
 * preventing perfect convergence — this is a property of TCP, not the
 * scheduler.
 *
 * COMPARISON WITH CHANG ET AL.
 * ----------------------------
 * Their code was not released.  Their Figures 6-13 show time-series of
 * the perceived ratio at each (T, R) combination.  Our summary.txt
 * reports the steady-state mean, which should match the plateau visible
 * in their plots.  Direct comparison is most meaningful for WRR at
 * T=10 Mbps (their Figures 12-13).
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-scfq-scheduler.h"
#include "ns3/stratum-scheduler-registry.h"
#include "ns3/stratum-sfq-scheduler.h"
#include "ns3/stratum-wf2qp-scheduler.h"
#include "ns3/stratum-wfq-scheduler.h"
#include "ns3/stratum-wrr-scheduler.h"
#include "ns3/traffic-control-module.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::kAnyHost;
using ns3::stratum::kAnyProtocol;
using ns3::stratum::MarkRule;
using ns3::stratum::PolicerType;
using ns3::stratum::SchedulerArgs;
using ns3::stratum::SchedulerRegistry;

NS_LOG_COMPONENT_DEFINE("ChangComparison");

// Track bytes received per flow
static uint64_t g_rxBytes[2] = {0, 0};
static std::ofstream g_ratioFile;
static uint64_t g_lastBytes[2] = {0, 0};

void
RxCallback(uint32_t flowIdx, Ptr<const Packet> pkt, const Address&)
{
    g_rxBytes[flowIdx] += pkt->GetSize();
}

void
SampleRatio(double interval, double weightRatio)
{
    double now = Simulator::Now().GetSeconds();
    uint64_t delta0 = g_rxBytes[0] - g_lastBytes[0];
    uint64_t delta1 = g_rxBytes[1] - g_lastBytes[1];
    g_lastBytes[0] = g_rxBytes[0];
    g_lastBytes[1] = g_rxBytes[1];

    double ratio = 0.0;
    if (delta1 > 0)
    {
        ratio = static_cast<double>(delta0) / static_cast<double>(delta1);
    }

    g_ratioFile << std::fixed << std::setprecision(3) << now << " " << ratio << "\n";

    Simulator::Schedule(Seconds(interval), &SampleRatio, interval, weightRatio);
}

int
main(int argc, char* argv[])
{
    // Defaults matching Chang et al.
    std::string scheduler = "WFQ";
    double dataRateMbps = 10.0; // T in paper
    double weightRatio = 2.0;   // w1/w2
    std::string outputDir = "output/chang-comparison";
    bool useUdp = false;          // bypass TCP feedback for scheduler-only debugging
    double simTimeOverride = 0.0; // 0 = auto-pick from dataRate (default heuristic)
    double linkDelayMs = 5.0;     // one-way link delay (Chang's published default)

    CommandLine cmd;
    cmd.AddValue("scheduler", "WFQ, WRR, WF2Q+, SCFQ, SFQ", scheduler);
    cmd.AddValue("dataRate", "Sender data rate T in Mbps", dataRateMbps);
    cmd.AddValue("weightRatio", "Weight ratio w1/w2", weightRatio);
    cmd.AddValue("outputDir", "Output directory", outputDir);
    cmd.AddValue("udp", "Use UDP CBR instead of TCP BulkSend", useUdp);
    cmd.AddValue("simTime",
                 "Override simulation duration in seconds (0 = auto-pick from dataRate)",
                 simTimeOverride);
    cmd.AddValue("linkDelay",
                 "One-way link delay in milliseconds (default 5 ms = Chang published)",
                 linkDelayMs);
    cmd.Parse(argc, argv);

    // Guard against degenerate weightRatio values that would poison downstream
    // math: weightRatio = -1 would divby0 below; weightRatio = 0 would divby0
    // in the error_pct summary print. Ratios must be strictly positive for the
    // two-flow weighted scheduler scenario to make physical sense.
    NS_ABORT_MSG_IF(weightRatio <= 0.0,
                    "chang-comparison: --weightRatio must be > 0 (got " << weightRatio << ")");

    // Compute weights: w1/w2 = weightRatio, w1 + w2 = 20 (arbitrary sum)
    double w2 = 20.0 / (weightRatio + 1.0);
    double w1 = 20.0 - w2;

    // Bottleneck = 0.5T
    double bottleneckMbps = dataRateMbps * 0.5;

    // Create output directory
    std::string tag = scheduler + "-T" + std::to_string(static_cast<int>(dataRateMbps * 1000)) +
                      "-R" + std::to_string(static_cast<int>(weightRatio));
    std::string runDir = outputDir + "/" + tag;
    diffserv::EnsureDir(runDir);

    // ---- Topology: S0, S1 --> Router --> R0, R1 ----
    //   S0 --[T Mbps, 5ms]--> Router --[0.5T Mbps, 5ms]--> Receiver
    //   S1 --[T Mbps, 5ms]--/                          \--> (same receiver node)

    NodeContainer senders;
    NodeContainer router;
    NodeContainer receiver;
    senders.Create(2);
    router.Create(1);
    receiver.Create(1);

    InternetStackHelper internet;
    internet.Install(senders);
    internet.Install(router);
    internet.Install(receiver);

    // Access links: sender -> router (high bandwidth, no bottleneck)
    PointToPointHelper accessLink;
    accessLink.SetDeviceAttribute(
        "DataRate",
        DataRateValue(DataRate(static_cast<uint64_t>(dataRateMbps * 1e6))));
    accessLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(linkDelayMs)));

    NetDeviceContainer s0r = accessLink.Install(senders.Get(0), router.Get(0));
    NetDeviceContainer s1r = accessLink.Install(senders.Get(1), router.Get(0));

    // Bottleneck link: router -> receiver
    PointToPointHelper bottleneckLink;
    bottleneckLink.SetDeviceAttribute(
        "DataRate",
        DataRateValue(DataRate(static_cast<uint64_t>(bottleneckMbps * 1e6))));
    bottleneckLink.SetChannelAttribute("Delay", TimeValue(MilliSeconds(linkDelayMs)));
    // Minimise device queue to 1 packet
    bottleneckLink.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue("1p"));

    NetDeviceContainer rRecv = bottleneckLink.Install(router.Get(0), receiver.Get(0));

    // IP addresses
    Ipv4AddressHelper addr;
    addr.SetBase("10.0.1.0", "255.255.255.0");
    Ipv4InterfaceContainer if0 = addr.Assign(s0r);

    addr.SetBase("10.0.2.0", "255.255.255.0");
    Ipv4InterfaceContainer if1 = addr.Assign(s1r);

    addr.SetBase("10.0.3.0", "255.255.255.0");
    Ipv4InterfaceContainer ifBn = addr.Assign(rRecv);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // ---- DiffServ edge disc on bottleneck (router -> receiver) ----
    // Use edge disc with mark rules for source-IP classification
    Ptr<EdgeQueueDisc> disc = CreateObject<EdgeQueueDisc>();
    auto discInner = CreateObject<stratum::RedQueueDisc>();
    disc->SetInnerDisc(discInner);
    discInner->SetNumQueues(2);

    // Mark rules: classify by source IP
    //   Flow 0 from 10.0.1.1 -> DSCP 10
    //   Flow 1 from 10.0.2.1 -> DSCP 20
    MarkRule rule0;
    rule0.dscp = 10;
    rule0.srcAddr = static_cast<int32_t>(Ipv4Address("10.0.1.1").Get());
    rule0.dstAddr = kAnyHost;
    rule0.protocol = kAnyProtocol;
    disc->AddMarkRule(rule0);

    MarkRule rule1;
    rule1.dscp = 20;
    rule1.srcAddr = static_cast<int32_t>(Ipv4Address("10.0.2.1").Get());
    rule1.dstAddr = kAnyHost;
    rule1.protocol = kAnyProtocol;
    disc->AddMarkRule(rule1);

    // Dumb metering (no rate limiting, passthrough)
    diffserv::Helper helper;
    helper.AddDumbPolicy(disc, 10);
    helper.AddDumbPolicy(disc, 20);
    helper.AddPolicerEntry(disc, PolicerType::DUMB, 10, 10, 10);
    helper.AddPolicerEntry(disc, PolicerType::DUMB, 20, 20, 20);

    // PHB: DSCP 10 -> queue 0, DSCP 20 -> queue 1
    helper.AddPhbEntry(discInner, 10, 0, 0);
    helper.AddPhbEntry(discInner, 20, 1, 0);

    // Scheduler — dispatched via SchedulerRegistry. The CLI accepts
    // both the legacy uppercase forms ("WFQ", "WF2Q+", ...) and the
    // canonical lowercase tags ("wfq", "wf2qp", ...). We case-fold and
    // map the "+" variant to its canonical alias before lookup.
    std::string canonicalScheduler = scheduler;
    std::transform(canonicalScheduler.begin(),
                   canonicalScheduler.end(),
                   canonicalScheduler.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (canonicalScheduler == "wf2q+")
    {
        canonicalScheduler = "wf2qp";
    }

    SchedulerArgs schedArgs;
    schedArgs.numQueues = 2;
    schedArgs.linkBps = bottleneckMbps * 1e6;
    schedArgs.weights = {static_cast<double>(w1), static_cast<double>(w2)};

    if (!SchedulerRegistry::Get().Find(canonicalScheduler))
    {
        std::cerr << "Unknown scheduler: " << scheduler
                  << " (valid: wfq, wrr, wf2qp, scfq, sfq — uppercase legacy names accepted)\n";
        return 1;
    }
    auto sched = SchedulerRegistry::Get().Construct(canonicalScheduler, schedArgs);
    discInner->SetScheduler(sched);

    // Install on the router's outgoing device toward receiver
    TrafficControlHelper tch;
    tch.Uninstall(rRecv.Get(0)); // remove default
    Ptr<NetDevice> routerDev = rRecv.Get(0);
    Ptr<TrafficControlLayer> tc = router.Get(0)->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(routerDev, disc);
    disc->Initialize();

    // Large queue limits (paper: "practically unbounded")
    discInner->SetQueueLimit(0, 10000);
    discInner->SetQueueLimit(1, 10000);
    // Disable RED: set thresholds very high
    discInner->ConfigQueue(0, 0, 50000.0, 100000.0, 0.1);
    discInner->ConfigQueue(1, 0, 50000.0, 100000.0, 0.1);

    // ---- Applications ----
    uint16_t port0 = 5000;
    uint16_t port1 = 5001;

    // Receivers (PacketSink)
    const std::string sockFactory = useUdp ? "ns3::UdpSocketFactory" : "ns3::TcpSocketFactory";
    PacketSinkHelper sink0(sockFactory, InetSocketAddress(Ipv4Address::GetAny(), port0));
    PacketSinkHelper sink1(sockFactory, InetSocketAddress(Ipv4Address::GetAny(), port1));
    ApplicationContainer sinkApps0 = sink0.Install(receiver.Get(0));
    ApplicationContainer sinkApps1 = sink1.Install(receiver.Get(0));
    sinkApps0.Start(Seconds(0.0));
    sinkApps1.Start(Seconds(0.0));

    // Connect Rx callbacks
    sinkApps0.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxCallback, 0u));
    sinkApps1.Get(0)->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxCallback, 1u));

    ApplicationContainer sendApp0;
    ApplicationContainer sendApp1;

    if (useUdp)
    {
        // Each UDP source emits at the access rate (T Mbps) -> bottleneck
        // is 2x oversubscribed -> scheduler decides allocation purely on
        // weight, free of TCP cwnd / RTO feedback dynamics. Reveals the
        // scheduler's true weight-tracking behaviour.
        OnOffHelper udp0(sockFactory, InetSocketAddress(ifBn.GetAddress(1), port0));
        udp0.SetAttribute("DataRate", DataRateValue(DataRate(dataRateMbps * 1e6)));
        udp0.SetAttribute("PacketSize", UintegerValue(1000));
        udp0.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
        udp0.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        OnOffHelper udp1(sockFactory, InetSocketAddress(ifBn.GetAddress(1), port1));
        udp1.SetAttribute("DataRate", DataRateValue(DataRate(dataRateMbps * 1e6)));
        udp1.SetAttribute("PacketSize", UintegerValue(1000));
        udp1.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
        udp1.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));

        sendApp0 = udp0.Install(senders.Get(0));
        sendApp1 = udp1.Install(senders.Get(1));
    }
    else
    {
        BulkSendHelper bulk0(sockFactory, InetSocketAddress(ifBn.GetAddress(1), port0));
        bulk0.SetAttribute("MaxBytes", UintegerValue(0));
        bulk0.SetAttribute("SendSize", UintegerValue(1000));

        BulkSendHelper bulk1(sockFactory, InetSocketAddress(ifBn.GetAddress(1), port1));
        bulk1.SetAttribute("MaxBytes", UintegerValue(0));
        bulk1.SetAttribute("SendSize", UintegerValue(1000));

        sendApp0 = bulk0.Install(senders.Get(0));
        sendApp1 = bulk1.Install(senders.Get(1));
    }

    sendApp0.Start(Seconds(1.0));
    sendApp1.Start(Seconds(1.0));

    // Sim time: long enough for TCP cwnd to reach steady state at the
    // configured access rate. Default heuristic: 600 s for T <= 1 Mbps
    // (slow cwnd ramp), 300 s for T = 10 Mbps (Chang et al. default),
    // and 1200 s for T >= 50 Mbps (high-rate cwnd does not settle in
    // 300 s — Q-16.1 spec-methodology refinement, 2026-05-03). The
    // --simTime CLI flag overrides the heuristic.
    double simTime = 300.0;
    if (dataRateMbps <= 1.0)
    {
        simTime = 600.0;
    }
    else if (dataRateMbps >= 50.0)
    {
        simTime = 1200.0;
    }
    if (simTimeOverride > 0.0)
    {
        simTime = simTimeOverride;
    }

    // ---- Sampling ----
    double sampleInterval = 0.001; // 1ms as in paper
    // But for plotting, use coarser intervals at low data rates
    if (dataRateMbps <= 1.0)
    {
        sampleInterval = 1.0;
    }
    else if (dataRateMbps <= 10.0)
    {
        sampleInterval = 0.1;
    }

    std::string ratioPath = runDir + "/ratio.tr";
    g_ratioFile.open(ratioPath);
    Simulator::Schedule(Seconds(2.0), &SampleRatio, sampleInterval, weightRatio);

    // ---- Run ----
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    g_ratioFile.close();

    // Final summary: compute ratio from second half of sim (steady state)
    double halfTime = simTime / 2.0;
    // Re-read ratio.tr and compute mean ratio from second half
    g_ratioFile.close();
    std::ifstream ratioIn(ratioPath);
    double sumRatio = 0.0;
    int countRatio = 0;
    {
        double t;
        double r;
        while (ratioIn >> t >> r)
        {
            if (t >= halfTime && r > 0.01 && r < 1000.0)
            {
                sumRatio += r;
                countRatio++;
            }
        }
    }
    double finalRatio = (countRatio > 0) ? (sumRatio / countRatio) : 0.0;

    double totalR0 = g_rxBytes[0] * 8.0 / 1e6; // Mbits
    double totalR1 = g_rxBytes[1] * 8.0 / 1e6;

    std::printf("Scheduler: %s  T=%.1f Mbps  Bottleneck=%.1f Mbps\n",
                scheduler.c_str(),
                dataRateMbps,
                bottleneckMbps);
    std::printf("Weights: w1=%.1f  w2=%.1f  ratio=%.1f\n", w1, w2, weightRatio);
    std::printf("Flow 0: %.2f Mbit  Flow 1: %.2f Mbit\n", totalR0, totalR1);
    std::printf("Perceived ratio: %.4f  (expected: %.1f)\n", finalRatio, weightRatio);
    std::printf("Error: %.2f%%\n", std::abs(finalRatio - weightRatio) / weightRatio * 100.0);

    // Write summary
    std::string summaryPath = runDir + "/summary.txt";
    FILE* fp = std::fopen(summaryPath.c_str(), "w");
    NS_ABORT_MSG_IF(fp == nullptr,
                    "chang-comparison: cannot open summary file " << summaryPath << " for writing");
    std::fprintf(fp, "scheduler %s\n", scheduler.c_str());
    std::fprintf(fp, "dataRate_Mbps %.1f\n", dataRateMbps);
    std::fprintf(fp, "bottleneck_Mbps %.1f\n", bottleneckMbps);
    std::fprintf(fp, "w1 %.1f\n", w1);
    std::fprintf(fp, "w2 %.1f\n", w2);
    std::fprintf(fp, "weightRatio %.1f\n", weightRatio);
    std::fprintf(fp, "flow0_Mbit %.2f\n", totalR0);
    std::fprintf(fp, "flow1_Mbit %.2f\n", totalR1);
    std::fprintf(fp, "perceived_ratio %.4f\n", finalRatio);
    std::fprintf(fp, "expected_ratio %.1f\n", weightRatio);
    std::fprintf(fp, "error_pct %.2f\n", std::abs(finalRatio - weightRatio) / weightRatio * 100.0);
    std::fclose(fp);

    Simulator::Destroy();
    return 0;
}
