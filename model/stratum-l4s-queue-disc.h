/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * L4S (RFC 9331 / 9332 DualPI2) queue disc — composition realization.
 */

#ifndef NS3_STRATUM_L4S_QUEUE_DISC_H
#define NS3_STRATUM_L4S_QUEUE_DISC_H

#include "stratum-red-queue-disc.h"
#include "stratum-scheduler.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/queue-disc.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traced-callback.h"

namespace ns3::stratum::l4s
{

/**
 * @ingroup stratum
 * @brief L4S / DualPI2 queue disc with composed classic-AQM strategy.
 *
 * `QueueDisc` descends directly from `ns3::QueueDisc` and composes
 * two child queue discs:
 *
 *   * `m_l4sQueue`    — default `FifoQueueDisc` with the L4S timestamp tag
 *   * `m_classicAqm`  — default `RedQueueDisc` (classic MRED + PHB)
 *
 * Both children are registered as `QueueDiscClass` entries (idx 0 = L4S,
 * idx 1 = classic) so the ns-3 base handles stats aggregation and child
 * drop forwarding automatically.
 *
 * The 2001 thesis (§3.3.2) called for a `dsDropper` strategy
 * symmetric to the `dsScheduler` strategy it actually extracted in
 * Figure 3.11. The inner classic AQM is pluggable: any
 * `Ptr<ns3::QueueDisc>` works via `SetClassicAqmDisc`. The default is a
 * `RedQueueDisc`; mainline `FqCoDelQueueDisc` is the alternative
 * shipped in v1.
 *
 * **DualPI2 state** (RFC 9332 §2.1 eq. (1); App. A.1 Fig. 6):
 *   * `p'`  — P.I base probability, updated on a periodic 16 ms tick
 *   * `p_L` — L4S mark prob: `min(k * p', 1)` coupled branch, stepping
 *     to 1.0 when a packet's own sojourn reaches the L4S target. Both the
 *     step and coupled marks are applied at dequeue, per packet
 *     (RFC 9332 App. A.1 step AQM); the coupled mark/drop is suppressed
 *     while the total queue is below two MTUs.
 *   * `p_C` — classic coupled drop prob: `p'^2` (k does not appear;
 *     the RFC couples via `p_C = (p_CL / k)^2` with `p_CL = k * p'`)
 *
 * **Stream discipline:** `ns3::QueueDisc` has no base `AssignStreams`. The
 * composer cascades explicitly via the new `AssignStreams` override,
 * descending into `m_classicAqm`'s inner `RedSubQueue` children so
 * the RED RNGs receive deterministic streams.
 *
 * **Briscoe atomic-DualQ compliance** (`draft-briscoe-tsvwg-l4s-diffserv-02`):
 * this queue disc is an indivisible DualQ per the §4 deployment
 * examples. It never alters the
 * DSCP of any packet (§7.1) and never alters the ECN codepoint except
 * by setting CE via `ns3::QueueDisc::Mark()` on the L4S lane (RFC 9331 §5.1,
 * Briscoe §7.1). DSCP classification lives in an outer
 * `diffserv::PolicyClassifier`; this inner queueing component only reads
 * headers, it does not rewrite them. The `GetNQueueDiscClasses() == 2`
 * invariant enforced by `CheckConfig` is the atomicity guarantee:
 * callers cannot split this DualQ into independent queue discs without
 * violating Briscoe §4.
 */
class QueueDisc : public ns3::QueueDisc, public QueueStatsProvider
{
  public:
    /**
     * @brief Classic-queue AQM strategy.
     *
     * Wred and CoupledOnly both assume the inner classic AQM is a
     * RedQueueDisc; the CoupledOnly pass-through munging in
     * DoInitialize only applies to Red sub-queues. FqCoDel swaps the
     * default inner disc to FqCoDelQueueDisc — Red-specific
     * forwarders (AddPhbEntry, ConfigQueue, SetMredMode, SetNumQueues,
     * SetNumPrec, SetMeanPacketSize, SetQueueBandwidth, SetQueueLimit
     * on the classic slot) assert and must not be called when FqCoDel
     * is selected.
     */
    enum class ClassicAqm
    {
        Wred,        //!< Parent WRED pipeline + coupled drop overlay.
        CoupledOnly, //!< Coupled drop is sole AQM; WRED early-drop suppressed.
        FqCoDel,     //!< Mainline FqCoDelQueueDisc as inner classic AQM.
    };

