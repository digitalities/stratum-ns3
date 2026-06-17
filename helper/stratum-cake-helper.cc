/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "stratum-cake-helper.h"

#include "../model/stratum-cake-linux-autorate-hook.h"
#include "../model/stratum-cake-live-bulk-counter.h"
#include "../model/stratum-rate-based-shaper-dispatcher.h"
#include "stratum-cake-stats-formatter.h"

#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/enum.h"
#include "ns3/fq-cobalt-queue-disc.h"
#include "ns3/log.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/queue-disc.h"
#include "ns3/simulator.h"
#include "ns3/stratum-hybrid-llq-dispatcher.h"
#include "ns3/stratum-l4s-coupled-scheduler.h"
#include "ns3/stratum-l4s-queue-disc.h"
#include "ns3/stratum-shaped-tin-dispatcher.h"
#include "ns3/stratum-tin-shaper-dispatcher.h"
#include "ns3/string.h"
#include "ns3/tbf-queue-disc.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace ns3::stratum::cake
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::cake::Helper");

NS_OBJECT_ENSURE_REGISTERED(ProfileMarker);

TypeId
ProfileMarker::GetTypeId()
{
    static TypeId tid = TypeId("ns3::stratum::cake::ProfileMarker")
                            .SetParent<Object>()
                            .SetGroupName("Stratum");
    return tid;
}

namespace
{

/**
 * @brief Record @p profile on @p edge (aggregate a fresh marker or
 *        re-stamp the existing one — the last composer wins).
 */
void
StampProfile(Ptr<EdgeQueueDisc> edge, Helper::Profile profile)
{
    Ptr<ProfileMarker> marker = edge->GetObject<ProfileMarker>();
    if (!marker)
    {
        marker = CreateObject<ProfileMarker>();
        edge->AggregateObject(marker);
    }
    marker->SetProfile(profile);
}

} // namespace

void
Helper::SetShaperMode(Helper::ShaperMode mode)
{
    m_shaperMode = mode;
}

Helper::ShaperMode
Helper::GetShaperMode() const
{
    return m_shaperMode;
}

void
Helper::SetUseInnerTbfShaping(bool enable)
{
    if (enable)
    {
        m_shaperMode = ShaperMode::TbfInner;
    }
    else if (m_shaperMode == ShaperMode::TbfInner)
    {
        m_shaperMode = ShaperMode::TokenBucket;
    }
    // RateBased is preserved unchanged when enable=false.
}

void
Helper::SetGlobalRateBps(uint64_t rateBps)
{
    m_globalRateBps = rateBps;
}

void
Helper::SetTinRateBpsAll(uint64_t rateBps)
{
    m_uniformTinRateBps = rateBps;
}

void
Helper::SetTinRateBps(uint32_t slot, uint64_t rateBps)
{
    NS_ASSERT_MSG(slot < m_tinCount,
                  "SetTinRateBps: slot " << slot << " out of range for tin count " << m_tinCount);
    m_tinRateOverride[slot] = rateBps;
}

void
Helper::SetTinCount(uint32_t n)
{
    m_tinCount = n;
}

void
Helper::SetEnableAutorateIngress(bool enable)
{
    m_enableAutorateIngress = enable;
    if (enable)
    {
        if (m_autorateImpl == AutorateImpl::Linux)
        {
            m_autorateHook = std::make_shared<LinuxAutorateHook>();
        }
        else
        {
            m_autorateHook = std::make_shared<NoOpAutorateHook>();
        }
    }
    else
    {
        m_autorateHook.reset();
    }
}

bool
Helper::GetEnableAutorateIngress() const
{
    return m_enableAutorateIngress;
}

const AutorateIngressHook*
Helper::GetAutorateIngressHook() const
{
    return m_autorateHook.get();
}

void
Helper::SetAutorateImpl(AutorateImpl impl)
{
    m_autorateImpl = impl;
    // If autorate is already enabled, recreate the hook with the new
    // implementation so the change takes effect before BuildDispatcher.
    if (m_enableAutorateIngress)
    {
        if (impl == AutorateImpl::Linux)
        {
            m_autorateHook = std::make_shared<LinuxAutorateHook>();
        }
        else
        {
            m_autorateHook = std::make_shared<NoOpAutorateHook>();
        }
    }
}

Helper::AutorateImpl
Helper::GetAutorateImpl() const
{
    return m_autorateImpl;
}

void
Helper::SetEnableIngressMode(bool enabled)
{
    m_enableIngressMode = enabled;
}

bool
Helper::GetEnableIngressMode() const
{
    return m_enableIngressMode;
}

