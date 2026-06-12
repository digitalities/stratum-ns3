/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-onoff-helper.h"

#include "ns3/node.h"
#include "ns3/stratum-onoff-application.h"

namespace ns3::stratum
{

OnOffHelper::OnOffHelper(const Address& remote)
{
    m_factory.SetTypeId(OnOffApplication::GetTypeId());
    m_factory.Set("Remote", AddressValue(remote));
}

void
OnOffHelper::SetAttribute(const std::string& name, const AttributeValue& value)
{
    m_factory.Set(name, value);
}

Ptr<Application>
OnOffHelper::InstallOn(Ptr<Node> node) const
{
    Ptr<Application> app = m_factory.Create<Application>();
    node->AddApplication(app);
    return app;
}

ApplicationContainer
OnOffHelper::Install(Ptr<Node> node) const
{
    ApplicationContainer apps;
    apps.Add(InstallOn(node));
    return apps;
}

ApplicationContainer
OnOffHelper::Install(NodeContainer nodes) const
{
    ApplicationContainer apps;
    for (auto it = nodes.Begin(); it != nodes.End(); ++it)
    {
        apps.Add(InstallOn(*it));
    }
    return apps;
}

} // namespace ns3::stratum