    /**
     * @brief Get the TypeId for this class.
     * @return the TypeId.
     */
    static TypeId GetTypeId();

    /** @brief Construct a QueueDisc with default composed children. */
    QueueDisc();

    ~QueueDisc() override;

    // --- Inner classic-AQM strategy injection ---

    /**
     * @brief Install a pre-built classic AQM strategy.
     *
     * Replaces the default inner classic AQM with the caller-provided
     * queue disc. Any `Ptr<ns3::QueueDisc>` is accepted; Red-specific
     * forwarders (AddPhbEntry, ConfigQueue, SetMredMode, etc.)
     * assert on non-Red inners, so callers installing a foreign AQM
     * must pre-configure it directly via its own API before this
     * call.
     *
     * @param aqm pre-built classic AQM queue disc
     */
    void SetClassicAqmDisc(Ptr<ns3::QueueDisc> aqm);

    /**
     * @brief Get the composed classic AQM strategy (never nullptr post-init).
     * @return the composed classic AQM queue disc
     */
    Ptr<ns3::QueueDisc> GetClassicAqmDisc() const;

    /**
     * @brief Install a pre-built L4S-lane queue disc.
     *
     * Default is FifoQueueDisc. Advanced users may swap to any FIFO-
     * compatible disc that accepts the L4S timestamp tag.
     *
     * @param l4s pre-built L4S-lane queue disc
     */
    void SetL4sQueueDisc(Ptr<ns3::QueueDisc> l4s);

    /**
     * @brief Get the composed L4S queue disc (never nullptr post-init).
     * @return the composed L4S-lane queue disc
     */
    Ptr<ns3::QueueDisc> GetL4sQueueDisc() const;

    /**
     * @brief Set the outer scheduler (2-queue view: idx 0 = L4S, idx 1 = classic).
     * @param scheduler the scheduler instance to install
     */
    void SetScheduler(Ptr<Scheduler> scheduler);

    /**
     * @brief Get the outer scheduler (may be nullptr until first enqueue).
     * @return the currently-installed outer scheduler
     */
    Ptr<Scheduler> GetScheduler() const;

    // --- PHB forwarders (preserve the 2001 RedQueueDisc caller API) ---

    /**
     * @brief Forwards to the classic AQM's PHB table.
     * @param codePt DSCP code point
     * @param queue physical queue index in the classic AQM
     * @param prec drop-precedence index
     */
    void AddPhbEntry(uint8_t codePt, uint8_t queue, uint8_t prec);

    /**
     * @brief Forwards to the classic AQM's PHB table.
     * @param codePt DSCP code point to look up
     * @param[out] queue physical queue index
     * @param[out] prec drop-precedence index
     * @return true if a matching entry exists, false otherwise
     */
    bool LookupPhb(uint8_t codePt, uint8_t& queue, uint8_t& prec) const;

    // --- Red-specific forwarders (DynamicCast under the hood) ---

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param numQueues number of physical queues
     */
    void SetNumQueues(uint32_t numQueues);

    /**
     * @brief QueueStatsProvider override — number of classic-AQM sub-queues.
     * @return number of top-level queues (Red inner only)
     */
    uint32_t GetNumQueues() const override;

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param cfg RED threshold configuration
     */
    void ConfigQueue(const RedQueueConfig& cfg);

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param mode MRED mode to install
     * @param q physical queue index
     */
    void SetMredMode(MredMode mode, uint32_t q);

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param q physical queue index
     * @param n number of precedence levels
     */
    void SetNumPrec(uint32_t q, uint32_t n);

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param q physical queue index
     * @param n buffer limit in packets
     */
    void SetQueueLimit(uint32_t q, uint32_t n);

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param mps mean packet size in bytes
     */
    void SetMeanPacketSize(int mps);