Ptr<QueueDisc>
Helper::BuildDispatcher()
{
    switch (m_shaperMode)
    {
    case ShaperMode::TokenBucket:
    case ShaperMode::TbfInner: {
        Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();
        const bool useInnerTbf = (m_shaperMode == ShaperMode::TbfInner);
        Helper::SetAsCakeDiffserv4(edge,
                                   DataRate(m_globalRateBps),
                                   /*enableAckFilter=*/false,
                                   /*enableLlq=*/false,
                                   /*enableTinShaping=*/useInnerTbf,
                                   /*enableHostIsolation=*/false,
                                   /*useInnerTbfShaping=*/useInnerTbf);
        return edge;
    }
    case ShaperMode::RateBased: {
        Ptr<RateBasedShaperDispatcher> rb = CreateObject<RateBasedShaperDispatcher>();
        rb->ConfigureGlobal(m_globalRateBps);
        for (uint32_t slot = 0; slot < m_tinCount; ++slot)
        {
            const auto it = m_tinRateOverride.find(slot);
            const uint64_t tinRate =
                (it != m_tinRateOverride.end()) ? it->second : m_uniformTinRateBps;
            rb->ConfigureTin(slot,
                             tinRate,
                             /*overhead=*/0,
                             /*mpu=*/0,
                             RateBasedTinClock::FramingMode::NoAtm);
        }
        // Default DSCP -> slot map. For m_tinCount == 4 we replicate the
        // diffserv4 layout used by SetAsCakeDiffserv4 (Bulk / BE / Video /
        // Voice). For other tin counts we leave the table at its zero-
        // initialised default (all DSCPs -> tin 0), which suits the v1
        // bulk-TCP scenarios at DSCP=0.
        if (m_tinCount == 4)
        {
            // DSCP -> tin slot transcribed from the Linux sch_cake.c
            // diffserv4[] table (the same table SetAsCakeDiffserv4 uses),
            // mapped through this layout's slot order
            // Bulk(0)/BE(1)/Video(2)/Voice(3). Linux storage tins: 0=BE,
            // 1=Bulk, 2=Video, 3=Voice.
            static constexpr std::array<uint8_t, 64> kLinuxDiffserv4Tin = {
                0, 1, 0, 0, 2, 0, 0, 0, //
                1, 0, 0, 0, 0, 0, 0, 0, //
                2, 0, 2, 0, 2, 0, 2, 0, //
                2, 0, 2, 0, 2, 0, 2, 0, //
                3, 0, 2, 0, 2, 0, 2, 0, //
                3, 0, 0, 0, 3, 0, 3, 0, //
                3, 0, 0, 0, 0, 0, 0, 0, //
                3, 0, 0, 0, 0, 0, 0, 0, //
            };
            static constexpr std::array<uint8_t, 4> kTinToSlot = {1, 0, 2, 3};
            for (uint8_t d = 0; d < 64; ++d)
            {
                rb->SetDscpToSlot(d, kTinToSlot[kLinuxDiffserv4Tin[d]]);
            }
            // Linux cake_config_diffserv4 scan priority: BE < Bulk < Video <
            // Voice (raw tin-index order there); this layout's slots are
            // Bulk(0)/BE(1)/Video(2)/Voice(3), so the ascending-priority
            // permutation swaps the first two.
            rb->SetTinPriorityOrder({1u, 0u, 2u, 3u});
        }
        if (m_enableIngressMode)
        {
            rb->SetIngressMode(true);
        }
        // If the Linux autorate hook is installed, wire it into the dispatcher
        // so that DoEnqueue calls OnEnqueue on every accepted packet.
        // The dispatcher receives a shared_ptr so it co-owns the hook and
        // the hook outlives a stack-local helper.
        if (m_enableAutorateIngress && m_autorateImpl == AutorateImpl::Linux &&
            m_autorateHook != nullptr)
        {
            std::shared_ptr<LinuxAutorateHook> linuxHook =
                std::dynamic_pointer_cast<LinuxAutorateHook>(m_autorateHook);
            if (linuxHook != nullptr)
            {
                rb->SetAutorateHook(linuxHook);
            }
        }
        return rb;
    }
    }
    // Unreachable; satisfies non-void return on compilers that do not
    // analyse the exhaustive switch above.
    return CreateObject<EdgeQueueDisc>();
}

Ptr<QueueDisc>
Helper::BuildAndInstall(Ptr<NetDevice> device)
{
    Ptr<QueueDisc> qdisc = BuildDispatcher();
    Ptr<TrafficControlLayer> tc = device->GetNode()->GetObject<TrafficControlLayer>();
    NS_ASSERT_MSG(tc, "TrafficControlLayer must be installed on the node");
    // InternetStackHelper::Install attaches a default root qdisc per
    // device; remove it before installing the CAKE dispatcher so the
    // install does not collide with the existing root qdisc.  The Get
    // probe is required because DeleteRootQueueDiscOnDevice asserts
    // when no root qdisc is present on the device.
    if (tc->GetRootQueueDiscOnDevice(device))
    {
        tc->DeleteRootQueueDiscOnDevice(device);
    }
    tc->SetRootQueueDiscOnDevice(device, qdisc);
    return qdisc;
}

