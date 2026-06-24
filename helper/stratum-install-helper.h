/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef NS3_STRATUM_INSTALL_HELPER_H
#define NS3_STRATUM_INSTALL_HELPER_H

#include "ns3/net-device.h"
#include "ns3/ptr.h"
#include "ns3/queue-disc.h"

namespace ns3::stratum
{

/**
 * @brief Install @p disc as @p device's root queue disc, first removing any
 *        default qdisc that InternetStackHelper attached.
 *
 * The pre-built-disc analogue of TrafficControlHelper::Install, which accepts
 * only a TypeId plus attribute strings. Safe whether or not a default root
 * is already present: if the device already carries one it is deleted before
 * @p disc is set, so the call works whether or not the internet stack ran
 * first.
 *
 * @param device target NetDevice (must have a TrafficControlLayer aggregated)
 * @param disc fully-built root queue disc
 * @return @p disc, for call-site chaining
 */
Ptr<QueueDisc> InstallRoot(Ptr<NetDevice> device, Ptr<QueueDisc> disc);

} // namespace ns3::stratum

#endif // NS3_STRATUM_INSTALL_HELPER_H