    /**
     * @brief Forwards to m_classicAqm as RedQueueDisc. Red-only.
     * @param q physical queue index
     * @param bps link bandwidth in bits per second
     */
    void SetQueueBandwidth(uint32_t q, double bps);

    /**
     * @brief Virtual queue length for sampling diagnostics.
     *
     * For the L4S slot (index `L4sQueueIdx`) this returns the FIFO
     * current size (precedence ignored). For classic slots this
     * forwards to the inner `RedQueueDisc::GetVirtualQueueLen`.
     */
    int GetVirtualQueueLen(uint32_t q, uint32_t prec) const;

    /**
     * @brief QueueStatsProvider override.
     *
     * Semantic mapping:
     *  - (L4sQueueIdx, _) -> L-queue FIFO occupancy (precedence ignored)
     *  - (other, prec) -> classic-AQM GetVirtualQueueLen(other, prec)
     *
     * @param queue scheduler-slot index
     * @param prec drop-precedence index (ignored for the L4S slot)
     * @return current queue length in packets
     */
    int GetQueueLen(uint32_t queue, uint32_t prec) const override
    {
        return GetVirtualQueueLen(queue, prec);
    }

    /** @brief QueueStatsProvider override — placeholder summary. */
    void PrintStats() const override;

    // --- L4S configuration ---

    /**
     * @brief L4S scheduler-slot index.
     *
     * The composer's direct children are always 2 and fixed
     * (`m_l4sQueue` + `m_classicAqm`), but the attached `Scheduler`
     * sees a 2-queue world whose slot indices are caller-chosen. `L4sQueueIdx` names the slot the
     * scheduler considers "L4S" — it must match the `l4sIdx` the user passes to
     * `CoupledScheduler`'s constructor. The composer dispatches
     * `OnEnqueueWithTime(L4sQueueIdx, ...)` on the L4S path and
     * `OnEnqueueWithTime(1 - L4sQueueIdx, ...)` on the classic path.
     */
    void SetL4sQueueIdx(uint32_t idx);

    /**
     * @brief Get the scheduler-slot index assigned to the L4S lane.
     * @return the scheduler-slot index (0 or 1)
     */
    uint32_t GetL4sQueueIdx() const;

    /**
     * @brief Set the L4S step-marking threshold (Linux/GPRT default 1 ms).
     * @param ms target sojourn time in milliseconds
     */
    void SetL4sTargetSojournMs(double ms);

    /**
     * @brief Get the L4S target sojourn time.
     * @return target sojourn time in milliseconds
     */
    double GetL4sTargetSojournMs() const;

    /**
     * @brief Set the classic-queue target sojourn time used by the
     *        P.I.² controller. RFC 9332 App. A.1 default 15 ms.
     * @param ms classic-queue target sojourn time in milliseconds
     */
    void SetClassicTargetSojournMs(double ms);

    /**
     * @brief Get the classic-queue target sojourn time.
     * @return classic-queue target sojourn time in milliseconds
     */
    double GetClassicTargetSojournMs() const;

    /**
     * @brief Set the coupling factor k; p_CL = k * p' on the L4S lane
     *        (RFC 9332 §2.1 eq. (1); p_C = p'^2 is k-independent).
     *        Default k = 2.
     * @param k coupling factor
     */
    void SetCouplingFactor(double k);

    /**
     * @brief Get the coupling factor k.
     * @return the coupling factor
     */
    double GetCouplingFactor() const;

    /**
     * @brief Set the classic-queue AQM strategy (Wred / CoupledOnly enum variant).
     * @param m the ClassicAqm variant to select
     */
    void SetClassicAqm(ClassicAqm m);

    /**
     * @brief Get the classic-queue AQM strategy.
     * @return the currently-selected ClassicAqm variant
     */
    ClassicAqm GetClassicAqm() const;

    /**
     * @brief Set the bandwidth proxy used when per-packet sojourn is unavailable.
     * @param bps link bandwidth in bits per second
     */
    void SetL4sBandwidthBps(double bps);

    /**
     * @brief Get the bandwidth proxy.
     * @return link bandwidth in bits per second
     */
    double GetL4sBandwidthBps() const;

