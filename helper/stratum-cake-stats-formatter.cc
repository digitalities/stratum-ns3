/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Adapted ns-3 traffic-control patterns originally contributed by
 * Pasquale Imputato and Stefano Avallone.
 */

#include "stratum-cake-stats-formatter.h"

#include "ns3/cobalt-queue-disc.h"
#include "ns3/fq-cobalt-queue-disc.h"
#include "ns3/log.h"
#include "ns3/queue-disc.h"
#include "ns3/stratum-edge-queue-disc.h"
#include "ns3/stratum-slot-dispatcher.h"
#include "ns3/tbf-queue-disc.h"

#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::StatsFormatter");

namespace
{

/**
 * @brief Format a bit-rate as a Linux-style human-readable token
 * (e.g. "100Mbit", "1.5Gbit", "0bit").
 *
 * Mirrors `iproute2 lib/utils.c::sprint_rate` ordering: bps, Kbit,
 * Mbit, Gbit. Sub-Mbit values keep one decimal digit; Mbit/Gbit
 * round to integer when the fractional part is zero.
 */
std::string
FormatRate(uint64_t bps)
{
    if (bps == 0)
    {
        return "0bit";
    }
    std::ostringstream oss;
    if (bps >= 1000000000ULL)
    {
        oss << (bps / 1000000000.0) << "Gbit";
    }
    else if (bps >= 1000000ULL)
    {
        oss << (bps / 1000000ULL) << "Mbit";
    }
    else if (bps >= 1000ULL)
    {
        oss << (bps / 1000ULL) << "Kbit";
    }
    else
    {
        oss << bps << "bit";
    }
    return oss.str();
}

/**
 * @brief Probe the inner at @p slot for the per-tin link rate the
 * shaper is enforcing.
 *
 * When the slot wraps a `TbfQueueDisc` (path γ — `useInnerTbfShaping`
 * or `enableTinShaping=true`) the formatter reports the TBF's
 * configured `Rate` attribute. Slots without a TBF wrapper return 0
 * (work-conserving DRR — no per-tin cap).
 */
uint64_t
ProbeTinRateBps(Ptr<QueueDisc> inner)
{
    if (!inner)
    {
        return 0;
    }
    Ptr<TbfQueueDisc> tbf = inner->GetObject<TbfQueueDisc>();
    if (!tbf)
    {
        return 0;
    }
    return tbf->GetRate().GetBitRate();
}

/**
 * @brief Walk an inner queue disc to count the flows ever observed.
 *
 * Returns the value of `inner->GetNQueueDiscClasses()` for a stock
 * `FqCobaltQueueDisc`. Returns 0 for non-flow-keyed inners
 * (`CobaltQueueDisc` under flowblind mode, flat DropTail under path β).
 *
 * **Substrate fidelity gap.** Stock ns-3 `FqCobaltQueueDisc::m_classes`
 * is append-only — the count includes flows that have since drained
 * to empty. Reported under the `ever_seen` field rather than the
 * Linux name `bulk_flow_count` to keep the discrepancy honest.
 */
uint32_t
EverSeenFlowCount(Ptr<QueueDisc> inner)
{
    if (!inner)
    {
        return 0;
    }
    return static_cast<uint32_t>(inner->GetNQueueDiscClasses());
}

/**
 * @brief Identify the per-tin inner type for the diagnostic header.
 *
 * Reports the concrete inner class as a short token: `fq_cobalt`,
 * `cobalt`, `tbf`, or `unknown`. Used in the per-tin block so operators
 * can verify path composition without parsing the full helper-call chain.
 */
std::string
InnerKindToken(Ptr<QueueDisc> inner)
{
    if (!inner)
    {
        return "none";
    }
    if (inner->GetObject<FqCobaltQueueDisc>())
    {
        return "fq_cobalt";
    }
    if (inner->GetObject<CobaltQueueDisc>())
    {
        return "cobalt";
    }
    if (inner->GetObject<TbfQueueDisc>())
    {
        return "tbf";
    }
    return "unknown";
}

} // namespace

