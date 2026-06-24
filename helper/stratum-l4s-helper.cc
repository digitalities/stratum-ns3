/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-l4s-helper.h"

#include "ns3/log.h"
#include "ns3/object-factory.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/uinteger.h"

namespace ns3::stratum::l4s
{

NS_LOG_COMPONENT_DEFINE("StratumL4sHelper");

void
Helper::SetAsL4s(Ptr<QueueDisc> disc, const L4sSpec& spec)
{
    NS_LOG_FUNCTION(disc);

    const uint32_t classicIdx = (spec.l4sQueueIdx == 0) ? 1u : 0u;

    disc->SetNumQueues(2);
    disc->SetL4sQueueIdx(spec.l4sQueueIdx);
    disc->SetQueueLimit(0, spec.queueLimit);
    disc->SetQueueLimit(1, spec.queueLimit);
    disc->AddPhbEntry(46, static_cast<uint8_t>(spec.l4sQueueIdx), 0); // EF fallback -> L4S lane
    disc->AddPhbEntry(0, static_cast<uint8_t>(classicIdx), 0);        // best-effort -> classic lane

    Ptr<CoupledScheduler> scheduler =
        CreateObjectWithAttributes<CoupledScheduler>("NumQueues",
                                                     UintegerValue(2),
                                                     "L4sQueueIdx",
                                                     UintegerValue(spec.l4sQueueIdx),
                                                     "BurstCap",
                                                     UintegerValue(spec.burstCap));
    disc->SetScheduler(scheduler);

    // ConfigQueue forwards to the inner classic WRED, so .queue selects a
    // precedence band within the classic lane — not the L4S lane, which is a
    // FIFO whose congestion signal is the coupled/step CE mark applied at
    // dequeue. Both calls below tune the classic WRED: one band wide, one
    // band the standard profile.
    disc->ConfigQueue(
        {.queue = spec.l4sQueueIdx, .prec = 0, .thMin = 100.0, .thMax = 200.0, .maxP = 0.1});
    disc->ConfigQueue({.queue = classicIdx, .prec = 0, .thMin = 30.0, .thMax = 80.0, .maxP = 0.1});
}

} // namespace ns3::stratum::l4s