    /**
     * @brief Set the P.I controller tick interval (RFC 9332 default 16 ms).
     * @param interval controller tick interval
     */
    void SetControllerInterval(Time interval);

    /**
     * @brief Get the P.I controller tick interval.
     * @return controller tick interval
     */
    Time GetControllerInterval() const;

    // --- Inspection / test hooks ---

    /**
     * @brief Inspection: current base probability p'.
     * @return the base probability
     */
    double GetBaseProb() const;

    /**
     * @brief Inspection: last coupled drop probability p_C applied on
     * the classic lane.
     * @return the last coupled drop probability
     */
    double GetLastClassicCoupledProb() const;

    /**
     * @brief Inspection: last L4S mark probability p_L applied on the
     * L4S lane.
     * @return the last L4S mark probability
     */
    double GetLastL4sMarkProb() const;

    /**
     * @brief Test hook: force p' to a fixed value.
     * @param p forced base probability in [0, 1]
     */
    void ForceBaseProbForTest(double p);

    /** @brief Test hook: clear the forced base probability. */
    void ClearForcedBaseProbForTest();

    /**
     * @brief Cascade RNG streams into self + child sub-queues.
     *
     * Assigns @p stream to the composer's own coupling RNG, then
     * descends into m_classicAqm (as RedQueueDisc) and assigns
     * stream+1.. to its RedSubQueue children. m_l4sQueue
     * (FifoQueueDisc) has no RNG.
     *
     * @param stream starting RNG stream index
     * @return number of streams consumed (1 + number of inner RED sub-queues)
     */
    int64_t AssignStreams(int64_t stream);

    typedef void (*ClassifiedTracedCallback)(Ptr<const QueueDiscItem> item, bool isL4s);
    typedef void (*L4sMarkedTracedCallback)(Ptr<const QueueDiscItem> item);
    typedef void (*ClassicCoupledDropTracedCallback)(Ptr<const QueueDiscItem> item, double pC);

  protected:
    bool DoEnqueue(Ptr<QueueDiscItem> item) override;
    Ptr<QueueDiscItem> DoDequeue() override;
    Ptr<const QueueDiscItem> DoPeek() override;
    bool CheckConfig() override;
    void InitializeParams() override;
    void DoInitialize() override;
    void DoDispose() override;

  private:
    /**
     * @brief Determine whether a packet is L4S-eligible (ECT(1) or CE).
     * @param item the queue disc item
     * @return true iff the packet should be routed to the L4S lane
     */
    bool IsL4sPacket(Ptr<const QueueDiscItem> item) const;

    /** @brief Periodic P.I controller tick (RFC 9332). */
    void ControllerTick();

    /** @brief Update the base probability p' from the latest sojourn sample. */
    void UpdateBaseProb();

    /**
     * @brief Compute a packet's own sojourn time from its enqueue timestamp.
     *
     * Reads the `TimestampTag` stamped on @p item at enqueue and returns
     * `Now() - tag` in milliseconds. Used by the dequeue-time step mark so
     * each packet is judged against its own age (RFC 9332 App. A.1 step
     * AQM), not the head's.
     *
     * @param item the dequeued queue disc item
     * @return the item's sojourn time in milliseconds (or 0 if untagged)
     */
    double ComputeItemSojournMs(Ptr<const QueueDiscItem> item) const;

    /**
     * @brief Total bytes currently buffered across both sub-queues.
     * @return the combined L4S + classic occupancy in bytes
     */
    uint32_t TotalQueueBytes() const;

    /**
     * @brief Compute the current classic-queue sojourn time. Drives the
     *        RFC 9332 App. A.1 P.I.² controller via UpdateBaseProb().
     * @return sojourn time in milliseconds (or 0 if unavailable)
     */
    double ComputeClassicSojournMs() const;

    /**
     * @brief Apply the L4S coupled mark (ECN CE) to @p item if eligible.
     * @param item queue disc item to mark
     * @return true iff CE was set
     */
    bool ApplyL4sCoupledMark(Ptr<QueueDiscItem> item);

