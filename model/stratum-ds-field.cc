/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-ds-field.h"

#include "ns3/ipv4-header.h"
#include "ns3/ipv4-queue-disc-item.h"
#include "ns3/ipv6-header.h"
#include "ns3/ipv6-queue-disc-item.h"
#include "ns3/queue-disc.h"
#include "ns3/queue-item.h"

namespace ns3::stratum
{

bool
GetDsField(Ptr<const QueueDiscItem> item, uint8_t& dsField)
{
    if (!item)
    {
        return false;
    }
    return item->GetUint8Value(QueueItem::IP_DSFIELD, dsField);
}

bool
GetDscp(Ptr<const QueueDiscItem> item, uint8_t& dscp)
{
    uint8_t dsField = 0;
    if (!GetDsField(item, dsField))
    {
        return false;
    }
    dscp = dsField >> 2;
    return true;
}

bool
GetEcn(Ptr<const QueueDiscItem> item, uint8_t& ecn)
{
    uint8_t dsField = 0;
    if (!GetDsField(item, dsField))
    {
        return false;
    }
    ecn = dsField & 0x3;
    return true;
}

bool
SetDscpPreservingEcn(Ptr<QueueDiscItem> item, uint8_t dscp)
{
    if (auto v4 = DynamicCast<Ipv4QueueDiscItem>(item))
    {
        const_cast<Ipv4Header&>(v4->GetHeader()).SetDscp(Ipv4Header::DscpType(dscp));
        return true;
    }
    if (auto v6 = DynamicCast<Ipv6QueueDiscItem>(item))
    {
        const_cast<Ipv6Header&>(v6->GetHeader()).SetDscp(Ipv6Header::DscpType(dscp));
        return true;
    }
    return false;
}

bool
GetL3Source(Ptr<const QueueDiscItem> item, Address& out)
{
    if (auto v4 = DynamicCast<const Ipv4QueueDiscItem>(item))
    {
        out = v4->GetHeader().GetSource();
        return true;
    }
    if (auto v6 = DynamicCast<const Ipv6QueueDiscItem>(item))
    {
        out = v6->GetHeader().GetSource();
        return true;
    }
    return false;
}

bool
GetL3Destination(Ptr<const QueueDiscItem> item, Address& out)
{
    if (auto v4 = DynamicCast<const Ipv4QueueDiscItem>(item))
    {
        out = v4->GetHeader().GetDestination();
        return true;
    }
    if (auto v6 = DynamicCast<const Ipv6QueueDiscItem>(item))
    {
        out = v6->GetHeader().GetDestination();
        return true;
    }
    return false;
}

} // namespace ns3::stratum