void
StatsFormatter::Print(std::ostream& os, Ptr<const QueueDisc> edgeBase)
{
    NS_LOG_FUNCTION(edgeBase);

    if (!edgeBase)
    {
        os << "qdisc cake (null)\n";
        return;
    }

    // The CAKE-aware report requires EdgeQueueDisc accessors
    // (GetTinStats, GetInnerDiscAt, GetNumInnerSlots). Non-edge qdiscs
    // get the QueueDisc::Stats fallback header only.
    Ptr<const EdgeQueueDisc> edge = DynamicCast<const EdgeQueueDisc>(edgeBase);
    if (!edge)
    {
        // Non-CAKE qdisc: minimal header so the caller still gets a
        // useful one-line diagnostic. `GetStats()` is non-const on
        // QueueDisc, so cast away const for the read-only counter
        // copy.
        auto qd = const_cast<QueueDisc*>(PeekPointer(edgeBase));
        const auto& s = qd->GetStats();
        os << "qdisc (non-cake)\n"
           << " Sent " << s.nTotalSentBytes << " bytes " << s.nTotalSentPackets << " pkt (dropped "
           << s.nTotalDroppedPackets << ", overlimits 0 requeues " << s.nTotalRequeuedPackets
           << ")\n"
           << " backlog " << qd->GetNBytes() << "b " << qd->GetNPackets() << "p\n";
        return;
    }

    // ---- Header line ----
    //
    // Mirror iproute2 `cake_print_opt`: qdisc tag, pseudo-handle, base
    // option tokens (besteffort/diffserv4/etc. is left to the caller's
    // deployment knowledge — the helper does not retain the profile
    // selector after composition).
    auto edgeMut = const_cast<QueueDisc*>(static_cast<const QueueDisc*>(PeekPointer(edge)));
    const auto& edgeStats = edgeMut->GetStats();
    const uint32_t numTins = edge->GetNumInnerSlots();

    os << "qdisc cake 0: dev (ns-3)";
    os << " tins " << numTins;
    os << "\n";

    // ---- Aggregate Sent / dropped / backlog ----
    os << " Sent " << edgeStats.nTotalSentBytes << " bytes " << edgeStats.nTotalSentPackets
       << " pkt (dropped " << edgeStats.nTotalDroppedPackets << ", overlimits 0 requeues "
       << edgeStats.nTotalRequeuedPackets << ")\n";
    os << " backlog " << edgeMut->GetNBytes() << "b " << edgeMut->GetNPackets() << "p requeues "
       << edgeStats.nTotalRequeuedPackets << "\n";

    // ---- Memory and capacity (substrate-fidelity gap) ----
    //
    // Linux `tc -s qdisc show cake` prints `memory used: ... of ...` and
    // `capacity estimate`. The substrate has no aggregate memlimit
    // counter (per-tin MemLimit lives on the inner FqCobalt; aggregate
    // is v1.1) and no learned capacity estimator. Report explicit
    // "n/a" tokens so a downstream parser can tell the line is present
    // but not load-bearing — better than omitting the line and
    // breaking section-key expectations.
    os << " memory used: n/a of n/a\n";
    os << " capacity estimate: n/a\n";

    // ---- Per-tin block ----
    //
    // Linux's `cake_print_xstats` emits a per-tin header row followed
    // by `thresh`, `target`, drop counts, packets/bytes, and bulk-flow
    // counter. The per-row layout below preserves the same
    // conceptual ordering with substrate-faithful fields.
    for (uint32_t slot = 0; slot < numTins; ++slot)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(slot);
        TinStats t = edge->GetTinStats(slot);

        os << " tin " << slot << " kind=" << InnerKindToken(inner) << "\n";

        const uint64_t rateBps = ProbeTinRateBps(inner);
        os << "  thresh " << FormatRate(rateBps) << "\n";

        // bytes/pkts: enqueued vs dequeued so operators can see the
        // current backlog at a glance (Linux conflates them under
        // `Sent` because Linux qdiscs don't separate enqueue from
        // post-shape dequeue accounting).
        os << "  bytes_enqueued " << t.bytesEnqueued << " bytes_dequeued " << t.bytesDequeued
           << "\n";
        os << "  drops " << t.drops << " marks " << t.marks << "\n";

        // ever_seen: substrate-fidelity-gap field. Linux reports
        // `bulk_flow_count` (live count of flows currently active in
        // the bucket); the substrate's stock `FqCobaltQueueDisc`
        // counts flows ever observed. See class Doxygen for the gap
        // rationale.
        const uint32_t everSeen = EverSeenFlowCount(inner);
        os << "  ever_seen " << everSeen
           << " (substrate-gap: stock FqCobaltQueueDisc class list is "
              "append-only; live flow count is v1.1)\n";

        // backlog
        const uint32_t innerNPkts = inner ? inner->GetNPackets() : 0u;
        const uint32_t innerBytes = inner ? inner->GetNBytes() : 0u;
        os << "  backlog " << innerBytes << "b " << innerNPkts << "p\n";
    }
}

} // namespace ns3::stratum::cake