    /**
     * @brief Compute the classic coupled drop probability p_C = p'^2
     *        (RFC 9332 App. A.1 Fig. 6 line 5).
     * @return the coupled drop probability
     */
    double ComputeCoupledDropProb() const;

    /**
     * @brief Drop @p item with probability p_C.
     * @param item queue disc item considered for coupled drop
     * @return true iff the item was dropped
     */
    bool MaybeCoupledDrop(Ptr<QueueDiscItem> item);

    /**
     * @brief Enqueue a packet on the L4S lane.
     * @param item queue disc item to enqueue
     * @return true if enqueued, false if dropped
     */
    bool EnqueueL4s(Ptr<QueueDiscItem> item);

    /** @brief Ensure composer children exist; called lazily from CheckConfig. */
    void EnsureDefaultChildren();

    /**
     * @brief Resolve classic AQM as RedQueueDisc, asserting on
     *        non-Red inners.
     * @return classic AQM cast to RedQueueDisc
     */
    Ptr<RedQueueDisc> GetClassicAsRed() const;

    // Composed children (kMaxChildrenIdx: outer-scheduler view)
    static constexpr uint32_t kL4sChildIdx = 0;     //!< Composer child-slot of m_l4sQueue
    static constexpr uint32_t kClassicChildIdx = 1; //!< Composer child-slot of m_classicAqm

    Ptr<ns3::QueueDisc> m_l4sQueue;   //!< L4S lane (FifoQueueDisc by default)
    Ptr<ns3::QueueDisc> m_classicAqm; //!< Classic AQM strategy (RedQueueDisc)
    Ptr<Scheduler> m_scheduler;       //!< Outer scheduler (2-queue view)

    // Configuration (attributes)
    uint32_t m_l4sQueueIdxLegacy;    //!< Round-tripped for deprecated attribute
    double m_l4sTargetSojournMs;     //!< L4S target sojourn time (ms)
    double m_classicTargetSojournMs; //!< Classic-queue target sojourn time (ms); RFC 9332
                                     //!< App. A.1 default 15
    double m_couplingFactor;         //!< Coupling factor k
    ClassicAqm m_classicAqmMode;     //!< Selected classic AQM strategy
    double m_l4sBandwidthBps;        //!< Bandwidth proxy (bits/second)
    uint32_t m_mtu;                  //!< MTU (bytes); the coupled signal is suppressed
                                     //!< while the total queue sits below 2 * m_mtu

    // P.I controller state
    double m_baseProb;             //!< Base probability p'
    Time m_controllerInterval;     //!< Controller tick interval
    EventId m_controllerEvent;     //!< Pending controller tick event
    double m_lastSojournMs; //!< Previous classic sojourn sample (ms); starts at 0 (RFC prevq=0)

    // Test hook
    bool m_forceBaseProbForTest; //!< True while a test override is active on m_baseProb

    //! True once the caller has touched any classic-AQM config (SetQueueLimit,
    //! ConfigQueue, SetMredMode, AddPhbEntry). When true, DoInitialize skips
    //! the Wred-mode default-mitigation block so user config wins. When
    //! false, defaults are applied so a fresh `Wred` enum picker gets a
    //! functional classic queue without further setup.
    bool m_classicUserConfigured = false;

    // Inspection snapshots
    double m_lastCoupledProb; //!< Last coupled drop probability applied
    double m_lastL4sMarkProb; //!< Last L4S mark probability applied

    // Random source for coupling draws
    Ptr<UniformRandomVariable> m_rng; //!< Uniform RNG for coupling-draw decisions

    // Trace sources
    TracedCallback<Ptr<const QueueDiscItem>, bool>
        m_l4sClassifiedTrace;                                //!< Classified (L4S vs classic) trace
    TracedCallback<Ptr<const QueueDiscItem>> m_l4sMarkTrace; //!< L4S CE-mark trace
    TracedCallback<Ptr<const QueueDiscItem>, double>
        m_classicCoupledDropTrace; //!< Classic coupled-drop trace
};

} // namespace ns3::stratum::l4s

#endif // NS3_STRATUM_L4S_QUEUE_DISC_H
