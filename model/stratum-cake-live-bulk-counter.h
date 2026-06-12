/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_CAKE_LIVE_BULK_COUNTER_H
#define NS3_STRATUM_CAKE_LIVE_BULK_COUNTER_H

#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/queue-disc.h"

#include <cstdint>
#include <unordered_map>

namespace ns3::stratum::cake
{

/**
 * @ingroup stratum
 *
 * @brief Substrate wrapper that owns a live bulk-flow count for one
 *        inner FqCobaltQueueDisc.
 *
 * Connects to the inner disc's `Enqueue` trace source at attach time.
 * On every enqueue the 5-tuple is hashed and the per-flow `lastSeen`
 * timestamp is updated. The live bulk count is the number of flows
 * whose `lastSeen + idleWindow > Now()`.
 *
 * Replaces the `ever_seen` fidelity gap in `StatsFormatter`:
 * the append-only mainline class list inflates the bulk count under
 * churn; this wrapper exposes a live count instead.
 *
 * Opt-in: call `Helper::AttachLiveBulkCounter(edge)` after
 * `SetAsCake*` and after `Initialize`. Read via
 * `Helper::GetLiveBulkCount(edge, slot)`.
 *
 * @see hoiland-jorgensen2018cake §3.3 "Flow Isolation"
 * @see provenance/linux-sch-cake-67dc6c56b871/sch_cake.c::cake_dump_stats
 */
class LiveBulkCounter : public Object
{
  public:
    /**
     * @brief Register this class with the ns-3 object system.
     * @return the TypeId for LiveBulkCounter
     */
    static TypeId GetTypeId();

    LiveBulkCounter();
    ~LiveBulkCounter() override = default;

    /**
     * @brief Attach to @p inner's Enqueue trace and begin counting.
     *
     * @param inner the per-tin FqCobaltQueueDisc to instrument;
     *        must be non-null
     * @param idleWindow flows idle for longer than this window drop out
     *        of the live count; pass `Time(0)` (the default) to
     *        auto-derive from `inner`'s `Interval` attribute
     *        (`Interval × 8`, matching Linux `bulk_flow_threshold`)
     */
    void Attach(Ptr<QueueDisc> inner, Time idleWindow = Time(0));

    /**
     * @brief Return the live bulk-flow count, evicting stale flows as
     *        a side effect.
     *
     * @param now current simulation time (typically `Simulator::Now()`)
     * @return number of flows whose lastSeen + idleWindow > @p now
     */
    uint32_t GetLiveCount(Time now);

  private:
    void OnEnqueueTrace(Ptr<const QueueDiscItem> item);
    static uint64_t FlowHashFromItem(Ptr<const QueueDiscItem> item);

    std::unordered_map<uint64_t, Time> m_lastSeen;
    Time m_idleWindow{Time(0)};
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_CAKE_LIVE_BULK_COUNTER_H