namespace
{

constexpr uint32_t kCakeMtu = 1514;   // Ethernet frame, matches Linux tc-cake.
constexpr uint32_t kQuantumScale = 4; // DRR quantum = MTU * share * scale.

/**
 * Build one tin as a mainline `FqCobaltQueueDisc` configured per the
 * CAKE §III-B (8-way set-associative hashing, SetWays=8,
 * Quantum=MTU). Per-tin rate-capping is enforced by the across-tin DRR
 * quantum, not by an outer rate-shaper — the bottleneck link itself caps
 * aggregate rate; DRR distributes share among busy tins; idle-tin
 * capacity is redistributed to busy tins (Linux tc-cake's default).
 *
 * When @p enableHostIsolation is true, `EnableHostIsolation=true` and
 * `HostIsolationMode=Triple` are set on the disc (attributes from
 * `patches/ns3/0016-fq-cobalt-host-isolation.patch`), matching Linux
 * `sch_cake.c` triple-isolate semantics (67dc6c56b871).
 *
 * `EnableAckFilter`, `EnableAckFilterAggressive`, and `MemLimit` are
 * exposed by `patches/ns3/0006-fq-cobalt-ack-filter-memlimit.patch`
 * (filed upstream; patch retires once the pin advances past the merge).
 */
Ptr<QueueDisc>
MakeTin(bool enableAckFilter,
        bool enableAckFilterAggressive,
        bool enableHostIsolation,
        bool useDualPi2Inner)
{
    NS_ASSERT_MSG(!(useDualPi2Inner && enableHostIsolation),
                  "DualPI2 inner is mutually exclusive with host-isolation: "
                  "DualPI2 has its own classic+L4S queueing layer that does not "
                  "compose with the host-isolation attributes on FqCobaltQueueDisc. "
                  "To compose CAKE host-fairness with L4S signaling, the substrate "
                  "would need a host-isolated DualPI2 variant (future work).");
    if (useDualPi2Inner)
    {
        // L4S DualPI2 as the per-tin inner. The tin's outer rate cap is
        // enforced by the across-tin DRR + optional TBF wrapper; DualPI2
        // does marking (ECT(1)) for scalable congestion controls and
        // drop-based AQM (RED-flavoured) for classic flows, with coupled
        // marking probability between the two queues per RFC 9332. The
        // ACK-filter knobs do not apply (DualPI2 has no ACK filter); they
        // are silently ignored when this branch is taken.
        //
        // Configure the inner the same way a standalone DualPI2 bottleneck
        // is configured. Left at raw construction defaults the inner runs a
        // shallow WRED classic queue (25-packet limit, early-mark from an
        // average of 5 packets) with no starvation-safe scheduler — far
        // below a typical bandwidth-delay product, so the two responsive
        // flows cannot keep the link full and aggregate throughput
        // collapses to a fraction of the standalone case. CoupledOnly turns
        // the classic queue into a deep pass-through FIFO whose only
        // congestion signal is the coupled probability, BDP-scale buffering
        // lets the flows fill the pipe, and the coupled scheduler keeps the
        // classic queue from starving under sustained L4S load.
        constexpr uint32_t kInnerClassicLimitPkts = 500;
        constexpr uint32_t kInnerL4sLimitPkts = 500;
        Ptr<l4s::QueueDisc> dualPi2 = CreateObject<l4s::QueueDisc>();
        dualPi2->SetClassicAqm(l4s::QueueDisc::ClassicAqm::CoupledOnly);
        dualPi2->SetNumQueues(2);
        dualPi2->SetL4sQueueIdx(1);
        dualPi2->SetQueueLimit(0, kInnerClassicLimitPkts);
        dualPi2->SetQueueLimit(1, kInnerL4sLimitPkts);
        dualPi2->AddPhbEntry(0, 0, 0);
        Ptr<l4s::CoupledScheduler> sched =
            CreateObjectWithAttributes<l4s::CoupledScheduler>("NumQueues",
                                                              UintegerValue(2),
                                                              "L4sQueueIdx",
                                                              UintegerValue(1),
                                                              "BurstCap",
                                                              UintegerValue(8));
        dualPi2->SetScheduler(sched);
        return dualPi2;
    }
    // Host-iso path and standard path both produce a mainline
    // FqCobaltQueueDisc; the host-isolation knob is threaded in via the
    // EnableHostIsolation / HostIsolationMode attributes supplied by
    // patches/ns3/0016-fq-cobalt-host-isolation.patch. Per-side-max
    // triple-isolate keying matches Linux sch_cake.c @ 67dc6c56b871.
    Ptr<FqCobaltQueueDisc> fq = CreateObjectWithAttributes<FqCobaltQueueDisc>(
        "EnableSetAssociativeHash",
        BooleanValue(true),
        "SetWays",
        UintegerValue(8),
        "EnableAckFilter",
        BooleanValue(enableAckFilter),
        "EnableAckFilterAggressive",
        BooleanValue(enableAckFilterAggressive),
        "EnableHostIsolation",
        BooleanValue(enableHostIsolation),
        "HostIsolationMode",
        EnumValue<FqCobaltQueueDisc::HostIsolationMode>(
            FqCobaltQueueDisc::HostIsolationMode::Triple));
    fq->SetQuantum(kCakeMtu);
    return fq;
}

/// Sentinel meaning "no LLQ slot configured; use pure-DRR dispatcher".
constexpr uint32_t kNoLlqSlot = std::numeric_limits<uint32_t>::max();

/**
 * Install @p numTins tins on @p edge with @p shares, build the across-
 * tin dispatcher with share-proportional quanta, and stamp the DSCP map.
 *
 * @param edge fresh EdgeQueueDisc
 * @param totalRate aggregate rate
 * @param enableAckFilter ACK-filter opt-in
 * @param shares per-tin shares (size == numTins)
 * @param numTins tin count
 * @param dscpMap 64-entry DSCP -> tin index lookup
 * @param llqSlot if < numTins, install `HybridLlqDispatcher` and mark
 *        this slot strict-priority; otherwise install
 *        `TinShaperDispatcher` (pure DRR)
 * @param enableTinShaping when true, set per-tin hard rate caps on the
 *        dispatcher at `share × totalRate` with a 100 ms burst floored
 *        at `4 × MTU` (Linux tc-cake "bandwidth N" semantics). When
 *        false the dispatcher runs work-conserving and tin shares are
 *        enforced only by the DRR quanta.
 */
void
InstallTins(Ptr<EdgeQueueDisc> edge,
            DataRate totalRate,
            bool enableAckFilter,
            const double* shares,
            uint32_t numTins,
            const uint8_t* dscpMap,
            uint32_t llqSlot,
            bool enableTinShaping,
            bool enableHostIsolation,
            bool useInnerTbfShaping,
            bool enableAckFilterAggressive,
            bool useDualPi2Inner)
{
    NS_ASSERT_MSG(!useInnerTbfShaping || enableTinShaping,
                  "useInnerTbfShaping requires enableTinShaping; the toggle picks the "
                  "implementation of tin shaping, not whether shaping happens");
    NS_ASSERT_MSG(!(useDualPi2Inner && enableHostIsolation),
                  "useDualPi2Inner and enableHostIsolation are mutually exclusive "
                  "(see MakeTin contract).");
    NS_ASSERT_MSG(numTins >= 1 && numTins <= EdgeQueueDisc::kMaxInnerSlots,
                  "tin count " << numTins << " out of range");
    NS_ASSERT_MSG(llqSlot == kNoLlqSlot || llqSlot < numTins,
                  "LLQ slot " << llqSlot << " out of range for tin count " << numTins);

    const bool useLlq = (llqSlot != kNoLlqSlot);

    // Compose the across-tin dispatcher. The two paths share the per-
    // slot quantum derivation (MTU * share * scale, floored at one MTU);
    // the LLQ path skips quantum installation on the SP slot since SP
    // and DRR are mutually exclusive.
    Ptr<TinShaperDispatcher> shaper;
    Ptr<HybridLlqDispatcher> hybrid;
    if (useLlq)
    {
        hybrid = CreateObject<HybridLlqDispatcher>();
        hybrid->SetSlotStrictPriority(llqSlot);
    }
    else
    {
        shaper = CreateObject<TinShaperDispatcher>();
    }

    for (uint32_t s = 0; s < numTins; ++s)
    {
        Ptr<QueueDisc> tinCore = MakeTin(enableAckFilter,
                                         enableAckFilterAggressive,
                                         enableHostIsolation,
                                         useDualPi2Inner);

        // Compute the per-tin shaping parameters once (used by both path
        // alpha and path gamma below). Linux tc-cake "bandwidth N <profile>":
        // each tin's hard rate is `share × totalRate`; the burst is 100 ms
        // of that rate floored at 4 × MTU.
        const auto tinRateBps = static_cast<uint64_t>(totalRate.GetBitRate() * shares[s]);
        const uint64_t burstBytes =
            std::max<uint64_t>(static_cast<uint64_t>(4 * kCakeMtu), tinRateBps / 8 / 10);

        Ptr<QueueDisc> tinAsInner = tinCore;
        if (useInnerTbfShaping && enableTinShaping)
        {
            // Path gamma: wrap the tin core (FqCobalt-flavoured) with a
            // mainline TbfQueueDisc that enforces the per-tin cap. The
            // dispatcher's SetRateCap is intentionally skipped below; the
            // gate lives in the TBF. This composition relies on the
            // patches/ns3/0004 inner-mode guard so the TBF's watchdog
            // doesn't trip the m_send assertion when nested.
            Ptr<TbfQueueDisc> tbf = CreateObject<TbfQueueDisc>();
            tbf->SetAttribute("Rate", DataRateValue(DataRate(tinRateBps)));
            tbf->SetAttribute("Burst", UintegerValue(burstBytes));
            tbf->SetAttribute("Mtu", UintegerValue(kCakeMtu));
            // The TBF's auto-default FifoQueueDisc child is replaced by
            // tinCore via AddQueueDiscClass before Initialize. Setting
            // MaxSize is a no-op here because the FqCobalt-flavoured
            // inner uses MULTIPLE_QUEUES sizing; the FQ owns its own
            // packet-count limit per-flow.
            Ptr<QueueDiscClass> cls = CreateObject<QueueDiscClass>();
            cls->SetQueueDisc(tinCore);
            tbf->AddQueueDiscClass(cls);
            tinAsInner = tbf;
        }

        edge->SetInnerDiscAt(s, tinAsInner);

        const bool isSpSlot = (useLlq && s == llqSlot);

        if (!isSpSlot)
        {
            const uint32_t quantum =
                std::max<uint32_t>(static_cast<uint32_t>(kCakeMtu * shares[s] * kQuantumScale),
                                   kCakeMtu);
            if (useLlq)
            {
                hybrid->SetQuantum(s, quantum);
            }
            else
            {
                shaper->SetQuantum(s, quantum);
            }
        }

        if (enableTinShaping && !useInnerTbfShaping)
        {
            // Path alpha: in-dispatcher TinTokenBucket gate. Applies to SP
            // and DRR slots equally — (enableLlq && enableTinShaping) is
            // the Cisco MQC LLQ pattern (priority class with hard cap).
            if (useLlq)
            {
                hybrid->SetRateCap(s, tinRateBps, burstBytes);
            }
            else
            {
                shaper->SetRateCap(s, tinRateBps, burstBytes);
            }
        }
    }

    edge->SetSlotDispatcher(useLlq ? Ptr<SlotDispatcher>(hybrid) : Ptr<SlotDispatcher>(shaper));

    for (uint32_t dscp = 0; dscp < kMaxCodePoints; ++dscp)
    {
        edge->SetDscpToSlot(static_cast<uint8_t>(dscp), dscpMap[dscp]);
    }
}

} // namespace

