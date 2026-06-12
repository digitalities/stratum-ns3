/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-hybrid-llq-dispatcher.h"

#include "stratum-edge-queue-disc.h"

#include "ns3/log.h"
#include "ns3/queue-disc.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::HybridLlqDispatcher");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(HybridLlqDispatcher);

TypeId
HybridLlqDispatcher::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::HybridLlqDispatcher")
                            .SetParent<SlotDispatcher>()
                            .SetGroupName("Stratum")
                            .AddConstructor<HybridLlqDispatcher>();
    return tid;
}

HybridLlqDispatcher::HybridLlqDispatcher()
{
    NS_LOG_FUNCTION(this);
}

void
HybridLlqDispatcher::SetSlotStrictPriority(uint32_t slot)
{
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::SetSlotStrictPriority: slot " << slot << " out of range");
    NS_ASSERT_MSG(m_quantum[slot] == 0,
                  "HybridLlqDispatcher::SetSlotStrictPriority: slot "
                      << slot << " already has DRR quantum " << m_quantum[slot]
                      << "; SP and DRR are mutually exclusive");
    m_isStrictPriority[slot] = true;
}

bool
HybridLlqDispatcher::IsStrictPriority(uint32_t slot) const
{
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::IsStrictPriority: slot " << slot << " out of range");
    return m_isStrictPriority[slot];
}

void
HybridLlqDispatcher::SetQuantum(uint32_t slot, uint32_t bytes)
{
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::SetQuantum: slot " << slot << " out of range");
    NS_ASSERT_MSG(bytes > 0, "HybridLlqDispatcher::SetQuantum: quantum must be > 0");
    NS_ASSERT_MSG(!m_isStrictPriority[slot],
                  "HybridLlqDispatcher::SetQuantum: slot "
                      << slot << " is strict-priority; SP and DRR are mutually exclusive");
    m_quantum[slot] = bytes;
}

uint32_t
HybridLlqDispatcher::GetQuantum(uint32_t slot) const
{
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::GetQuantum: slot " << slot << " out of range");
    return m_quantum[slot];
}

void
HybridLlqDispatcher::SetRateCap(uint32_t slot, uint64_t rateBps, uint64_t burstBytes)
{
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::SetRateCap: slot " << slot << " out of range");
    if (rateBps > 0)
    {
        NS_ASSERT_MSG(burstBytes > 0,
                      "HybridLlqDispatcher::SetRateCap: burstBytes must be > 0 when rateBps > 0 "
                      "(slot "
                          << slot << ")");
    }
    m_tokenBuckets[slot].Configure(rateBps, burstBytes, Simulator::Now());
}

uint64_t
HybridLlqDispatcher::GetRateCapBps(uint32_t slot) const
{
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::GetRateCapBps: slot " << slot << " out of range");
    return m_tokenBuckets[slot].rateBps;
}

