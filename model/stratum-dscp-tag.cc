/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-dscp-tag.h"

#include "ns3/log.h"
#include "ns3/type-id.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::DscpTag");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(DscpTag);

TypeId
DscpTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::DscpTag")
                            .SetParent<Tag>()
                            .SetGroupName("Stratum")
                            .AddConstructor<DscpTag>();
    return tid;
}

TypeId
DscpTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

DscpTag::DscpTag()
    : Tag(),
      m_dscp(0)
{
    NS_LOG_FUNCTION(this);
}

DscpTag::DscpTag(uint8_t dscp)
    : Tag(),
      m_dscp(dscp)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp));
}

void
DscpTag::SetDscp(uint8_t dscp)
{
    NS_LOG_FUNCTION(this << static_cast<uint32_t>(dscp));
    m_dscp = dscp;
}

uint8_t
DscpTag::GetDscp() const
{
    NS_LOG_FUNCTION(this);
    return m_dscp;
}

uint32_t
DscpTag::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    return 1;
}

void
DscpTag::Serialize(TagBuffer i) const
{
    NS_LOG_FUNCTION(this << &i);
    i.WriteU8(m_dscp);
}

void
DscpTag::Deserialize(TagBuffer i)
{
    NS_LOG_FUNCTION(this << &i);
    m_dscp = i.ReadU8();
}

void
DscpTag::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    os << "DiffServDscp=" << static_cast<uint32_t>(m_dscp);
}

} // namespace ns3::stratum