void
Helper::SetAsCakeDiffserv4(Ptr<EdgeQueueDisc> edge,
                           DataRate totalRate,
                           bool enableAckFilter,
                           bool enableLlq,
                           bool enableTinShaping,
                           bool enableHostIsolation,
                           bool useInnerTbfShaping,
                           bool enableAckFilterAggressive,
                           bool useDualPi2Inner)
{
    NS_LOG_FUNCTION(edge << totalRate << enableAckFilter << enableLlq << enableTinShaping
                         << enableHostIsolation << useInnerTbfShaping << enableAckFilterAggressive
                         << useDualPi2Inner);

    // Linux tc-cake(8) diffserv4 shares: Bulk 6.25%, BE 100%, Video 50%, Voice 25%.
    static constexpr std::array<double, 4> kShares = {0.0625, 1.0, 0.5, 0.25};

    // DSCP -> tin table transcribed verbatim from the frozen Linux
    // excerpt (the diffserv4[] array, sch_cake.c:337-346 at
    // provenance/linux-sch-cake-67dc6c56b871/sch_cake.c); Linux tins
    // 0..3 = BE / Bulk / Video / Voice. Bulk = {LE, CS1}; Video =
    // {TOS4, CS2, AF2x, CS3, AF3x, AF4x}; Voice = {CS4, CS5, VA, EF,
    // CS6, CS7}; everything else (including AF1x and TOS2) is BE.
    static constexpr std::array<uint8_t, kMaxCodePoints> kLinuxTin = {
        0, 1, 0, 0, 2, 0, 0, 0, //
        1, 0, 0, 0, 0, 0, 0, 0, //
        2, 0, 2, 0, 2, 0, 2, 0, //
        2, 0, 2, 0, 2, 0, 2, 0, //
        3, 0, 2, 0, 2, 0, 2, 0, //
        3, 0, 0, 0, 3, 0, 3, 0, //
        3, 0, 0, 0, 0, 0, 0, 0, //
        3, 0, 0, 0, 0, 0, 0, 0, //
    };
    // Linux tin -> this layout's slot order Bulk(0) / BE(1) / Video(2)
    // / Voice(3).
    static constexpr std::array<uint8_t, 4> kTinToSlot = {1, 0, 2, 3};
    std::array<uint8_t, kMaxCodePoints> dscpMap{};
    for (uint32_t d = 0; d < kMaxCodePoints; ++d)
    {
        dscpMap[d] = kTinToSlot[kLinuxTin[d]];
    }

    // diffserv4 LLQ slot is Voice (slot 3, EF/CS5/CS4/CS6/CS7/VA).
    InstallTins(edge,
                totalRate,
                enableAckFilter,
                kShares.data(),
                4,
                dscpMap.data(),
                enableLlq ? 3u : kNoLlqSlot,
                enableTinShaping,
                enableHostIsolation,
                useInnerTbfShaping,
                enableAckFilterAggressive,
                useDualPi2Inner);
    StampProfile(edge, Profile::Diffserv4);
}

void
Helper::SetAsCakeDiffserv3(Ptr<EdgeQueueDisc> edge,
                           DataRate totalRate,
                           bool enableAckFilter,
                           bool enableLlq,
                           bool enableTinShaping,
                           bool enableHostIsolation,
                           bool useInnerTbfShaping,
                           bool enableAckFilterAggressive)
{
    NS_LOG_FUNCTION(edge << totalRate << enableAckFilter << enableLlq << enableTinShaping
                         << enableHostIsolation << useInnerTbfShaping << enableAckFilterAggressive);

    // diffserv3 shares follow the kernel quantum ladder — quanta
    // 1024 / 64 / 256 for BE / Bulk / Latency-Sensitive
    // (cake_config_diffserv3, sch_cake.c:2576-2580 at the frozen
    // provenance excerpt): Bulk 6.25%, Latency-Sensitive 25%, BE 100%.
    static constexpr std::array<double, 3> kShares = {0.0625, 0.25, 1.0};

    // DSCP -> tin table transcribed verbatim from the frozen Linux
    // excerpt (the diffserv3[] array, sch_cake.c:348-357 at
    // provenance/linux-sch-cake-67dc6c56b871/sch_cake.c); Linux tins
    // 0..2 = BE / Bulk / Latency-Sensitive. Bulk = {LE, CS1};
    // Latency-Sensitive = {TOS4, VA, EF, CS6, CS7}; everything else
    // (including the AF classes and CS2..CS5) is BE.
    static constexpr std::array<uint8_t, kMaxCodePoints> kLinuxTin = {
        0, 1, 0, 0, 2, 0, 0, 0, //
        1, 0, 0, 0, 0, 0, 0, 0, //
        0, 0, 0, 0, 0, 0, 0, 0, //
        0, 0, 0, 0, 0, 0, 0, 0, //
        0, 0, 0, 0, 0, 0, 0, 0, //
        0, 0, 0, 0, 2, 0, 2, 0, //
        2, 0, 0, 0, 0, 0, 0, 0, //
        2, 0, 0, 0, 0, 0, 0, 0, //
    };
    // Linux tin -> this layout's slot order Bulk(0) /
    // Latency-Sensitive(1) / BE(2).
    static constexpr std::array<uint8_t, 3> kTinToSlot = {2, 0, 1};
    std::array<uint8_t, kMaxCodePoints> dscpMap{};
    for (uint32_t d = 0; d < kMaxCodePoints; ++d)
    {
        dscpMap[d] = kTinToSlot[kLinuxTin[d]];
    }

    // diffserv3 LLQ slot is Latency-Sensitive (slot 1).
    InstallTins(edge,
                totalRate,
                enableAckFilter,
                kShares.data(),
                3,
                dscpMap.data(),
                enableLlq ? 1u : kNoLlqSlot,
                enableTinShaping,
                enableHostIsolation,
                useInnerTbfShaping,
                enableAckFilterAggressive,
                /*useDualPi2Inner=*/false);
    StampProfile(edge, Profile::Diffserv3);
}

