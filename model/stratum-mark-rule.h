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

#include "ns3/address.h"
#include "ns3/ipv4-address.h"
#include "ns3/ipv6-address.h"

#include <cstdint>

namespace ns3::stratum
{

/** Wildcard value for port fields (matches any port). */
static constexpr uint16_t kAnyPort = 0;

/**
 * @ingroup stratum
 *
 * @brief Family-tagged address matcher for multi-field classification rules.
 *
 * Holds a source or destination address as a tagged union (Any, V4, V6)
 * and tests whether a packet's layer-3 address matches the rule.
 *
 * - `Any` (default-constructed): wildcard — matches every address.
 * - `V4`: exact IPv4 match against the stored `v4` address.
 * - `V6`: exact IPv6 match against the stored `v6` address.
 *
 * Construct with a typed address or leave default for a wildcard:
 *
 * @code
 * AddrMatch any;                              // wildcard
 * AddrMatch src(Ipv4Address("10.0.0.1"));     // exact IPv4
 * AddrMatch dst(Ipv6Address("2001:db8::1"));  // exact IPv6
 * @endcode
 */
struct AddrMatch
{
    /** Address family tag. */
    enum Family
    {
        Any, //!< Wildcard — matches every address
        V4,  //!< IPv4 exact match
        V6   //!< IPv6 exact match
    };

    Family family{Any};     //!< Which family / wildcard
    Ipv4Address v4;         //!< IPv4 address (used when family == V4)
    Ipv6Address v6;         //!< IPv6 address (used when family == V6)
    uint8_t prefixLen{255}; //!< Reserved. Matches() ignores this field today; any
                            //!< value other than 255 is silently treated as exact match.

    /** Default constructor: wildcard (Any). */
    AddrMatch() = default;

    /**
     * @brief Construct an exact IPv4 matcher.
     *
     * Implicitly converts an `Ipv4Address` to an `AddrMatch` with
     * `family == V4`. Pass a default-constructed `AddrMatch` (`{}`) for
     * wildcard matching instead.
     *
     * @param a the IPv4 address to match exactly
     */
    AddrMatch(Ipv4Address a) // NOLINT(google-explicit-constructor)
        : family(V4),
          v4(a),
          prefixLen(255)
    {
    }

    /**
     * @brief Construct an exact IPv6 matcher.
     *
     * @param a the IPv6 address to match exactly
     */
    AddrMatch(Ipv6Address a) // NOLINT(google-explicit-constructor)
        : family(V6),
          v6(a),
          prefixLen(255)
    {
    }

    /**
     * @brief Test whether `l3addr` matches this rule.
     *
     * Only exact matching is currently implemented. `prefixLen` is reserved
     * and not consulted here; prefix masking is a future task. Until then,
     * setting `prefixLen` to anything other than 255 has no effect — the match
     * stays exact.
     *
     * @param l3addr  The layer-3 source or destination address extracted from
     *                the packet (via `stratum::GetL3Source` /
     *                `stratum::GetL3Destination`), wrapped as an `ns3::Address`.
     * @return `true` if the address matches (or this is a wildcard).
     */
    bool Matches(const Address& l3addr) const
    {
        switch (family)
        {
        case Any:
            return true;
        case V4:
            return Ipv4Address::IsMatchingType(l3addr) && v4 == Ipv4Address::ConvertFrom(l3addr);
        case V6:
            return Ipv6Address::IsMatchingType(l3addr) && v6 == Ipv6Address::ConvertFrom(l3addr);
        }
        return false;
    }
};

/**
 * @ingroup stratum
 *
 * @brief A multi-field classification rule for the DiffServ edge router.
 *
 * Maps packets matching (srcAddr, dstAddr, protocol, srcPort, dstPort) to an
 * initial DSCP. Port of ns-2 edgeQueue::tMarkRuleTable, extended with
 * transport-layer port matching per RFC 2475 §2.3.1.
 *
 * Every field defaults to a wildcard, so only the fields that matter need to
 * appear in a C++23 designated-initializer construction:
 *
 * @code
 * // DSCP 46 for all traffic from a specific source host:
 * edge->AddMarkRule({.dscp = 46, .srcAddr = srcAddr});
 *
 * // DSCP 10 for all TCP traffic to port 5004:
 * edge->AddMarkRule({.dscp = 10, .protocol = 6, .dstPort = 5004});
 *
 * // DSCP 0 for all traffic (catch-all rule):
 * edge->AddMarkRule({.dscp = 0});
 * @endcode
 *
 * Field declaration order is load-bearing for designated initializers —
 * do not reorder without updating every call site.
 *
 * @see src/ns-2.29/diffserv/dsEdge.h tMarkRuleTable
 */
struct MarkRule
{
    uint8_t dscp{0};                //!< Initial DSCP to assign on match
    AddrMatch srcAddr;              //!< Source address matcher (default = Any)
    AddrMatch dstAddr;              //!< Destination address matcher (default = Any)
    uint8_t protocol{kAnyProtocol}; //!< IP protocol number (0 = any, 6 = TCP, 17 = UDP)
    uint16_t srcPort{kAnyPort};     //!< Source port (0 = any)
    uint16_t dstPort{kAnyPort};     //!< Destination port (0 = any)
};

} // namespace ns3::stratum

#endif // NS3_STRATUM_MARK_RULE_H
