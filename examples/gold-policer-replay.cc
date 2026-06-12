/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Gold-class policer trace-replay equivalence check.
 *
 * Replays a captured ingress packet stream, byte-for-byte, through the
 * ns-3 TSW2CM meter + RIO-C policer configured exactly as the Gold class
 * of the three-way DiffServ scenario, and reports the fraction of packets
 * the meter downgraded to the out-of-profile code point (AF12).
 *
 * The reference stream is an ns-2.35 Gold ingress capture; the ns-2.35
 * TSW2CM logged its own out-of-profile (AF12) fraction on that same
 * stream. Feeding the identical stream to the ns-3 meter and comparing
 * the two AF12 fractions isolates the policer from the traffic generator:
 * a match demonstrates the two TSW2CM implementations are equivalent
 * given byte-identical input, so any cross-simulator throughput residual
 * is attributable to the generator, not to the metering.
 *
 * The meter marks probabilistically (downgrade with probability
 * 1 - CIR/avgRate when out of profile), so a few percent of spread around
 * the reference fraction is expected; the comparison is a fraction match
 * within RNG noise, not a bit-exact equality.
 *
 * Usage:
 *   ./ns3 run "gold-policer-replay --pcap=<abs path to ingress pcap>"
 */

#include "ns3/core-module.h"
#include "ns3/double.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/network-module.h"
#include "ns3/stratum-constants.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-helper.h"
#include "ns3/stratum-pq-scheduler.h"
#include "ns3/stratum-red-queue-disc.h"
#include "ns3/stratum-trace-replay-application.h"
#include "ns3/traffic-control-module.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace ns3;
namespace diffserv = ns3::stratum::diffserv;
using ns3::stratum::EdgeQueueDisc;
using ns3::stratum::Meter;
using ns3::stratum::MredMode;
using ns3::stratum::PolicerType;
using ns3::stratum::PriorityScheduler;
using ns3::stratum::TraceReplayApplication;

NS_LOG_COMPONENT_DEFINE("GoldPolicerReplay");