int32_t
HybridLlqDispatcher::FirstReadyStrictPrioritySlot(EdgeQueueDisc* edge) const
{
    const uint32_t kMax = EdgeQueueDisc::kMaxInnerSlots;
    for (uint32_t i = 0; i < kMax; ++i)
    {
        if (!m_isStrictPriority[i])
        {
            continue;
        }
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(i);
        if (!inner || inner->GetNPackets() == 0)
        {
            continue;
        }
        // Token gate: a rate-capped SP slot is held to its configured
        // ceiling even though SP would otherwise win unconditionally.
        // This is the Cisco MQC LLQ pattern — priority class with hard
        // bandwidth ceiling. When `rateBps == 0` the bucket is
        // disabled and `HasTokensFor` returns true unconditionally,
        // making the gate a no-op. `inner->Peek()` is a cached read
        // on RED / CoDel / FqCobalt inners — no allocation, no state
        // mutation.
        Ptr<const QueueDiscItem> head = inner->Peek();
        if (!head)
        {
            // Inner refused to peek even though GetNPackets > 0 — the
            // path-gamma TBF-as-inner case where tokens are exhausted.
            // Skip so SP walk falls through to the next priority slot
            // or, eventually, to DRR.
            continue;
        }
        if (!m_tokenBuckets[i].HasTokensFor(head->GetSize(), Simulator::Now()))
        {
            continue;
        }
        return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t
HybridLlqDispatcher::SelectDequeueSlot(EdgeQueueDisc* edge)
{
    NS_ASSERT_MSG(edge, "HybridLlqDispatcher::SelectDequeueSlot: edge must be non-null");

    // 1. Strict-priority fast path. SP slots win whenever they are
    // non-empty, with no deficit accounting; this is the source of the
    // sub-30 ms latency claim under saturated RRUL.
    const int32_t spSlot = FirstReadyStrictPrioritySlot(edge);
    if (spSlot >= 0)
    {
        return spSlot;
    }

    // 2. DRR fall-through over non-SP slots. Same algorithm as
    // cake::TinShaperDispatcher: round-start credit when deficit has
    // exhausted, idle slots skipped without crediting, cursor stays
    // on the served slot until OnDequeue advances it on round
    // completion. The cursor is only ever an index into non-SP slots;
    // SP slots are skipped during the walk.
    const uint32_t kMax = EdgeQueueDisc::kMaxInnerSlots;
    for (uint32_t tries = 0; tries < kMax; ++tries)
    {
        const uint32_t i = (m_drrCursor + tries) % kMax;
        if (m_isStrictPriority[i])
        {
            continue;
        }
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(i);
        if (!inner || inner->GetNPackets() == 0)
        {
            continue;
        }
        // Token gate — mirrors cake::TinShaperDispatcher exactly: sits
        // BEFORE the round-start deficit credit so a throttled slot
        // does not save up unused quantum across the period the cap
        // held it back. When `rateBps == 0` the bucket is disabled
        // and the gate is a no-op.
        Ptr<const QueueDiscItem> head = inner->Peek();
        if (!head)
        {
            // Path-gamma null-peek skip: the inner refused (e.g. TBF
            // tokens exhausted) — let DRR walk continue to the next
            // candidate rather than parking on an unservable slot.
            continue;
        }
        if (!m_tokenBuckets[i].HasTokensFor(head->GetSize(), Simulator::Now()))
        {
            continue;
        }
        if (m_deficit[i] <= 0)
        {
            m_deficit[i] += static_cast<int32_t>(m_quantum[i]);
        }
        m_drrCursor = i;
        return static_cast<int32_t>(i);
    }
    return -1;
}

int32_t
HybridLlqDispatcher::PeekSlot(EdgeQueueDisc* edge)
{
    NS_ASSERT_MSG(edge, "HybridLlqDispatcher::PeekSlot: edge must be non-null");

    // SP walk is naturally pure — same code path as Select.
    const int32_t spSlot = FirstReadyStrictPrioritySlot(edge);
    if (spSlot >= 0)
    {
        return spSlot;
    }

    // DRR look-ahead — first non-empty non-SP slot from the cursor is
    // what a subsequent SelectDequeueSlot would serve (after crediting
    // if the deficit has exhausted). The class member m_deficit is NOT
    // mutated; this is the side-effect-free contract enforced by
    // .
    const uint32_t kMax = EdgeQueueDisc::kMaxInnerSlots;
    for (uint32_t tries = 0; tries < kMax; ++tries)
    {
        const uint32_t i = (m_drrCursor + tries) % kMax;
        if (m_isStrictPriority[i])
        {
            continue;
        }
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(i);
        if (!inner || inner->GetNPackets() == 0)
        {
            continue;
        }
        // Mirror the SelectDequeueSlot token gate so peek and select
        // agree on which slot would actually serve next under a rate
        // cap. HasTokensFor is `const` (no bucket mutation) — preserves
        // 's side-effect-free contract.
        Ptr<const QueueDiscItem> head = inner->Peek();
        if (!head)
        {
            // Path-gamma null-peek skip — keep peek and select agreeing.
            continue;
        }
        if (!m_tokenBuckets[i].HasTokensFor(head->GetSize(), Simulator::Now()))
        {
            continue;
        }
        return static_cast<int32_t>(i);
    }
    return -1;
}

void
HybridLlqDispatcher::OnEnqueue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* /*edge*/)
{
    if (item && slot < EdgeQueueDisc::kMaxInnerSlots)
    {
        m_bytesEnqueued[slot] += item->GetSize();
    }
}

void
HybridLlqDispatcher::OnDequeue(uint32_t slot, Ptr<QueueDiscItem> item, EdgeQueueDisc* edge)
{
    NS_ASSERT_MSG(item, "HybridLlqDispatcher::OnDequeue: item must be non-null");
    NS_ASSERT_MSG(edge, "HybridLlqDispatcher::OnDequeue: edge must be non-null");
    NS_ASSERT_MSG(slot < EdgeQueueDisc::kMaxInnerSlots,
                  "HybridLlqDispatcher::OnDequeue: slot " << slot << " out of range");

    // S-17.32 — track wire-bytes drained for both SP and DRR slots.
    m_bytesDequeued[slot] += item->GetSize();

    // Charge the token bucket FIRST, uniformly for SP and DRR slots.
    // When `rateBps == 0` the bucket is disabled and Charge is a
    // no-op. Charging on SP slots is what makes the Cisco MQC LLQ
    // pattern work — a priority class served ahead of DRR but capped
    // to its configured ceiling.
    m_tokenBuckets[slot].Charge(item->GetSize(), Simulator::Now());

    // SP slot: no deficit accounting beyond the token charge. The
    // cursor stays where DRR last left it; the next SP-walk on a future
    // enqueue picks up correctly.
    if (m_isStrictPriority[slot])
    {
        return;
    }

    // DRR slot: identical bookkeeping to cake::TinShaperDispatcher.
    m_deficit[slot] -= static_cast<int32_t>(item->GetSize());

    Ptr<QueueDisc> inner = edge->GetInnerDiscAt(slot);
    const uint32_t kMax = EdgeQueueDisc::kMaxInnerSlots;
    if (!inner || inner->GetNPackets() == 0)
    {
        // Drain-to-empty: reset deficit so an idle tin does not
        // accrue save-up credit across the quiet period. Leave the
        // cursor on this slot — when work returns it (the typical
        // single-active-tin case under TCP), the next
        // SelectDequeueSlot starts here rather than walking through
        // every empty slot first.
        m_deficit[slot] = 0;
        return;
    }
    if (m_deficit[slot] <= 0)
    {
        // Round complete for this slot — yield the cursor so other DRR
        // slots get a turn before this one is re-credited. The deficit
        // is intentionally left non-positive; the next visit will add
        // quantum and restore eligibility.
        m_drrCursor = (slot + 1) % kMax;
    }
}

cake::TinStats
HybridLlqDispatcher::GetTinStats(uint32_t tinIdx, const EdgeQueueDisc* edge) const
{
    cake::TinStats out{};
    if (tinIdx >= EdgeQueueDisc::kMaxInnerSlots)
    {
        return out;
    }
    out.bytesEnqueued = m_bytesEnqueued[tinIdx];
    out.bytesDequeued = m_bytesDequeued[tinIdx];

    if (edge)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(tinIdx);
        if (inner)
        {
            const auto& s = inner->GetStats();
            out.drops = s.nTotalDroppedPackets;
            out.marks = s.nTotalMarkedPackets;
        }
    }
    return out;
}

} // namespace ns3::stratum
