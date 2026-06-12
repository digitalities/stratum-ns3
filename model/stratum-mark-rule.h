/*
 * Copyright (C) 2001-2026 Sergio Andreozzi
 * Copyright (C) 2000 Nortel Networks
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Port of ns-2 edgeQueue::tMarkRuleTable from dsEdge.h (2001).
 */

#ifndef NS3_STRATUM_MARK_RULE_H
#define NS3_STRATUM_MARK_RULE_H

#include "stratum-constants.h"

#include <cstdint>

namespace ns3::stratum
{

/** Wildcard value for port fields (matches any port). */
static constexpr uint16_t kAnyPort = 0;

/**
 * @ingroup stratum
 *
 * @brief A multi-field classification rule for the DiffServ edge router.
 *
 * Maps packets matching (srcAddr, dstAddr, protocol, appType, srcPort,
 * dstPort) to an initial DSCP. Port of ns-2 edgeQueue::tMarkRuleTable,
 * extended with transport-layer port matching per RFC 2475 §2.3.1.
 *
 * Wildcards: kAnyHost for addresses, kAnyProtocol for protocol,
 * 0 for application type (kAnyAppType), kAnyPort for ports.
 *
 * @see src/ns-2.29/diffserv/dsEdge.h tMarkRuleTable
 */
struct MarkRule
{
    uint8_t dscp{0};                //!< Initial DSCP to assign on match
    int32_t srcAddr{kAnyHost};      //!< Source address (-1 = any)
    int32_t dstAddr{kAnyHost};      //!< Destination address (-1 = any)
    uint8_t protocol{kAnyProtocol}; //!< IP protocol number (0 = any, 6 = TCP, 17 = UDP)
    uint32_t appType{0};            //!< Application type (0 = any, matches AppTypeTag)
    uint16_t srcPort{kAnyPort};     //!< Source port (0 = any)
    uint16_t dstPort{kAnyPort};     //!< Destination port (0 = any)
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_MARK_RULE_H
