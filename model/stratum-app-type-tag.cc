/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-app-type-tag.h"

#include "ns3/log.h"
#include "ns3/type-id.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::AppTypeTag");

} // namespace ns3

namespace ns3::stratum
{

NS_OBJECT_ENSURE_REGISTERED(AppTypeTag);

TypeId
AppTypeTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::AppTypeTag")
                            .SetParent<Tag>()
                            .SetGroupName("Stratum")
                            .AddConstructor<AppTypeTag>();
    return tid;
}

TypeId
AppTypeTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

AppTypeTag::AppTypeTag()
    : Tag(),
      m_appType(kAnyAppType)
{
    NS_LOG_FUNCTION(this);
}

AppTypeTag::AppTypeTag(uint32_t appType)
    : Tag(),
      m_appType(appType)
{
    NS_LOG_FUNCTION(this << appType);
}

void
AppTypeTag::SetAppType(uint32_t appType)
{
    NS_LOG_FUNCTION(this << appType);
    m_appType = appType;
}

uint32_t
AppTypeTag::GetAppType() const
{
    NS_LOG_FUNCTION(this);
    return m_appType;
}

uint32_t
AppTypeTag::GetSerializedSize() const
{
    NS_LOG_FUNCTION(this);
    return 4;
}

void
AppTypeTag::Serialize(TagBuffer i) const
{
    NS_LOG_FUNCTION(this << &i);
    i.WriteU32(m_appType);
}

void
AppTypeTag::Deserialize(TagBuffer i)
{
    NS_LOG_FUNCTION(this << &i);
    m_appType = i.ReadU32();
}

void
AppTypeTag::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this << &os);
    os << "DiffServAppType=" << m_appType;
}

} // namespace ns3::stratum
