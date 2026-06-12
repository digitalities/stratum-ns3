/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Text-format diagnostic dump for a CAKE-composed
 * `EdgeQueueDisc` — produces output that mirrors
 * Linux `tc -s qdisc show cake` (iproute2 `q_cake.c::cake_print_xstats`)
 * within the limits documented in the user handbook.
 *
 * Adapted ns-3 traffic-control patterns originally contributed by
 * Pasquale Imputato and Stefano Avallone.
 */

#ifndef NS3_STRATUM_CAKE_STATS_FORMATTER_H
#define NS3_STRATUM_CAKE_STATS_FORMATTER_H

#include "ns3/ptr.h"
#include "ns3/queue-disc.h"
#include "ns3/stratum-edge-queue-disc.h"

#include <ostream>

namespace ns3::stratum::cake
{

/**
 * @ingroup stratum
 *
 * @brief Emit a `tc -s qdisc show cake`-equivalent text dump for a
 * CAKE-composed `EdgeQueueDisc`.
 *
 * The formatter walks the edge's populated inner slots (CAKE tins),
 * pulls per-tin counters via `EdgeQueueDisc::GetTinStats(slot)`
 * and per-flow counters via `TinShaperDispatcher::GetPerFlowStats`
 * when the dispatcher is the tin-shaping kind, and writes a Linux-
 * compatible report to @p os.
 *
 * **Substrate fidelity gap.** The bulk-flow counter is reported as
 * `ever_seen=N` rather than the Linux name `bulk_flow_count`. The
 * upstream ns-3 `FqCobaltQueueDisc` exposes only an append-only
 * class list (`GetNQueueDiscClasses()`), counting flows ever observed
 * in the bucket — not the live `bulk_flow_count` Linux's `sch_cake`
 * (provenance: `sch_cake.c @ 67dc6c56b871`,
 * provenance/linux-sch-cake-67dc6c56b871/sch_cake.c) tracks. The same gap affects per-host
 * `flowsActive` (S-17.48) and outer-DRR quantum modulation (S-17.38). See the user handbook's CAKE
 * chapter "Diagnostic output" subsection.
 *
 * **Non-CAKE compositions.** When the edge is not CAKE-composed the
 * formatter emits a degenerate header with `Sent` / `dropped` /
 * `backlog` lines from `QueueDisc::Stats` and a single empty per-tin
 * block — matching Linux's behaviour when querying a non-CAKE qdisc
 * with `cake_print_xstats`.
 *
 * The output is structural, not byte-exact. The fixture S-17.51
 * asserts presence of a defined section-key set rather than a literal
 * byte-for-byte transcript, so future iproute2 cosmetic changes do
 * not regress the fixture.
 *
 * @see specs/02-structural.md S-17.51
 */
class StatsFormatter
{
  public:
    /**
     * @brief Write a `tc -s qdisc show cake`-equivalent text dump for
     * @p edge to @p os.
     *
     * Safe to call at any point after `Initialize()`. Reads no
     * dispatcher or inner-disc state that mutates between
     * `OnEnqueue` / `OnDequeue` callbacks; each call captures a
     * point-in-time snapshot.
     *
     * @param os destination stream (may be `std::cout`, a
     *        `std::ostringstream`, or a file stream)
     * @param edge the CAKE-composed edge queue disc to introspect
     *        (nullable; null yields a single-line "qdisc cake (null)"
     *        diagnostic and returns)
     */
    static void Print(std::ostream& os, Ptr<const QueueDisc> edge);
};

} // namespace ns3::stratum::cake

#endif // NS3_STRATUM_CAKE_STATS_FORMATTER_H
