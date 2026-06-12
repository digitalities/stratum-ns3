/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-send-time-tag.h"

#include "ns3/log.h"
#include "ns3/type-id.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::SendTimeTag");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(SendTimeTag);

TypeId
SendTimeTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::SendTimeTag")
                            .SetParent<Tag>()
                            .SetGroupName("Stratum")
                            .AddConstructor<SendTimeTag>();
    return tid;
}

TypeId
SendTimeTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

SendTimeTag::SendTimeTag()
    : Tag(),
      m_sendTime(0.0)
{
}

SendTimeTag::SendTimeTag(double sendTimeSeconds)
    : Tag(),
      m_sendTime(sendTimeSeconds)
{
}

void
SendTimeTag::SetSendTime(double sendTimeSeconds)
{
    m_sendTime = sendTimeSeconds;
}

double
SendTimeTag::GetSendTime() const
{
    return m_sendTime;
}

uint32_t
SendTimeTag::GetSerializedSize() const
{
    return 8; // sizeof(double)
}

void
SendTimeTag::Serialize(TagBuffer i) const
{
    i.WriteDouble(m_sendTime);
}

void
SendTimeTag::Deserialize(TagBuffer i)
{
    m_sendTime = i.ReadDouble();
}

void
SendTimeTag::Print(std::ostream& os) const
{
    os << "DiffServSendTime=" << m_sendTime << "s";
}

} // namespace ns3::stratum
