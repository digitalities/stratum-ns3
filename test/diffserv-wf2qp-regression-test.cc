/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * WF2Q+ no-stall regression smoke fixture.
 *
 * Bennett & Zhang (1996) WF2Q+ uses an eligibility predicate
 * S_i <= V at dequeue. Correctness requires the Eq. (22)
 * virtual-time floor V := max(V_continuous, min_{i in active} S_i)
 * be applied at every busy-set transition; without it, V can fall
 * behind the minimum start tag of the active set and the predicate
 * rejects every backlogged packet, stalling the dispatcher.
 *
 * This fixture exercises a synthetic enqueue-then-drain pattern at
 * symmetric and asymmetric weights and asserts that
 * SelectNextQueue() never returns -1 while the scheduler is
 * backlogged. It is the regression canary for any V(t)-bookkeeping
 * change to Wf2qPlusScheduler that fails to apply the Eq. (22)
 * floor at both the enqueue and dequeue busy-set transitions.
 *
 * Reference: A. K. Parekh and R. G. Gallager, "A Generalized
 * Processor Sharing Approach to Flow Control in Integrated Services
 * Networks: The Single-Node Case," IEEE/ACM Trans. Networking,
 * vol. 1, no. 3, pp. 344-357, June 1993, Eq. 10 (V(t)).
 *
 * J. Bennett and H. Zhang, "WF2Q: Worst-case Fair Weighted Fair
 * Queueing," Proc. IEEE INFOCOM, 1996, Eq. (22) (V floor) and
 * Eq. (28) (eligibility predicate).
 */

#include "ns3/core-module.h"
#include "ns3/stratum-wf2qp-scheduler.h"
#include "ns3/test.h"

#include <sstream>
#include <string>
#include <vector>

using namespace ns3;
using ns3::stratum::Wf2qPlusScheduler;

namespace
{

struct Wf2qpSmokeState
{
    Ptr<Wf2qPlusScheduler> sched;
    std::vector<int> dequeues;
    uint32_t stalls{0};
};

void
DriveEnqueue(Wf2qpSmokeState* s, uint32_t qIdx, uint32_t sizeBytes)
{
    s->sched->OnEnqueueWithTime(qIdx, sizeBytes, Simulator::Now().GetSeconds());
}

void
DriveDequeue(Wf2qpSmokeState* s)
{
    int q = s->sched->SelectNextQueue();
    if (q < 0)
    {
        ++s->stalls;
    }
    else
    {
        s->dequeues.push_back(q);
    }
}

/// Synthetic enqueue-then-drain at the configured weights. Asserts no
/// stall and full drain.
class Wf2qpDrainNoStallCase : public TestCase
{
  public:
    Wf2qpDrainNoStallCase(const std::string& tag, double w0, double w1)
        : TestCase("WF2Q+ drain no-stall (" + tag + ")"),
          m_w0(w0),
          m_w1(w1),
          m_tag(tag)
    {
    }

  private:
    void DoRun() override
    {
        constexpr double kBwBps = 10e6;
        constexpr uint32_t kPktBytes = 1500;
        constexpr uint32_t kPktsPerFlow = 50;
        const double kPktServiceTime = (kPktBytes * 8.0) / kBwBps; // 1.2 ms

        Wf2qpSmokeState state;
        state.sched = CreateObjectWithAttributes<Wf2qPlusScheduler>("NumQueues",
                                                                    UintegerValue(2),
                                                                    "LinkBandwidth",
                                                                    DoubleValue(kBwBps));
        state.sched->SetParam(0, m_w0);
        state.sched->SetParam(1, m_w1);

        // Enqueue 50 packets per flow at t=0 (interleaved).
        for (uint32_t k = 0; k < kPktsPerFlow; ++k)
        {
            Simulator::Schedule(Seconds(0.0), &DriveEnqueue, &state, 0u, kPktBytes);
            Simulator::Schedule(Seconds(0.0), &DriveEnqueue, &state, 1u, kPktBytes);
        }

        // Drive 2*kPktsPerFlow dequeues at packet-service intervals,
        // starting after all enqueues have fired at t=0.
        for (uint32_t k = 0; k < 2 * kPktsPerFlow; ++k)
        {
            Simulator::Schedule(Seconds(0.001 + k * kPktServiceTime), &DriveDequeue, &state);
        }

        Simulator::Stop(Seconds(1.0));
        Simulator::Run();

        std::ostringstream stallMsg;
        stallMsg << "WF2Q+ stalled (SelectNextQueue returned -1) " << state.stalls
                 << " times mid-drain at weights " << m_w0 << "/" << m_w1
                 << " — Eq. (22) virtual-time floor likely missing at a busy-set transition";
        NS_TEST_ASSERT_MSG_EQ(state.stalls, 0u, stallMsg.str());

        std::ostringstream drainMsg;
        drainMsg << "WF2Q+ did not drain all packets at weights " << m_w0 << "/" << m_w1
                 << ": expected " << 2 * kPktsPerFlow << " dequeues, got " << state.dequeues.size();
        NS_TEST_ASSERT_MSG_EQ(state.dequeues.size(), 2 * kPktsPerFlow, drainMsg.str());

        Simulator::Destroy();
    }

    double m_w0;
    double m_w1;
    std::string m_tag;
};

class DiffServWf2qpRegressionSuite : public TestSuite
{
  public:
    DiffServWf2qpRegressionSuite()
        : TestSuite("stratum-wf2qp-regression", Type::UNIT)
    {
        AddTestCase(new Wf2qpDrainNoStallCase("symmetric", 0.5, 0.5), Duration::QUICK);
        AddTestCase(new Wf2qpDrainNoStallCase("asymmetric-1to10", 1.0 / 11.0, 10.0 / 11.0),
                    Duration::QUICK);
    }
};

static DiffServWf2qpRegressionSuite g_diffServWf2qpRegressionSuite;

} // namespace
