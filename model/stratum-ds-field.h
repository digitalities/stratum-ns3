/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_DS_FIELD_H
#define NS3_STRATUM_DS_FIELD_H

#include "ns3/address.h"
#include "ns3/ptr.h"
#include "ns3/queue-item.h"

namespace ns3
{
class QueueDiscItem;
} // namespace ns3

namespace ns3::stratum
{

/**
 * @ingroup stratum
 * @defgroup stratum-ds-field DS-field accessors
 *
 * Family-agnostic helpers for reading and writing the DS (Differentiated
 * Services) octet of a QueueDiscItem, whether IPv4 or IPv6.
 *
 * Read path — delegates to the per-family `GetUint8Value(IP_DSFIELD)` virtual,
 * which is already overridden by `Ipv4QueueDiscItem` and `Ipv6QueueDiscItem`.
 * All read helpers return `false` for null or non-IP items (no DS field).
 *
 * Write path — `SetDscpPreservingEcn` uses the structured `Ipv4Header::SetDscp`
 * / `Ipv6Header::SetDscp` API which preserves the 2 ECN bits (`m_tos &= 0x3;
 * m_tos |= dscp<<2` for v4; symmetric for v6).  For IPv4 this is bit-identical
 * to the legacy edge arithmetic `SetTos((dscp<<2)|(GetTos()&0x3))`.
 *
 * @{
 */

/**
 * @brief Read the full 8-bit DS octet (DSCP[7:2] | ECN[1:0]).
 *
 * Wraps `item->GetUint8Value(QueueItem::IP_DSFIELD, dsField)`.
 *
 * @param[in]  item    The queue disc item to inspect.
 * @param[out] dsField Receives the DS octet on success.
 * @return `true` if the item carries an IP header and the DS octet was read;
 *         `false` for null or non-IP items.
 */
bool GetDsField(Ptr<const QueueDiscItem> item, uint8_t& dsField);

/**
 * @brief Read the 6-bit DSCP value (DS octet >> 2).
 *
 * @param[in]  item The queue disc item to inspect.
 * @param[out] dscp Receives the 6-bit DSCP on success.
 * @return `true` on success; `false` for null or non-IP items.
 */
bool GetDscp(Ptr<const QueueDiscItem> item, uint8_t& dscp);

/**
 * @brief Read the 2-bit ECN field (DS octet & 0x3).
 *
 * The raw 2-bit value maps to the ECN codepoints as follows:
 *   - `0x00` — Not-ECT
 *   - `0x01` — ECT(1)  (L4S-capable transport)
 *   - `0x02` — ECT(0)  (classic ECN-capable transport, not L4S)
 *   - `0x03` — CE      (Congestion Experienced)
 *
 * @param[in]  item The queue disc item to inspect.
 * @param[out] ecn  Receives the 2-bit ECN field on success.
 * @return `true` on success; `false` for null or non-IP items.
 */
bool GetEcn(Ptr<const QueueDiscItem> item, uint8_t& ecn);

/**
 * @brief Write a new DSCP value while preserving the existing 2 ECN bits.
 *
 * For IPv4 items calls `Ipv4Header::SetDscp(DscpType(dscp))`, which performs
 * `m_tos &= 0x3; m_tos |= dscp<<2`.  This is bit-identical to the legacy
 * edge-queue-disc arithmetic `SetTos((dscp<<2)|(GetTos()&0x3))`.
 *
 * For IPv6 items calls `Ipv6Header::SetDscp(DscpType(dscp))`, which performs
 * `m_trafficClass &= 0x3; m_trafficClass |= dscp<<2` — ECN bits preserved.
 *
 * Returns `false` (no-op) for non-IP items; never calls `SetTrafficClass`
 * (which would clobber the ECN bits).
 *
 * @param item The queue disc item to modify.
 * @param dscp The 6-bit DSCP value to write.
 * @return `true` if the header was updated; `false` for non-IP items.
 */
bool SetDscpPreservingEcn(Ptr<QueueDiscItem> item, uint8_t dscp);

/**
 * @brief Extract the layer-3 source address into a family-agnostic `Address`.
 *
 * For IPv4 items the `Ipv4Address` is wrapped via `Ipv4Address::ConvertTo()`.
 * For IPv6 items the `Ipv6Address` is wrapped via `Ipv6Address::ConvertTo()`.
 *
 * @param[in]  item The queue disc item to inspect.
 * @param[out] out  Receives the source address on success.
 * @return `true` on success; `false` for null or non-IP items.
 */
bool GetL3Source(Ptr<const QueueDiscItem> item, Address& out);

/**
 * @brief Extract the layer-3 destination address into a family-agnostic `Address`.
 *
 * For IPv4 items the `Ipv4Address` is wrapped via `Ipv4Address::ConvertTo()`.
 * For IPv6 items the `Ipv6Address` is wrapped via `Ipv6Address::ConvertTo()`.
 *
 * @param[in]  item The queue disc item to inspect.
 * @param[out] out  Receives the destination address on success.
 * @return `true` on success; `false` for null or non-IP items.
 */
bool GetL3Destination(Ptr<const QueueDiscItem> item, Address& out);

/** @} */

} // namespace ns3::stratum

#endif /* NS3_STRATUM_DS_FIELD_H */