int
main(int argc, char* argv[])
{
    // ---- Command line ----
    std::string pcapPath;
    double cir = 600000.0;    // Gold CIR in bit/s (thesis: 600 kbps)
    int64_t rngRun = 1;       // RNG run number (varies the probabilistic marking)
    double stopTime = 5002.0; // seconds; must exceed the capture span (~5000 s)

    CommandLine cmd(__FILE__);
    cmd.AddValue("pcap", "Absolute path to the Gold ingress pcap to replay", pcapPath);
    cmd.AddValue("cir", "TSW2CM committed information rate in bit/s", cir);
    cmd.AddValue("rngRun", "RNG run number (varies probabilistic marking)", rngRun);
    cmd.AddValue("stop", "Simulator stop time in seconds (> capture span)", stopTime);
    cmd.Parse(argc, argv);

    if (pcapPath.empty())
    {
        std::cerr << "error: --pcap=<path> is required\n";
        return 1;
    }

    RngSeedManager::SetRun(static_cast<uint64_t>(rngRun));

    // ====================================================================
    // Gold-only DiffServ edge.
    //
    //   1 physical queue, 2 drop precedences:
    //     prec 0 = AF11 (DSCP 10, in-profile)
    //     prec 1 = AF12 (DSCP 12, out-of-profile / downgraded)
    //
    //   Meter:   TSW2CM, CIR = 600 kbps (downgrade 10 -> 12 when over rate)
    //   Marking is the metric under test, so the inner queue is set to
    //   drop-tail with a limit far above the stream length: every metered
    //   packet enqueues, no queue-occupancy drop perturbs the AF12 count.
    // ====================================================================
    Ptr<EdgeQueueDisc> edgeDisc = CreateObject<EdgeQueueDisc>();
    diffserv::Helper helper;
    auto edgeInner = helper.InstallRedInner(edgeDisc);

    edgeInner->SetNumQueues(1);
    edgeInner->SetNumPrec(0, 2);          // AF11 (prec 0) + AF12 (prec 1)
    edgeInner->SetQueueLimit(0, 5000000); // larger than the ~1.06 M-packet stream

    // Single-queue priority scheduler (irrelevant to the marking metric, but
    // a scheduler is required). WinLen defaults to 1.0 s.
    auto sched = CreateObject<PriorityScheduler>();
    edgeInner->SetScheduler(sched);

    // No mark rules: the replayed packets already carry DSCP 10, and the
    // classifier preserves an unmatched packet's existing code point, so
    // every packet enters the Gold (AF11) policy as captured.

    // Meter + policer: TSW2CM on DSCP 10, downgrading out-of-profile to 12.
    helper.AddTsw2cmPolicy(edgeDisc, 10, cir);
    helper.AddDumbPolicy(edgeDisc, 12);
    helper.AddPolicerEntry(edgeDisc, PolicerType::TSW2CM, 10, 12, 12);
    helper.AddPolicerEntry(edgeDisc, PolicerType::DUMB, 12, 12, 12);

    // PHB table: AF11 -> queue 0 / prec 0; AF12 -> queue 0 / prec 1.
    helper.AddPhbEntry(edgeInner, 10, 0, 0);
    helper.AddPhbEntry(edgeInner, 12, 0, 1);

    edgeDisc->Initialize();

    // Drop-tail (no RED early drop), so only the policer affects code points.
    edgeInner->SetMredMode(MredMode::DROP_TAIL, 0);
    edgeInner->SetMeanPacketSize(245);

    // In drop-tail mode the per-precedence threshold is the accept ceiling
    // (in packets). Set it far above the stream length for both AF11 and
    // AF12 so every metered packet enqueues and the AF12 count reflects the
    // policer verdict alone, with no queue-occupancy drop.
    helper.ConfigQueue(edgeInner, 0, 0, 6000000.0, 6000000.0, 1.0);
    helper.ConfigQueue(edgeInner, 0, 1, 6000000.0, 6000000.0, 1.0);

    // ---- Replay application ----
    Ptr<Node> node = CreateObject<Node>();
    Ptr<TraceReplayApplication> app = CreateObject<TraceReplayApplication>();
    app->AddInputPcap(pcapPath);
    app->SetTargetQdisc(edgeDisc);
    node->AddApplication(app);
    app->SetStartTime(Seconds(0));

    std::cout << "Replaying " << pcapPath << " through the Gold TSW2CM/RIO-C policer "
              << "(CIR " << cir << " bit/s, RNG run " << rngRun << ") ...\n";

    Simulator::Stop(Seconds(stopTime));
    Simulator::Run();

    std::cout << "Replayed " << app->GetReplayedCount() << " packets; draining and counting ...\n";

    // Validity guard: the comparison is only meaningful if no packet was
    // dropped by the inner queue (a drop would remove a metered packet from
    // the tally and bias the AF12 fraction). With drop-tail + an accept
    // ceiling above the stream length, this must be zero.
    uint32_t dropped = edgeDisc->GetStats().nTotalDroppedPackets;
    if (dropped != 0)
    {
        std::cerr << "warning: " << dropped
                  << " packets dropped by the inner queue; the AF12 fraction is biased "
                     "(raise the queue limit / accept ceiling)\n";
    }

    // ---- Drain the queue and tally the post-policer code points ----
    // The composer rewrites each dequeued item's IPv4 DSCP from the code
    // point the policer assigned at enqueue, so the dequeued header carries
    // the metering verdict directly.
    uint64_t af11 = 0; // DSCP 10, in-profile
    uint64_t af12 = 0; // DSCP 12, downgraded
    uint64_t other = 0;
    Ptr<QueueDiscItem> item;
    while ((item = edgeDisc->Dequeue()))
    {
        Ptr<Ipv4QueueDiscItem> ip = DynamicCast<Ipv4QueueDiscItem>(item);
        if (!ip)
        {
            ++other;
            continue;
        }
        uint8_t dscp = static_cast<uint8_t>(ip->GetHeader().GetDscp());
        if (dscp == 12)
        {
            ++af12;
        }
        else if (dscp == 10)
        {
            ++af11;
        }
        else
        {
            ++other;
        }
    }

    uint64_t total = af11 + af12 + other;
    double af12Frac = (total > 0) ? static_cast<double>(af12) / static_cast<double>(total) : 0.0;

    std::cout << "----------------------------------------------------------------\n";
    std::cout << "Gold policer trace-replay result\n";
    std::cout << "  total packets   : " << total << "\n";
    std::cout << "  dropped (=0 ok) : " << dropped << "\n";
    std::cout << "  AF11 (DSCP 10)  : " << af11 << "\n";
    std::cout << "  AF12 (DSCP 12)  : " << af12 << "\n";
    std::cout << "  other DSCP      : " << other << "\n";
    std::cout << "  ns-3 AF12 frac  : " << af12Frac << "\n";
    std::cout << "----------------------------------------------------------------\n";

    Simulator::Destroy();
    return 0;
}