void
Helper::SetAsCakeDiffserv8(Ptr<EdgeQueueDisc> edge,
                           DataRate totalRate,
                           bool enableAckFilter,
                           bool enableLlq,
                           bool enableTinShaping,
                           bool enableHostIsolation,
                           bool useInnerTbfShaping,
                           bool enableAckFilterAggressive)
{
    NS_LOG_FUNCTION(edge << totalRate << enableAckFilter << enableLlq << enableTinShaping
                         << enableHostIsolation << useInnerTbfShaping << enableAckFilterAggressive);

    // diffserv8 shares follow the kernel's geometric quantum ladder —
    // each tin at 7/8 of the previous (quantum *= 7; quantum >>= 3;
    // sch_cake.c:2500-2507 at the frozen provenance excerpt). Lower
    // tins carry larger bandwidth-sharing weights; higher tins rely on
    // selection priority instead.
    std::array<double, 8> shares{};
    double share = 1.0;
    for (auto& s : shares)
    {
        s = share;
        share = share * 7.0 / 8.0;
    }

    // DSCP -> tin table transcribed verbatim from the frozen Linux
    // excerpt (the diffserv8[] array, sch_cake.c:326-335); slot order
    // follows the kernel tin order directly. Tins 0..7 = Background
    // {LE} / High Throughput {TOS2, CS1, AF1x} / Bog Standard {DF and
    // unspecified} / Video Streaming {CS3, AF3x, AF4x} / Low-Latency
    // Transactions {TOS4, AF2x} / Interactive Shell {CS2} / Minimum
    // Latency {CS4, CS5, VA, EF} / Network Control {CS6, CS7}.
    static constexpr std::array<uint8_t, kMaxCodePoints> kLinuxTin = {
        2, 0, 1, 2, 4, 2, 2, 2, //
        1, 2, 1, 2, 1, 2, 1, 2, //
        5, 2, 4, 2, 4, 2, 4, 2, //
        3, 2, 3, 2, 3, 2, 3, 2, //
        6, 2, 3, 2, 3, 2, 3, 2, //
        6, 2, 2, 2, 6, 2, 6, 2, //
        7, 2, 2, 2, 2, 2, 2, 2, //
        7, 2, 2, 2, 2, 2, 2, 2, //
    };
    std::array<uint8_t, kMaxCodePoints> dscpMap{};
    for (uint32_t d = 0; d < kMaxCodePoints; ++d)
    {
        dscpMap[d] = kLinuxTin[d];
    }

    // diffserv8 LLQ slot is the Minimum-Latency tin (slot 6:
    // CS4/CS5/VA/EF).
    InstallTins(edge,
                totalRate,
                enableAckFilter,
                shares.data(),
                8,
                dscpMap.data(),
                enableLlq ? 6u : kNoLlqSlot,
                enableTinShaping,
                enableHostIsolation,
                useInnerTbfShaping,
                enableAckFilterAggressive,
                /*useDualPi2Inner=*/false);
    StampProfile(edge, Profile::Diffserv8);
}

void
Helper::SetAsCakeBestEffort(Ptr<EdgeQueueDisc> edge,
                            DataRate totalRate,
                            bool enableAckFilter,
                            bool enableLlq,
                            bool enableTinShaping,
                            bool enableHostIsolation,
                            bool useInnerTbfShaping,
                            bool enableAckFilterAggressive)
{
    NS_LOG_FUNCTION(edge << totalRate << enableAckFilter << enableLlq << enableTinShaping
                         << enableHostIsolation << useInnerTbfShaping << enableAckFilterAggressive);

    // besteffort: a single tin, full rate. enableLlq is a no-op (one tin
    // — there is no cross-tin priority ordering).
    static constexpr std::array<double, 1> kShares = {1.0};

    std::array<uint8_t, kMaxCodePoints> dscpMap{};
    dscpMap.fill(0); // every DSCP -> tin 0

    InstallTins(edge,
                totalRate,
                enableAckFilter,
                kShares.data(),
                1,
                dscpMap.data(),
                kNoLlqSlot,
                enableTinShaping,
                enableHostIsolation,
                useInnerTbfShaping,
                enableAckFilterAggressive,
                /*useDualPi2Inner=*/false);
    StampProfile(edge, Profile::Besteffort);
}

void
Helper::SetAsCakePrecedence(Ptr<EdgeQueueDisc> edge,
                            DataRate totalRate,
                            bool enableAckFilter,
                            bool enableLlq,
                            bool enableTinShaping,
                            bool enableHostIsolation,
                            bool useInnerTbfShaping,
                            bool enableAckFilterAggressive)
{
    NS_LOG_FUNCTION(edge << totalRate << enableAckFilter << enableLlq << enableTinShaping
                         << enableHostIsolation << useInnerTbfShaping << enableAckFilterAggressive);

    // precedence: 8 tins, one per IP precedence value (top 3 bits of DSCP).
    // Per-tin DRR shares follow Linux's cake_config_precedence quantum ladder
    // (a base quantum decayed geometrically, quantum *= 7; quantum >>= 3), so
    // tin 0 (best effort) carries the largest bandwidth-sharing weight and tin 7
    // the smallest; higher tins rely on selection priority instead.
    std::array<double, 8> shares{};
    double share = 1.0;
    for (auto& s : shares)
    {
        s = share;
        share = share * 7.0 / 8.0;
    }

    std::array<uint8_t, kMaxCodePoints> dscpMap{};
    for (uint32_t dscp = 0; dscp < kMaxCodePoints; ++dscp)
    {
        // Top 3 bits of DSCP yield IP precedence (0..7). All eight DSCPs
        // sharing the same precedence land in the same tin.
        dscpMap[dscp] = static_cast<uint8_t>(dscp >> 3);
    }

    InstallTins(edge,
                totalRate,
                enableAckFilter,
                shares.data(),
                8,
                dscpMap.data(),
                enableLlq ? 7u : kNoLlqSlot,
                enableTinShaping,
                enableHostIsolation,
                useInnerTbfShaping,
                enableAckFilterAggressive,
                /*useDualPi2Inner=*/false);
    StampProfile(edge, Profile::Precedence);
}

