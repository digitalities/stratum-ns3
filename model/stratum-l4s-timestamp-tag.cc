/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-l4s-timestamp-tag.h"

namespace ns3::stratum::l4s
{

NS_OBJECT_ENSURE_REGISTERED(TimestampTag);

TypeId
TimestampTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::l4s::TimestampTag")
                            .SetParent<Tag>()
                            .SetGroupName("Stratum")
                            .AddConstructor<TimestampTag>();
    return tid;
}

TypeId
TimestampTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

TimestampTag::TimestampTag() = default;

TimestampTag::TimestampTag(Time t)
    : m_timestamp(t)
{
}

void
TimestampTag::Serialize(TagBuffer i) const
{
    i.WriteU64(m_timestamp.GetTimeStep());
}

void
TimestampTag::Deserialize(TagBuffer i)
{
    m_timestamp = TimeStep(i.ReadU64());
}

uint32_t
TimestampTag::GetSerializedSize() const
{
    return sizeof(uint64_t);
}

void
TimestampTag::Print(std::ostream& os) const
{
    os << "ts=" << m_timestamp.As(Time::S);
}

Time
TimestampTag::GetTimestamp() const
{
    return m_timestamp;
}

void
TimestampTag::SetTimestamp(Time t)
{
    m_timestamp = t;
}

} // namespace ns3::stratum::l4s
