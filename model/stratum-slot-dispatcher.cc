/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-slot-dispatcher.h"

#include "stratum-edge-queue-disc.h"

#include "ns3/log.h"
#include "ns3/queue-disc.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::SlotDispatcher");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(SlotDispatcher);

TypeId
SlotDispatcher::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::stratum::SlotDispatcher").SetParent<Object>().SetGroupName("Stratum");
    return tid;
}

NS_OBJECT_ENSURE_REGISTERED(StrictPriorityDispatcher);

TypeId
StrictPriorityDispatcher::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::StrictPriorityDispatcher")
                            .SetParent<SlotDispatcher>()
                            .SetGroupName("Stratum")
                            .AddConstructor<StrictPriorityDispatcher>();
    return tid;
}

int32_t
StrictPriorityDispatcher::SelectDequeueSlot(EdgeQueueDisc* edge)
{
    return FirstNonEmpty(edge);
}

int32_t
StrictPriorityDispatcher::PeekSlot(EdgeQueueDisc* edge)
{
    return FirstNonEmpty(edge);
}

int32_t
StrictPriorityDispatcher::FirstNonEmpty(EdgeQueueDisc* edge) const
{
    NS_ASSERT_MSG(edge, "StrictPriorityDispatcher requires a non-null edge");
    const uint32_t kMax = EdgeQueueDisc::kMaxInnerSlots;
    for (uint32_t s = 0; s < kMax; ++s)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(s);
        if (!inner)
        {
            break; // slots fill monotonically — stop at first empty
        }
        if (inner->GetNPackets() > 0)
        {
            return static_cast<int32_t>(s);
        }
    }
    return -1;
}

} // namespace ns3::stratum
