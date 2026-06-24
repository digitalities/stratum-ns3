/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef NS3_STRATUM_L4S_HELPER_H
#define NS3_STRATUM_L4S_HELPER_H

#include "ns3/ptr.h"
#include "ns3/stratum-l4s-queue-disc.h"

#include <cstdint>

namespace ns3::stratum::l4s
{

/// Declarative specification for SetAsL4s.
struct L4sSpec
{
    uint32_t l4sQueueIdx = 0;  //!< Index of the low-latency L4S lane.
    uint32_t burstCap = 8;     //!< Coupled-scheduler burst allowance.
    uint32_t queueLimit = 200; //!< Per-lane queue limit, packets.
};

/// One-call composer for the native L4S edge.
class Helper
{
  public:
    /**
     * @brief Compose @p disc as a two-lane L4S edge in one call.
     *
     * Sizes the L4S and classic lanes, writes the DSCP fallback PHB, attaches a
     * coupled scheduler, and applies the per-lane RED tuning. Mutates @p disc in
     * place; the caller initialises and installs it.
     *
     * @param disc native L4S queue disc to compose.
     * @param spec lane index, burst allowance, and queue limit.
     */
    static void SetAsL4s(Ptr<QueueDisc> disc, const L4sSpec& spec = {});
};

} // namespace ns3::stratum::l4s

#endif // NS3_STRATUM_L4S_HELPER_H
