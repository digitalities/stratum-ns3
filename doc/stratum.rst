.. highlight:: cpp

.. heading hierarchy:
   ------------- Chapter
   ************* Section (#.#)
   ============= Subsection (#.#.#)
   ############# Paragraph (no number)

Stratum contrib module
----------------------

.. _stratum:

Model Description
*****************

The ``stratum`` module (historically ``diffserv4ns3``, then
``diffserv``; renamed 2026-06-06 alongside the namespace move to
``ns3::stratum``) is an *ns-3* contrib module that provides a
DiffServ architecture supporting classic and L4S
service-differentiation models, as defined by RFC 2474 / RFC 2475
(DiffServ architecture and service classes) and RFC 9331 / RFC 9332
(L4S architecture and Dual-Queue Coupled AQM).  It is a port of
**DiffServ4NS** (Sergio Andreozzi, 2001), originally developed as an
*ns-2* patch, extended in 2026 with native L4S / DualPI2 support.
The module is GPLv2-licensed and feature-complete relative to the
original DiffServ4NS-0.1 release.

The source code is located in the ``contrib/stratum`` directory and
is organised into the standard *ns-3* module layout (``model/``,
``helper/``, ``test/``, ``examples/``).

The module provides the following components:

Meters
======

Nine meter classes implementing traffic metering and policing algorithms.
All meters derive from the abstract ``Meter`` base class.

* ``DumbMeter`` -- pass-through (no metering)
* ``TokenBucketMeter`` -- single token bucket
* ``SrTcmMeter`` -- Single Rate Three Color Marker (RFC 2697)
* ``TrTcmMeter`` -- Two Rate Three Color Marker (RFC 2698)
* ``Tsw2cmMeter`` -- Time Sliding Window Two Color Marker
* ``Tsw3cmMeter`` -- Time Sliding Window Three Color Marker (RFC 2859)
* ``FWMeter`` -- per-flow byte-accounting meter (ported from ns-2 SFDPolicy)

Queue management
================

* ``RedQueueDisc`` -- the central classful queue disc holding N
  ``RedSubQueue`` children (each wrapped in a ``QueueDiscClass``), a PHB
  table mapping DSCP code points to (queue, precedence) pairs, and a pluggable
  scheduler.
* ``RedSubQueue`` -- one physical queue with per-precedence MRED supporting
  four modes: ``RIO_C`` (coupled), ``RIO_D`` (decoupled), ``WRED`` (weighted
  RED), and ``DROP_TAIL``.

Schedulers
==========

Seven scheduling disciplines, all subclasses of the abstract ``Scheduler``.
The scheduler is a pluggable strategy object installed on a ``RedQueueDisc``.

* ``RoundRobinScheduler`` -- round-robin
* ``WeightedRoundRobinScheduler`` -- weighted round-robin
* ``WeightedInterleavedRoundRobinScheduler`` -- interleaved weighted round-robin
* ``PriorityScheduler`` -- strict priority (with optional per-queue rate cap)
* ``ScfqScheduler`` -- Self-Clocked Fair Queueing (SCFQ)
* ``SfqScheduler`` -- Stochastic Fairness Queueing (SFQ)
* ``Wf2qPlusScheduler`` -- Worst-case Fair Weighted Fair Queueing (WF2Q+)
* ``WfqScheduler`` -- Weighted Fair Queueing (WFQ)
* ``LlqScheduler`` -- Low-Latency Queueing (LLQ = strict-priority + WFQ)

Edge / Core pipeline
====================

* ``EdgeQueueDisc`` -- edge router queue disc: classify (mark rule
  table) then meter/police (``diffserv::PolicyClassifier``) then enqueue.  On
  dequeue the IPv4 header TOS field is rewritten with the policed DSCP.
  Descends directly from ``QueueDisc`` and composes a single
  ``Ptr<RedQueueDisc>`` inner for the queueing pipeline (ADR-0030, PR3a).
* ``CoreQueueDisc`` -- core router queue disc: a thin ``QueueDisc``
  composer wrapping an inner ``RedQueueDisc`` (ADR-0030, PR3a); relies on
  the DSCP already set by the upstream edge and performs no classification
  or metering of its own.

Tags
====

Per-packet metadata is carried via *ns-3* ``Tag`` objects rather than by
modifying IP header fields during internal processing:

* ``DscpTag`` -- carries the policed DSCP code point
* ``SendTimeTag`` -- records packet send time for OWD/IPDV measurement

Helpers
=======

* ``diffserv::Helper`` -- fluent configuration API for adding policy
  entries, policer entries, PHB entries, scheduler assignments, and RED
  thresholds.  Rates are accepted in bits/s.  Mark rules are added directly
  on ``EdgeQueueDisc`` via ``edge->AddMarkRule({.dscp = N, ...})``.
* ``MonitorHelper`` -- connects ``RedQueueDisc`` trace sources to
  ``Statistics`` and writes periodic metric traces (departure rate,
  queue length) to files.
* ``cake::Helper`` -- three static composers
  (``SetAsCakeDiffserv3``, ``SetAsCakeDiffserv4``, ``SetAsCakeDiffserv8``)
  that wire a ``EdgeQueueDisc`` for the corresponding Linux
  ``tc-cake(8)`` profile.  Each composer installs per-tin mainline
  ``FqCobaltQueueDisc`` inners (8-way set-associative hash,
  ``Quantum=1514``) plus a ``cake::TinShaperDispatcher`` with
  share-proportional DRR quanta, and stamps the byte-exact DSCP-to-tin
  map.  See ADR-0042.

  Each composer accepts three optional opt-in flags after the mandatory
  ``edge`` and ``totalRate`` arguments:

  * ``enableAckFilter`` -- when ``true``, each per-tin
    ``FqCobaltQueueDisc`` is constructed with ``EnableAckFilter=true``.
    The ACK-filter functionality lives in mainline ``FqCobaltQueueDisc``
    via the patch carried at
    ``patches/ns3/0006-fq-cobalt-ack-filter-memlimit.patch`` (filed
    upstream; pin advances and the patch retires once merged). Default
    ``false``.
  * ``enableLlq`` -- when ``true``, the across-slot dispatcher is
    swapped from ``cake::TinShaperDispatcher`` to ``HybridLlqDispatcher``
    and the latency-sensitive tin (Voice in ``diffserv4``,
    Latency-Sensitive in ``diffserv3``, CS6/EF/VA in ``diffserv8``) is
    served strict-priority.  Other tins remain DRR.  Sub-30 ms p99 OWD
    for EF probes on a saturated RRUL workload; idle-SP capacity still
    redistributes to busy DRR tins.  Default ``false`` -- byte-identical
    to Phase 7 behaviour.  See ADR-0044.
  * ``enableTinShaping`` -- when ``true``, each tin's serve rate is
    hard-capped at ``share × totalRate`` (matching Linux ``tc-cake
    bandwidth N <profile>``, the production default).  Composes
    orthogonally with ``enableLlq`` -- ``enableLlq && enableTinShaping``
    produces the Cisco MQC LLQ pattern (priority class with hard
    ceiling preventing EF starvation).  Default ``false`` ->
    byte-identical to Phase 8 behaviour.  See ADR-0045.
  * ``enableHostIsolation`` -- when ``true``, each tin's per-flow
    inner ``FqCobaltQueueDisc`` is configured with the host-isolation
    attribute surface (``EnableHostIsolation=true``,
    ``HostIsolationMode=Triple``) supplied by
    ``patches/ns3/0016-fq-cobalt-host-isolation.patch``.  Per-host
    bulk counts are kept in separate src-side and dst-side slot
    tables and combined via ``max(srcCount, dstCount)`` -- matching
    Linux ``sch_cake.c`` (``tc-cake(8)`` ``triple-isolate``
    semantics).  A single host with N concurrent flows receives the
    same total bandwidth share as a host with one flow.  Default
    ``false`` -> mainline ``FqCobaltQueueDisc`` behaviour with
    host-isolation disabled.

  Post-composition helpers (called after ``SetAsCake*``):

  * ``SetLinkLayer(edge, LinkPreset)`` -- applies one of 15 named
    Linux ``tc-cake(8)`` link-layer overhead presets (``Raw``,
    ``Ethernet``, ``EtherVlan``, ``Docsis``, ``PppoePtm``,
    ``BridgedPtm``, etc.) sourced from the iproute2
    ``q_cake.c`` keyword table.  Resolves to
    ``ConfigureLinkLayerOverhead`` internally.
  * ``ConfigureLinkLayerOverhead(edge, overhead, atm, ptm, mpu, raw)``
    -- fine-grained framing control; ``ptm=true`` selects PTM
    64b/65b linear cell framing.
  * ``SetRttPreset(edge, RttPreset)`` -- sets ``Target`` and
    ``Interval`` on every ``FqCobaltQueueDisc`` inner from one of
    8 named presets (``Datacentre``, ``Lan``, ``Metro``, ``Regional``,
    ``Internet``, ``Oceanic``, ``Satellite``, ``Interplanetary``).
  * ``AttachLiveBulkCounter(edge, idleWindow)`` -- attaches a
    ``cake::LiveBulkCounter`` to each inner, replacing the
    ``ever_seen`` approximation with a live idle-window count.
    Call after ``Initialize``.  Read back via
    ``GetLiveBulkCount(edge, slot)``.
  * ``SetEnableIngressMode(bool)`` -- when ``true`` and
    ``ShaperMode`` is ``RateBased``, virtual clocks advance on
    dropped packets as well as forwarded ones, matching Linux
    ``tc-cake ingress`` behaviour.
  * ``SetAutorateImpl(AutorateImpl)`` -- selects the autorate-ingress
    implementation: ``NoOp`` (default, API-only skeleton) or
    ``Linux`` (peak-rate EWMA closed-loop faithful to
    ``sch_cake.c::cake_enqueue``).

Monitoring
==========

* ``Statistics`` -- per-DSCP packet and byte counters for enqueue,
  dequeue, RED drops, and tail drops, plus OWD/IPDV accumulators.
* ``TracedCallback`` sources on ``RedQueueDisc``: ``StratumEnqueue``,
  ``StratumDequeue``, ``StratumDrop``.


Design
******

Architecture
============

Substrate classes reside in the ``ns3::stratum`` namespace; each of the
three clients adds a nested namespace for its client-specific classes:
``ns3::stratum::diffserv`` (policy classifiers and the configuration
helper), ``ns3::stratum::l4s`` (the coupled dual-queue), and
``ns3::stratum::cake`` (tin shaping, autorate hooks, and the CAKE
helper).

``RedQueueDisc`` is the central classful ``QueueDisc``.  It holds:

1. N ``RedSubQueue`` children (each inside a ``QueueDiscClass``), where each
   sub-queue maintains up to three virtual queues with independent RED
   parameters per drop-precedence level.
2. A **PHB table** that maps each DSCP code point to a (physical queue index,
   drop precedence) pair.
3. A **pluggable scheduler** (``Scheduler`` subclass) that decides which
   queue to dequeue from next.

The scheduler is a strategy object set via ``SetScheduler()``.  On enqueue the
disc notifies the scheduler (``OnEnqueue`` / ``OnEnqueueWithTime``); on dequeue
it polls ``SelectNextQueue()``.  Fair-queueing schedulers (SCFQ, SFQ, WF2Q+,
WFQ, LLQ) use the time-aware notification to maintain virtual clocks and
finish-time bookkeeping.

``EdgeQueueDisc`` composes a single inner ``RedQueueDisc`` at
child slot 0 (ADR-0030 / PR3a, 2026-04-18) and adds a multi-field classifier
(``MarkRule`` table) plus a ``diffserv::PolicyClassifier`` that holds meter
and policer state per traffic aggregate.  On enqueue:

1. The packet is classified against the mark rules to obtain an initial DSCP.
2. The ``diffserv::PolicyClassifier`` meters the packet, decides a colour, and
   maps the colour to a final DSCP via the policer table.
3. A ``DscpTag`` is attached carrying the final DSCP.
4. The composer delegates to ``m_inner->Enqueue(item)``; the inner reads the
   tag (symmetric with its tag-first ``DoDequeue``) and uses the PHB table
   to select the target queue.

``CoreQueueDisc`` is a ``QueueDisc`` composer with the same shape
as the edge, minus the classification.  It provides a distinct ``TypeId``
for topology configuration; per the thesis §3.3.1 edge/core split it
performs no classification or metering.

Per-packet metadata uses *ns-3* ``Tag`` objects (not header modification)
during internal queue processing.  The IPv4 header is rewritten only on dequeue
at the edge disc.

Scope and limitations
=====================

* **Colour-blind mode only.**  All meters operate in colour-blind mode; there
  is no colour-aware metering path.
* **Address-family agnostic.**  The DS-field and ECN handling is address-family
  agnostic: DiffServ, L4S, and CAKE operate identically over IPv4 and IPv6
  (RFC 2474 defines the same DS octet for both).
* **Structural OWD offset vs. ns-2.**  One-way delay measurements show a
  systematic offset relative to *ns-2* baselines.  This is caused by *ns-3*
  NetDevice queue serialisation delay (absent in *ns-2*) and by differences in
  packet size accounting between the two simulators.


Usage
*****

Building
========

The module builds from the *ns-3* root directory.  A symlink into *ns-3*'s
``contrib/`` directory is created automatically by ``scripts/fetch-ns3.sh``.

.. sourcecode:: bash

   # First-time setup (clones ns-3-dev and creates the symlink)
   ./scripts/fetch-ns3.sh

   # Build
   cd ns3/ns-3-dev
   ./ns3 configure --enable-tests --enable-examples
   ./ns3 build stratum

   # Run all tests
   python3 test.py -s stratum -v

   # Run a single test by name fragment
   python3 test.py -s stratum -v -r "S-2 srTCM"

Configuring an edge router
==========================

A typical edge router configuration uses ``diffserv::Helper``:

.. sourcecode:: cpp

   #include "ns3/stratum-edge-queue-disc.h"
   #include "ns3/stratum-helper.h"
   #include "ns3/stratum-pq-scheduler.h"

   using namespace ns3;
   using ns3::stratum::EdgeQueueDisc;
   namespace diffserv = ns3::stratum::diffserv;

   // Create the edge queue disc
   Ptr<EdgeQueueDisc> edge = CreateObject<EdgeQueueDisc>();

   diffserv::Helper helper;

   // Mark rule: classify EF traffic (DSCP 46) — UDP, any source/destination
   edge->AddMarkRule({.dscp = 46, .protocol = 17});

   // srTCM policy for DSCP 46: CIR=500 kbps, CBS=3000 B, EBS=3000 B
   helper.AddSrTcmPolicy(edge, {.codePt = 46, .cirBps = 500000.0, .cbsBytes = 3000.0, .ebsBytes = 3000.0});

   // Policer: GREEN -> 46, YELLOW -> 0, RED -> 0
   helper.AddPolicerEntry(edge, {.policer = PolicerType::SRTCM, .initialCodePt = 46, .downgrade1 = 0, .downgrade2 = 0, .policyIndex = static_cast<uint32_t>(PolicerType::SRTCM)});

   // PHB table: DSCP 46 -> queue 0, prec 0
   helper.AddPhbEntry(edge, 46, 0, 0);

   // Install a priority scheduler
   Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>(
       "NumQueues", UintegerValue(2));
   helper.SetScheduler(edge, pq);

   // Configure RED thresholds for queue 0, prec 0
   helper.ConfigQueue(edge, {.queue = 0, .prec = 0, .thMin = 5.0, .thMax = 10.0, .maxP = 0.02});

Configuring a core router
=========================

Core routers require only PHB table entries and a scheduler; they do not
classify or meter:

.. sourcecode:: cpp

   Ptr<CoreQueueDisc> core = CreateObject<CoreQueueDisc>();

   diffserv::Helper helper;
   helper.AddPhbEntry(core, 46, 0, 0);   // EF -> queue 0
   helper.AddPhbEntry(core, 0, 1, 0);    // BE -> queue 1

   Ptr<PriorityScheduler> pq = CreateObjectWithAttributes<PriorityScheduler>(
       "NumQueues", UintegerValue(2));
   helper.SetScheduler(core, pq);


Examples
========

Three examples reproduce the simulation scenarios from the 2001 thesis.  All
share the same 13-node topology (5 sources, 3 routers, 5 destinations) with a
2 Mbps / 5 ms bottleneck link between e1 and the core.

Example 1 — Scenario 1: EF/BE with scheduler sweep
###################################################

``examples/diffserv-example-1.cc`` configures EF and BE traffic classes with
srTCM and token-bucket policers under priority scheduling (and optionally
under any of the six fair-queueing schedulers: WFQ, SCFQ, SFQ, WF2Q+, LLQ).
This is a direct port of ``ns2/diffserv4ns/examples/example-1/simulation-1.tcl``.

.. sourcecode:: bash

   $ ./ns3 run "diffserv-example-1"
   $ ./ns3 run "diffserv-example-1 --scheduler=SCFQ"

Example 2 — Three-class AF/WRED scenario
#########################################

``examples/diffserv-example-2.cc`` exercises three service classes (Premium/EF,
Gold/AF, Best Effort) with RIO-C (WRED) dropping for the AF queue, TSW2CM
metering for FTP traffic, and port-based classification (Telnet=port 23,
FTP=port 21).  TCP traffic is generated using standard *ns-3* applications:
``ns3::OnOffApplication`` for Telnet and ``BulkSendApplication`` for FTP.  Three
scheduler options are available: PQ, SCFQ, and LLQ.

This is a port of ``ns2/diffserv4ns/examples/example-2/example-2.tcl``.

.. sourcecode:: bash

   $ ./ns3 run "diffserv-example-2 --scheduler=PQ"
   $ ./ns3 run "diffserv-example-2 --scheduler=LLQ"

The scenario validates AF drop-precedence differentiation: AF11 (Telnet)
receives 0% packet loss, while AF13 (FTP out-of-profile) receives 31–50%
early drops from RIO-C, consistent with the thesis Table 4.4 results.

Example 3 — Complete service model (thesis §4.3 reconstruction)
###############################################################

``examples/diffserv-example-3.cc`` reconstructs the complete service model
from thesis Section 4.3.  The original Tcl script was never published; this
example is a scaled-down reconstruction based on Table 4.5 and the prose
description.  Five service classes are deployed simultaneously:

* **Premium** (EF): VoIP traffic (G.723.1 codec, 6.3 kbps, ON/OFF), token
  bucket CIR=500 kbps, tail-drop
* **Gold** (AF11/AF12): Audio streaming, TSW2CM CIR=600 kbps, RIO-C
* **Silver** (AF21/AF22): Telnet + FTP, WRED (maxP=0.1/0.2)
* **Bronze** (AF31): HTTP, WRED (maxP=0.5)
* **Best Effort** (Default): Background CBR, token bucket CIR=400 kbps

The scheduler is LLQ with SFQ sub-scheduler (weights 3:3:3:1 for
Gold/Silver/Bronze/BE).

.. sourcecode:: bash

   $ ./ns3 run "diffserv-example-3"

Results are qualitatively consistent with the thesis: VoIP gets strict
priority (queue always at 0 packets, thesis: "never overcomes 10"), WRED
provides intra-class differentiation (Telnet < FTP in absolute drops), and
SFQ distributes excess bandwidth among Olympic services.

All examples produce trace files (ServiceRate, ClassRate, QueueLen, OWD, IPDV)
in the output directory.

Wireless examples
#################

Three examples demonstrate that the DiffServ edge composes above
``WifiNetDevice`` with no module-side changes.  ns-3 mainline's
``qos-utils.cc::QosUtilsMapTidToAc`` performs the RFC 8325 DSCP→802.11e
UP mapping automatically once the DSCP byte is stamped, so no DiffServ-
side helper is involved on the Wi-Fi side.

* ``examples/diffserv-wifi-demo.cc`` — minimal demo (1 AP, 2 STAs,
  802.11ax, ``QosSupported=true``) with a DiffServ edge stamping EF and
  BE flows on the AP downlink.  EF and BE survive across the wired→
  wireless transition and ride the per-AC EDCA path automatically.

* ``examples/diffserv-wifi-scheduler-comparison.cc`` — eight
  DiffServ schedulers (``pq|rr|wrr|wirr|scfq|wfq|wf2qp|llq``) over the
  same 802.11a 6 Mb/s topology with ``QosSupported=false`` so the qdisc
  is the only differentiating mechanism.  ``--singleAcSaturation``
  flag runs a single-station saturation Bianchi 2000 sanity check.

  .. sourcecode:: bash

     $ ./ns3 run "diffserv-wifi-scheduler-comparison --scheduler=pq"
     $ ./ns3 run "diffserv-wifi-scheduler-comparison --scheduler=wfq"
     $ ./ns3 run "diffserv-wifi-scheduler-comparison --singleAcSaturation"

* ``examples/diffserv-hybrid-wired-wireless.cc`` — wired backbone
  (1 Gb/s + 100 Mb/s) → 802.11a AP-as-DiffServ-edge → 4 STAs.  LLQ
  scheduler on the AP downlink.  ``--diffserv={true,false}`` toggles
  the qdisc for an A/B comparison; with DiffServ on, AF41 is protected
  and BE/BK p99 latency improves.

  .. sourcecode:: bash

     $ ./ns3 run "diffserv-hybrid-wired-wireless --diffserv=true"
     $ ./ns3 run "diffserv-hybrid-wired-wireless --diffserv=false"

These are demos, not Q-tier validated scenarios.  The
``LinkBandwidth`` and ``L2OverheadBytes`` attributes set on the
scheduler are representative of an airtime budget rather than a
physical line rate.

ns-2 Tcl full-scale reconstructions
###################################

Two thesis scenarios were never shipped as Tcl in the 2006 DiffServ4NS
release: §4.3 (771-node complete service model) and §4.2 (469-node AF PHB
importance-differentiation sweep).  Both were reconstructed in 2026 from
the thesis prose + tables + figures and live in the ns-2 tree under
``ns2/diffserv4ns/examples/``:

* ``example-3/`` — §4.3 Scenario 3 (771 nodes, 5-class LLQ).  Full
  quantitative match against thesis Appendix C.
* ``example-2-fullscale/`` — §4.2 Scenario 2 (469 nodes, 2-class SFQ
  with a 6-way WRED parameter sweep).  Qualitative Appendix B claims
  all reproduced; 29 / 54 Table 4.4 cells within tolerance
  (caPL ≤ 2 pp, boPL ≤ 0.5 pp, goodput ≤ 0.05).  Residual quantitative
  divergence is traceable to the DiffServ4NS-vs-PagePool/WebTraf
  incompatibility (the enlarged ``hdr_cmn`` struct shifts header offsets
  and crashes WebTraf's internal TCP agent construction), which forces
  HTTP to be approximated as bulk TCP.  See the ``README`` in that
  directory for the full divergence analysis.

Both directories contain explicit DISCLAIMER sections marking them as
2026 reconstructions rather than authentic 2001/2006 code.  They sit
alongside the authentic releases ``example-1/`` and ``example-2/`` so
that all three thesis scenarios can be exercised from the same tree.


Validation
**********

The module is validated at three levels:

Unit tests (S-tier)
===================

72 QUICK tests and 4 EXTENSIVE tests (76 total) exercise individual component
contracts.  Each test class carries a Doxygen ``@see`` line linking to the
corresponding structural spec entry under ``specs/02-structural.md``.

RFC conformance vectors
=======================

Dedicated tests verify RFC 2697 (srTCM) and RFC 2698 (trTCM) conformance by
replaying deterministic packet sequences and checking colour outcomes against
hand-computed expected results.

Q-tier scenario validation
==========================

End-to-end scenarios are compared against the original *ns-2* baselines and
thesis results:

* **Example 1** (Q-1): direct *ns-2* trace comparison.  Nine metrics are
  evaluated.  Six of nine are within tolerance; three show documented
  structural divergences attributable to *ns-3* vs. *ns-2* simulator
  differences (NetDevice queue serialisation delay and packet size accounting)
  rather than port defects.
* **Example 2** (Q-2): three-class AF/WRED.  Drop-precedence differentiation
  is qualitatively consistent with thesis Table 4.4: AF11=0% loss, AF13=31–50%
  early drops (thesis: 20–27% for staggered WRED parameter set 1).
* **Example 3** (Q-10): complete five-class service model.  Qualitatively
  consistent with thesis §4.3: LLQ gives VoIP strict priority (queue=0 pkts,
  thesis: "never overcomes 10"), WRED differentiates Silver Telnet/FTP, SFQ
  distributes excess bandwidth.  Quantitative differences are expected due to
  the scaled-down topology (13 vs. 771 nodes) and simplified traffic models.

The test suite can be run using the following commands:

.. sourcecode:: bash

   $ cd ns3/ns-3-dev
   $ ./ns3 configure --enable-examples --enable-tests
   $ ./ns3 build stratum
   $ python3 test.py -s stratum -v


References
**********

* RFC 2474 -- Definition of the Differentiated Services Field (DS Field)
  in the IPv4 and IPv6 Headers
* RFC 2475 -- An Architecture for Differentiated Services
* RFC 2597 -- Assured Forwarding PHB Group
* RFC 2598 -- An Expedited Forwarding PHB (superseded by RFC 3246)
* RFC 2697 -- A Single Rate Three Color Marker
* RFC 2698 -- A Two Rate Three Color Marker
* RFC 2859 -- A Time Sliding Window Three Colour Marker
* RFC 3246 -- An Expedited Forwarding PHB (Revised)
* RFC 7928 -- Characterization Guidelines for Active Queue Management
  (AQM) (used by the AQM characterisation harness in
  ``examples/aqm-eval-runner.cc``)
* RFC 8290 -- The FlowQueue-CoDel Packet Scheduler and Active Queue
  Management Algorithm (informs the per-tin FQ tier inside the CAKE
  composition)
* RFC 9331 -- The Explicit Congestion Notification (ECN) Protocol for
  Low Latency, Low Loss, and Scalable Throughput (L4S) (architecture
  of the ``StratumL4s`` substrate client)
* RFC 9332 -- Dual-Queue Coupled Active Queue Management (AQM) for Low
  Latency, Low Loss, and Scalable Throughput (L4S) (DualPI2 reference
  for ``l4s::QueueDisc``)
* S. Andreozzi, "Performance evaluation through simulation of IP DiffServ
  networks," M.Sc. thesis, University of Bologna, 2001.
* T. R. Henderson, M. Lacage, G. F. Riley, C. Dowell, and J. Kopena,
  "Network simulations with the ns-3 simulator," in *Proc. ACM SIGCOMM
  Demonstration*, Seattle, WA, 2008.