namespace
{
// CAKE statistical-overhead bimodal Internet mix:
// 50% small ACK-class packets (64B) + 50% MTU-class packets (1500B).
// Linux `tc-cake` measurements use ~equivalent traffic; deviation
// from this mix produces deterministic relative error bounded by
// the spread between min and max wire(s) values.
constexpr uint32_t kCakeBimodalSmallBytes = 64;
constexpr uint32_t kCakeBimodalLargeBytes = 1500;
constexpr double kCakeBimodalSmallProb = 0.5;
constexpr double kCakeBimodalLargeProb = 0.5;

// Mirror of Linux `cake_overhead()` for a single packet: apply the
// per-packet adjustment, then optional ATM/PTM cell rounding, then
// floor at MPU. Not stateful — pure function of the five inputs.
uint32_t
CakeWireBytesFor(uint32_t ipBytes, uint32_t overhead, bool atm, bool ptm, uint32_t mpu)
{
    const uint32_t base = ipBytes + overhead;
    uint32_t framed;
    if (atm)
    {
        framed = ((base + 47) / 48) * 53;
    }
    else if (ptm)
    {
        framed = base + (base + 63) / 64;
    }
    else
    {
        framed = base;
    }
    return std::max(framed, mpu);
}

// E[wire(s)] / E[s] over the bimodal mix. Returns 1.0 in degenerate
// configs (no overhead, no ATM, no PTM, no MPU above smallest packet)
// so the caller can no-op the rate-adjustment pass without branching.
double
CakeGammaForBimodalMix(uint32_t overhead, bool atm, bool ptm, uint32_t mpu)
{
    const uint32_t wireSmall = CakeWireBytesFor(kCakeBimodalSmallBytes, overhead, atm, ptm, mpu);
    const uint32_t wireLarge = CakeWireBytesFor(kCakeBimodalLargeBytes, overhead, atm, ptm, mpu);
    const double numerator = kCakeBimodalSmallProb * wireSmall + kCakeBimodalLargeProb * wireLarge;
    const double denominator = kCakeBimodalSmallProb * kCakeBimodalSmallBytes +
                               kCakeBimodalLargeProb * kCakeBimodalLargeBytes;
    return numerator / denominator;
}

struct LinkPresetTuple
{
    uint32_t overhead;
    bool atm;
    bool ptm;
    uint32_t mpu;
    bool raw{false};
};

LinkPresetTuple
ResolveLinkPreset(Helper::LinkPreset preset)
{
    switch (preset)
    {
    case Helper::LinkPreset::Raw:
        return {0, false, false, 0, /*raw=*/true};
    case Helper::LinkPreset::Conservative:
        return {48, false, false, 64};
    case Helper::LinkPreset::Ethernet:
        return {38, false, false, 84};
    case Helper::LinkPreset::EtherVlan:
        return {42, false, false, 84};
    case Helper::LinkPreset::Docsis:
        return {18, false, false, 64};
    case Helper::LinkPreset::PppoePtm:
        return {30, false, true, 0};
    case Helper::LinkPreset::PppoeVcmux:
        return {32, true, false, 0};
    case Helper::LinkPreset::PppoeLlcsnap:
        return {40, true, false, 0};
    case Helper::LinkPreset::PppoaVcmux:
        return {10, true, false, 0};
    case Helper::LinkPreset::PppoaLlc:
        return {14, true, false, 0};
    case Helper::LinkPreset::BridgedPtm:
        return {22, false, true, 0};
    case Helper::LinkPreset::BridgedVcmux:
        return {24, true, false, 0};
    case Helper::LinkPreset::BridgedLlcsnap:
        return {32, true, false, 0};
    case Helper::LinkPreset::IpoaVcmux:
        return {8, true, false, 0};
    case Helper::LinkPreset::IpoaLlcsnap:
        return {16, true, false, 0};
    }
    NS_FATAL_ERROR("Unknown LinkPreset enum value");
    return {0, false, false, 0};
}

// FqCobaltQueueDisc stores Target and Interval as string attributes
// (e.g. "5ms", "100ms"); they are parsed by StringValue and forwarded
// to each inner CobaltQueueDisc when InitializeParams fires.
struct RttPresetTuple
{
    const char* target;
    const char* interval;
};

RttPresetTuple
ResolveRttPreset(Helper::RttPreset preset)
{
    switch (preset)
    {
    case Helper::RttPreset::Datacentre:
        return {"5us", "100us"};
    case Helper::RttPreset::Lan:
        return {"50us", "1ms"};
    case Helper::RttPreset::Metro:
        return {"500us", "10ms"};
    case Helper::RttPreset::Regional:
        return {"1500us", "30ms"};
    case Helper::RttPreset::Internet:
        return {"5ms", "100ms"};
    case Helper::RttPreset::Oceanic:
        return {"15ms", "300ms"};
    case Helper::RttPreset::Satellite:
        return {"50ms", "1000ms"};
    case Helper::RttPreset::Interplanetary:
        return {"50s", "1000s"};
    }
    NS_FATAL_ERROR("Unknown RttPreset enum value");
    return {nullptr, nullptr};
}
} // namespace

void
Helper::SetAsCakeAlphaTinShaped(Ptr<EdgeQueueDisc> edge,
                                DataRate totalRate,
                                bool enableAckFilter,
                                bool enableLlq,
                                bool enableHostIsolation,
                                bool enableAckFilterAggressive)
{
    NS_LOG_FUNCTION(edge << totalRate << enableAckFilter << enableLlq << enableHostIsolation
                         << enableAckFilterAggressive);
    NS_ASSERT_MSG(edge, "SetAsCakeAlphaTinShaped requires non-null edge");
    // Path-α with per-tin caps: TokenBucket-via-dispatcher across tins
    // (useInnerTbfShaping=false) plus the in-dispatcher TinTokenBucket
    // gate inside each tin (enableTinShaping=true). Closes the gap
    // surfaced by the path α/β/γ comparison panel where default α
    // composition lets traffic through at link rate.
    Helper::SetAsCakeDiffserv4(edge,
                               totalRate,
                               enableAckFilter,
                               enableLlq,
                               /*enableTinShaping=*/true,
                               enableHostIsolation,
                               /*useInnerTbfShaping=*/false,
                               enableAckFilterAggressive);
}

