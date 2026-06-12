/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Retransmission-counter calibration harness.
 *
 * Single TCP bulk-transfer flow across a 2-node point-to-point link with
 * exactly one deterministic loss injected on the receiver NIC (segment 10).
 * The sender-side DiffServ edge queue disc is monitored by a
 * MonitorHelper, which split-counts per-DSCP bytes by
 * TcpRetransmitTag presence. Expected post-run output:
 *
 * - DSCP 0 orig bytes ~= 109 000 (100 kB BulkSend + TCP/IP headers)
 * - DSCP 0 retx bytes = one packet-size (with ns-3's default
 * SegmentSize=536 and TCP timestamp option: ~588 bytes; with
 * SegmentSize=1448: ~1500 bytes). A single deterministic drop
 * produces exactly one fast retransmission.
 * - DSCP 0 goodput_thesis = origBytes / (origBytes + retxBytes)
 * > 0.95 for a single-drop single-flow run.
 *
 * The sanity check is ratio-based: retxBytes must be strictly > 0
 * (counter fires) and retxBytes/(origBytes+retxBytes) < 0.05 (retx is a
 * small minority, no double-counting). If either invariant breaks, the
 * counter is unreliable and a 6-set sweep's goodput column cannot be
 * trusted. This harness is a 5-second sanity check that must pass
 * before long sweeps.
 *
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-monitor-helper.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-statistics.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::kAnyAppType;
using ns3::stratum::kAnyHost;
using ns3::stratum::kAnyProtocol;
using ns3::stratum::MonitorHelper;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::Statistics;

NS_LOG_COMPONENT_DEFINE("RetxCalibration");

int
main(int argc, char* argv[])
{
    // Fixed RNG for determinism; the drop is deterministic anyway, but
    // BulkSend's socket-connect timing can jitter without a fixed seed.
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    uint32_t dropSegment = 10;
    uint32_t bulkBytes = 100000;
    double simTimeSec = 10.0;

    CommandLine cmd(__FILE__);
    cmd.AddValue("dropSegment", "Segment index (1-based) to drop on the receiver NIC", dropSegment);
    cmd.AddValue("bulkBytes", "BulkSend MaxBytes", bulkBytes);
    cmd.AddValue("simTime", "Simulation duration (seconds)", simTimeSec);
    cmd.Parse(argc, argv);

    // --- Topology: n0 (sender) ---p2p--- n1 (receiver) ---
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    NetDeviceContainer devs = p2p.Install(nodes);

    // --- Drop exactly one segment on the receiver NIC. ReceiveListErrorModel
    // indexes packets 1-based; the drop happens after the packet has
    // traversed n0's edge disc (orig counter +1), so the eventual
    // fast-retransmit re-traverses the edge disc (retx counter +1).
    auto em = CreateObject<ReceiveListErrorModel>();
    std::list<uint32_t> drops{dropSegment};
    em->SetList(drops);
    devs.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(em));

    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer ifs = addr.Assign(devs);

    // --- Edge disc on the sender's outgoing device. Single queue, single
    // precedence, DROP_TAIL (no RED) — we do not want the calibration
    // to lose packets to RED at enqueue; the only intended drop is the
    // one on the receiver NIC.
    //
    // Replace ns-3's default pfifo_fast with EdgeQueueDisc on
    // the sender NIC (n0). The receiver's default disc is left alone.
    TrafficControlHelper tchUninstall;
    tchUninstall.Uninstall(devs.Get(0));

    Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
    auto inner = CreateObject<stratum::RedQueueDisc>();
    edge->SetInnerDisc(inner);
    inner->SetNumQueues(1);
    inner->SetNumPrec(0, 1);
    inner->SetQueueLimit(0, 200);
    inner->SetMredMode(MredMode::DROP_TAIL);

    diffserv::Helper helper;
    // any → DSCP 0 (kAnyAppType = 0 per the helper's doc-comment).
    helper.AddMarkRule(edge, 0, kAnyHost, kAnyHost, kAnyProtocol, 0);
    helper.AddDumbPolicy(edge, 0);
    helper.AddPolicerEntry(edge, PolicerType::DUMB, 0, 0, 0);
    helper.AddPhbEntry(inner, 0, 0, 0); // DSCP 0 → queue 0, prec 0

    Ptr<TrafficControlLayer> tc = nodes.Get(0)->GetObject<TrafficControlLayer>();
    tc->SetRootQueueDiscOnDevice(devs.Get(0), edge);
    edge->Initialize();

    // --- Monitor: hooks StratumEnqueue/Dequeue/Drop traces and splits per-DSCP
    // byte counters by TcpRetransmitTag presence.
    MonitorHelper monitor;
    monitor.Install(edge);

    // --- Application: receiver sink, sender bulk-transfer on TCP.
    uint16_t dstPort = 50000;

    PacketSinkHelper sink("ns3::TcpSocketFactory",
                          InetSocketAddress(Ipv4Address::GetAny(), dstPort));
    ApplicationContainer sinkApp = sink.Install(nodes.Get(1));
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(simTimeSec));

    BulkSendHelper bulk("ns3::TcpSocketFactory", InetSocketAddress(ifs.GetAddress(1), dstPort));
    bulk.SetAttribute("MaxBytes", UintegerValue(bulkBytes));
    ApplicationContainer srcApp = bulk.Install(nodes.Get(0));
    srcApp.Start(Seconds(0.1));
    srcApp.Stop(Seconds(simTimeSec));

    Simulator::Stop(Seconds(simTimeSec));
    Simulator::Run();

    // --- Report.
    std::cout << "\n==== retx-calibration: single-flow single-drop ====\n"
              << "drop segment    : " << dropSegment << "\n"
              << "bulk bytes tgt  : " << bulkBytes << "\n\n";

    edge->PrintStats();
    monitor.PrintStats();

    // Explicit goodput pass/fail banner for the calibration.
    Ptr<Statistics> stats = monitor.GetStats();
    uint64_t orig = stats->GetOrigBytes(0);
    uint64_t retx = stats->GetRetxBytes(0);
    double goodput =
        (orig + retx) > 0 ? static_cast<double>(orig) / static_cast<double>(orig + retx) : 0.0;
    std::cout << "\n==== calibration verdict ====\n"
              << "orig bytes (DSCP 0) : " << orig << "\n"
              << "retx bytes (DSCP 0) : " << retx << "\n"
              << "goodput_thesis       : " << goodput << "\n";

    // Sanity invariants, robust to SegmentSize choice:
    // - retx > 0 (counter actually increments on a stamped retx packet)
    // - retx / (orig + retx) < 0.05 (single-drop run, retx is a minority)
    // - goodput in (0.95, 1.0) (little loss but some)
    // With ns-3's default SegmentSize=536, observed retx ~= 588 bytes per
    // retransmitted packet (20 IP + 20 TCP + 12 timestamp option + 536).
    double retxFraction = (orig + retx) > 0 ? static_cast<double>(retx) / (orig + retx) : 0.0;
    bool retxOk = retx > 0 && retxFraction < 0.05;
    bool goodputOk = goodput > 0.95 && goodput < 1.0;
    if (retxOk && goodputOk)
    {
        std::cout << "verdict             : PASS\n";
    }
    else
    {
        std::cout << "verdict             : FAIL "
                  << "(retx>0 && retx/total<0.05 && goodput in (0.95,1.0))\n";
    }

    Simulator::Destroy();
    return (retxOk && goodputOk) ? 0 : 1;
}
