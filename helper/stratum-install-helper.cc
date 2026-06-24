/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-install-helper.h"

#include "ns3/assert.h"
#include "ns3/node.h"
#include "ns3/traffic-control-layer.h"

namespace ns3::stratum
{

Ptr<QueueDisc>
InstallRoot(Ptr<NetDevice> device, Ptr<QueueDisc> disc)
{
    Ptr<TrafficControlLayer> tc = device->GetNode()->GetObject<TrafficControlLayer>();
    NS_ASSERT_MSG(tc, "InstallRoot: TrafficControlLayer must be installed on the node");
    // The internet stack attaches a default root qdisc per device; remove it
    // before installing the pre-built disc so the set does not collide with an
    // existing root. The Get probe is required because the delete asserts when
    // no root qdisc is present.
    if (tc->GetRootQueueDiscOnDevice(device))
    {
        tc->DeleteRootQueueDiscOnDevice(device);
    }
    tc->SetRootQueueDiscOnDevice(device, disc);
    return disc;
}

} // namespace ns3::stratum