void
Helper::SetAsCakeConservative(Ptr<EdgeQueueDisc> edge)
{
    NS_LOG_FUNCTION(edge);
    NS_ASSERT_MSG(edge, "SetAsCakeConservative requires non-null edge");
    SetLinkLayer(edge, LinkPreset::Conservative);
}

void
Helper::SetLinkLayer(Ptr<EdgeQueueDisc> edge, LinkPreset preset)
{
    NS_LOG_FUNCTION(edge << static_cast<int>(preset));
    NS_ASSERT_MSG(edge, "SetLinkLayer requires non-null edge");

    const LinkPresetTuple t = ResolveLinkPreset(preset);
    ConfigureLinkLayerOverhead(edge, t.overhead, t.atm, t.ptm, t.mpu, t.raw);
}

void
Helper::SetBandwidth(Ptr<EdgeQueueDisc> edge, DataRate bandwidth)
{
    NS_LOG_FUNCTION(edge << bandwidth);
    NS_ASSERT_MSG(edge, "SetBandwidth requires non-null edge");
    NS_ABORT_MSG_IF(bandwidth.GetBitRate() == 0, "SetBandwidth requires a non-zero rate");

    // Derive the tin-rate ladder from the profile recorded at
    // composition time — the analog of Linux re-deriving tin
    // parameters from the stored q->tin_mode (sch_cake.c:212,
    // :2592-2624) rather than from a caller declaration. Slot count
    // alone cannot name a profile: precedence and diffserv8 both
    // build 8 tins.
    Ptr<ProfileMarker> marker = edge->GetObject<ProfileMarker>();
    NS_ABORT_MSG_IF(!marker,
                    "SetBandwidth: edge was not composed by a cake::Helper profile "
                    "composer (no tin profile recorded)");
    const Profile profile = marker->GetProfile();

    uint32_t expectedSlots = 0;
    const char* profileName = "";
    switch (profile)
    {
    case Profile::Besteffort:
        NS_ABORT_MSG("SetBandwidth does not support the 'besteffort' profile");
        break;
    case Profile::Precedence:
        NS_ABORT_MSG("SetBandwidth does not support the 'precedence' profile");
        break;
    case Profile::Diffserv3:
        expectedSlots = 3;
        profileName = "diffserv3";
        break;
    case Profile::Diffserv4:
        expectedSlots = 4;
        profileName = "diffserv4";
        break;
    case Profile::Diffserv8:
        expectedSlots = 8;
        profileName = "diffserv8";
        break;
    }

    uint32_t populated = 0;
    for (uint32_t s = 0; s < EdgeQueueDisc::kMaxInnerSlots; ++s)
    {
        if (edge->GetInnerDiscAt(s))
        {
            ++populated;
        }
    }
    if (populated != expectedSlots || !edge->GetInnerDiscAt(expectedSlots - 1))
    {
        NS_ABORT_MSG("SetBandwidth expects slots 0-"
                     << (expectedSlots - 1) << " populated (the recorded '" << profileName
                     << "' layout); found " << populated << " populated slots");
    }
    Ptr<TinShaperDispatcher> current = DynamicCast<TinShaperDispatcher>(edge->GetSlotDispatcher());
    NS_ABORT_MSG_IF(!current,
                    "SetBandwidth requires the default work-conserving dispatcher "
                    "(already-shaped, LLQ, and custom dispatchers are unsupported)");
    for (uint32_t s = 0; s < EdgeQueueDisc::kMaxInnerSlots; ++s)
    {
        if (edge->GetInnerDiscAt(s) && current->GetRateCapBps(s) != 0)
        {
            NS_ABORT_MSG("SetBandwidth requires an uncapped work-conserving composition; "
                         "slot "
                         << s << " carries a per-tin rate cap");
        }
        if (DynamicCast<TbfQueueDisc>(edge->GetInnerDiscAt(s)))
        {
            NS_ABORT_MSG("SetBandwidth cannot stack on TBF-wrapped inners; "
                         "slot "
                         << s << " is rate-capped below the aggregate clock");
        }
    }
    const uint64_t rate = bandwidth.GetBitRate();
    Ptr<ShapedTinDispatcher> shaped = CreateObject<ShapedTinDispatcher>();
    shaped->ConfigureGlobal(rate);
    switch (profile)
    {
    case Profile::Diffserv3:
        // cake_config_diffserv3 tin rates (rate, rate >> 4, rate >> 2
        // for BE / Bulk / Latency-Sensitive; sch_cake.c:2553-2583)
        // mapped onto this layout's slot order Bulk(0) /
        // Latency-Sensitive(1) / BE(2).
        shaped->ConfigureTin(0, rate >> 4, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        shaped->ConfigureTin(1, rate >> 2, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        shaped->ConfigureTin(2, rate, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        // Ascending scan priority: BE < Bulk < Latency-Sensitive — the
        // raw Linux tin-index order (the shaped-mode scan walks tin
        // indices ascending and the last schedule-meeting tin wins,
        // sch_cake.c:2110-2123) in this layout's slot numbering.
        shaped->SetTinPriorityOrder({2u, 0u, 1u});
        break;
    case Profile::Diffserv4:
        // cake_config_diffserv4 tin rates (rate, rate >> 4, rate >> 1,
        // rate >> 2 for BE / Bulk / Video / Voice; sch_cake.c:2526-2549)
        // mapped onto this layout's slot order Bulk(0) / BE(1) /
        // Video(2) / Voice(3).
        shaped->ConfigureTin(0, rate >> 4, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        shaped->ConfigureTin(1, rate, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        shaped->ConfigureTin(2, rate >> 1, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        shaped->ConfigureTin(3, rate >> 2, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
        // Ascending scan priority: BE < Bulk < Video < Voice; this layout
        // swaps the first pair relative to the raw tin-index order.
        shaped->SetTinPriorityOrder({1u, 0u, 2u, 3u});
        break;
    case Profile::Diffserv8:
    {
        // cake_config_diffserv8 geometric tin-rate ladder — each tin at
        // 7/8 of the previous (rate *= 7; rate >>= 3;
        // sch_cake.c:2467-2512). This layout's slot order follows the
        // ladder index, so the ascending scan priority is the identity
        // permutation (normal_order).
        uint64_t tinRate = rate;
        std::vector<uint32_t> order;
        order.reserve(expectedSlots);
        for (uint32_t s = 0; s < expectedSlots; ++s)
        {
            shaped->ConfigureTin(s, tinRate, 0, 0, RateBasedTinClock::FramingMode::NoAtm);
            order.push_back(s);
            tinRate *= 7;
            tinRate >>= 3;
        }
        shaped->SetTinPriorityOrder(order);
        break;
    }
    case Profile::Besteffort:
    case Profile::Precedence:
        break; // unreachable — rejected before the pre-flight checks
    }
    edge->SetSlotDispatcher(shaped);
}

void
Helper::SetRttPreset(Ptr<EdgeQueueDisc> edge, RttPreset preset)
{
    NS_LOG_FUNCTION(edge << static_cast<int>(preset));
    NS_ASSERT_MSG(edge, "SetRttPreset requires non-null edge");

    const RttPresetTuple t = ResolveRttPreset(preset);

    for (uint32_t slot = 0; slot < edge->GetNumInnerSlots(); ++slot)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(slot);
        if (!inner)
        {
            continue;
        }
        // Walk to the nearest FqCobaltQueueDisc: direct, or wrapped by TBF.
        // L4S (l4s::QueueDisc) tins are skipped — they carry no
        // FqCobaltQueueDisc and the GetObject<> / DynamicCast<> below will
        // fall through to the NS_LOG_WARN path.
        Ptr<FqCobaltQueueDisc> fq = inner->GetObject<FqCobaltQueueDisc>();
        if (!fq)
        {
            Ptr<TbfQueueDisc> tbf = inner->GetObject<TbfQueueDisc>();
            if (tbf && tbf->GetNQueueDiscClasses() > 0)
            {
                fq = DynamicCast<FqCobaltQueueDisc>(tbf->GetQueueDiscClass(0)->GetQueueDisc());
            }
        }
        if (!fq)
        {
            NS_LOG_WARN("SetRttPreset: slot " << slot
                                              << " has no FqCobaltQueueDisc to configure; skipped");
            continue;
        }
        fq->SetAttribute("Target", StringValue(t.target));
        fq->SetAttribute("Interval", StringValue(t.interval));
    }
}

void
Helper::ConfigureLinkLayerOverhead(Ptr<EdgeQueueDisc> edge,
                                   uint32_t overhead,
                                   bool atm,
                                   bool ptm,
                                   uint32_t mpu,
                                   bool raw)
{
    NS_LOG_FUNCTION(edge << overhead << atm << ptm << mpu << raw);
    NS_ASSERT_MSG(edge, "ConfigureLinkLayerOverhead requires non-null edge");
    NS_ASSERT_MSG(!(atm && ptm), "ATM and PTM framing are mutually exclusive");

    if (raw)
    {
        // Linux `raw` flag — interpret configured `bandwidth` at the IP
        // layer. No per-tin TBF rate adjustment.
        return;
    }

    const double gamma = CakeGammaForBimodalMix(overhead, atm, ptm, mpu);

    // Walk every populated inner slot; if it wraps a TBF (i.e. the user
    // composed with `enableTinShaping=true` or `useInnerTbfShaping=true`),
    // downscale that TBF's `Rate` attribute by gamma. Untouched slots
    // (no TBF wrapper) are silently skipped — matches Linux semantics
    // where overhead correction is meaningful only when bandwidth-shaping
    // is active.
    for (uint32_t slot = 0; slot < edge->GetNumInnerSlots(); ++slot)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(slot);
        if (!inner)
        {
            continue;
        }
        Ptr<TbfQueueDisc> tbf = inner->GetObject<TbfQueueDisc>();
        if (!tbf)
        {
            continue;
        }
        // TbfQueueDisc's `Rate` attribute is bound to SetRate only — the
        // mainline accessor is asymmetric. Read via GetRate(), write via
        // SetRate() on the concrete type.
        const uint64_t scaledBps = static_cast<uint64_t>(tbf->GetRate().GetBitRate() / gamma);
        tbf->SetRate(DataRate(scaledBps));
    }
}

void
Helper::PrintTcStats(std::ostream& os, Ptr<const QueueDisc> edge) const
{
    NS_LOG_FUNCTION(this << edge);
    StatsFormatter::Print(os, edge);
}

void
Helper::AttachLiveBulkCounter(Ptr<EdgeQueueDisc> edge, Time idleWindow)
{
    NS_LOG_FUNCTION(edge << idleWindow);
    if (!edge)
    {
        return;
    }
    for (uint32_t slot = 0; slot < edge->GetNumInnerSlots(); ++slot)
    {
        Ptr<QueueDisc> inner = edge->GetInnerDiscAt(slot);
        if (!inner)
        {
            continue;
        }
        // Try direct cast first; if wrapped by TBF, walk one level deeper.
        Ptr<FqCobaltQueueDisc> fq = inner->GetObject<FqCobaltQueueDisc>();
        if (!fq)
        {
            Ptr<TbfQueueDisc> tbf = inner->GetObject<TbfQueueDisc>();
            if (tbf && tbf->GetNQueueDiscClasses() > 0)
            {
                fq = DynamicCast<FqCobaltQueueDisc>(tbf->GetQueueDiscClass(0)->GetQueueDisc());
            }
        }
        if (!fq)
        {
            NS_LOG_WARN("AttachLiveBulkCounter: slot "
                        << slot << " has no FqCobaltQueueDisc to attach to; skipped");
            continue;
        }
        Ptr<LiveBulkCounter> counter = CreateObject<LiveBulkCounter>();
        counter->Attach(fq, idleWindow);
        fq->AggregateObject(counter);
    }
}

uint32_t
Helper::GetLiveBulkCount(Ptr<EdgeQueueDisc> edge, uint32_t slot)
{
    NS_LOG_FUNCTION(edge << slot);
    if (!edge)
    {
        return 0;
    }
    Ptr<QueueDisc> inner = edge->GetInnerDiscAt(slot);
    if (!inner)
    {
        return 0;
    }
    Ptr<FqCobaltQueueDisc> fq = inner->GetObject<FqCobaltQueueDisc>();
    if (!fq)
    {
        return 0;
    }
    Ptr<LiveBulkCounter> counter = fq->GetObject<LiveBulkCounter>();
    if (!counter)
    {
        return 0;
    }
    return counter->GetLiveCount(Simulator::Now());
}

} // namespace ns3::stratum::cake
