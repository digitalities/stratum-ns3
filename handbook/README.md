# Stratum Handbook

Stratum is a QoS substrate for ns-3 mainline that composes Differentiated
Services, L4S, and CAKE as three first-class clients of one module. The
substrate provides a four-slot pipeline — classify-and-meter, mark-and-route,
per-class slot array, and across-slot service policy — that each client
populates independently, yielding a single ns-3 contrib module with a
registry of IETF and Linux AQM cells. This handbook is the long-form
reference for the substrate: how to use it, how it is designed, and how its
behaviour has been validated against RFC test vectors, cross-simulator
comparisons, and real-network heritage measurements.

## How to read this book

Three entry points:

- **Users and practitioners** — start with Part I (task-oriented recipes).
  Each chapter is a short, self-contained guide to one client or capability.
  The quickstart (I-02) gets you to a running scenario in 15 minutes.

- **Designers and contributors** — read Part II (architecture and design).
  Start with II-01 (the part intro and reading path) then II-02 (the
  four-slot architecture and the three clients as peers). Background chapters
  II-03 and II-04 supply the traffic-management and DiffServ context the
  architecture chapter assumes; the client chapters (II-05, II-06, II-07)
  and the ns-3 port (II-08) follow.

- **Reviewers and researchers** — read Part III (evidence and validation).
  These chapters document the three-way cross-simulator comparison, L4S and
  CAKE fidelity results, the wireless demo, and the AQM characterisation
  suite. The appendices carry RFC conformance vectors and the long-form
  validation record.

The front matter (`00-preface.md`) describes provenance conventions and
citation guidance that apply across all parts.

For the ns-3-native Sphinx model documentation, see `doc/stratum.rst`
in the repository root: that file is the ns-3-side reference; this handbook
is the long-form book.

---

## Front matter

| File | Description |
|---|---|
| [00-preface.md](00-preface.md) | Orientation, scope, provenance conventions, and the 2001–2026 lineage summary. |

---

## Part I — Using Stratum

Task-oriented recipes for building, configuring, and running the three clients.

| Chapter | File | Description |
|---|---|---|
| I-01 | [I-01-introduction.md](I-01-introduction.md) | How Part I is organised, what the recipes assume, and where to start. |
| I-02 | [I-02-quickstart.md](I-02-quickstart.md) | Build Stratum, run a DiffServ scenario, preview CAKE and L4S, and modify an example: from clone to working simulation in 15 minutes. |
| I-03 | [I-03-diffserv.md](I-03-diffserv.md) | DiffServ recipes: single-edge foundation, meter configuration, DSCP-based PHB tables, AQM choice, and the full multi-tier pipeline. |
| I-04 | [I-04-l4s.md](I-04-l4s.md) | L4S recipes: ECT(1) classification, DualPI2 coupled scheduler, coupling-formula validation, and head-to-head comparison with FqCoDel. |
| I-05 | [I-05-cake.md](I-05-cake.md) | CAKE recipes: substrate demo, RRUL benchmark, host-pair isolation, and TCP fairness under load using the FqCobalt-based tin model. |
| I-06 | [I-06-aqm-eval.md](I-06-aqm-eval.md) | AQM evaluation recipes: invoke the `aqm-eval-runner` for a single AQM cell, enumerate the full catalogue from the CLI, interpret per-flow goodput/delay CSV output and the throughput-vs-latency ellipse plot, sweep the complete matrix, and reproduce an external scheduler benchmark. |
| I-07 | [I-07-wireless.md](I-07-wireless.md) | Wireless recipes: attach the substrate's edge queue disc to an ns-3 802.11ax access point (including Wi-Fi-specific bandwidth and framing parameters), then run all eight schedulers (PQ, RR, WRR, WIRR, SCFQ, WFQ, WF2Q+, LLQ) and compare per-class throughput and latency. |
| I-08 | [I-08-examples-catalog.md](I-08-examples-catalog.md) | Annotated catalogue of the runnable examples shipped under `examples/`, grouped by client. |
| I-09 | [I-09-extending.md](I-09-extending.md) | How to add an external AQM or scheduler via the `Registry<EntryT>` template so that it propagates to the CLI catalogue, plot palettes, and smoke tests automatically. |
| I-10 | [I-10-troubleshooting.md](I-10-troubleshooting.md) | Known gotchas, build-environment issues, and diagnostic guidance. |
| I-11 | [I-11-feedback.md](I-11-feedback.md) | How to report a bug, propose a feature, or reach the maintainer. |

---

## Part II — Architecture and design

The substrate top-down: the four-slot architecture, the three clients as peers, and the ns-3 port.

| Chapter | File | Description |
|---|---|---|
| II-01 | [II-01-introduction.md](II-01-introduction.md) | How Part II is organised and the top-down reading path through it. |
| II-02 | [II-02-stratum-architecture.md](II-02-stratum-architecture.md) | The Stratum substrate top-down: the four pluggable strategy slots, the three clients as slot choices, registry-based extensibility, and scope boundaries. |
| II-03 | [II-03-traffic-management.md](II-03-traffic-management.md) | Traffic management background: best-effort vs class differentiation, IntServ vs DiffServ, AQM (RED/WRED/CoDel/PIE/FQ-CoDel), and where the Stratum substrate fits. |
| II-04 | [II-04-diffserv-model.md](II-04-diffserv-model.md) | The DiffServ model: RFC 2474/2475 architecture, DSCP field, EF/AF/CS/BE PHBs, traffic-conditioning components, and the post-2001 RFC chronology. |
| II-05 | [II-05-diffserv-client.md](II-05-diffserv-client.md) | The DiffServ client: how the 2001 three-axis design maps onto the substrate's four strategy slots, meter and scheduler families, AF drop-precedence, LLQ composition, and the monitoring surface. |
| II-06 | [II-06-l4s-client.md](II-06-l4s-client.md) | The L4S client: what L4S is, DualPI2 coupling formulas, the RFC 9332 App. A.1 controller, the coupled scheduler, and composition with the substrate. |
| II-07 | [II-07-cake-client.md](II-07-cake-client.md) | The CAKE client: tin profiles, shaping modes, the ACK filter, feature scope, and composition with the substrate. |
| II-08 | [II-08-ns3-module.md](II-08-ns3-module.md) | The ns-3 module: spec-driven implementation, RFC conformance vectors, the patch workflow for ns-3 mainline changes, and validation summary. |

---

## Part III — Evidence and validation

Three-way cross-simulator results, L4S and CAKE fidelity measurements, wireless coverage, and the AQM characterisation suite.

| Chapter | File | Description |
|---|---|---|
| III-01 | [III-01-introduction.md](III-01-introduction.md) | How Part III is organised: the validation philosophy and the bounded reference sources. |
| III-02 | [III-02-three-way-validation.md](III-02-three-way-validation.md) | Three-way comparative results across ns-2.29, ns-2.35, and ns-3, with numeric comparison tables and four figures (OWD, IPDV, goodput, per-class service rates) for the three reference scenarios. |
| III-03 | [III-03-l4s.md](III-03-l4s.md) | L4S validation: ECN classification parity, DualPI2 coupling-formula verification, responsive-flow coexistence, AQM-vs-no-AQM latency advantage, and higher-load characterisation. |
| III-04 | [III-04-cake.md](III-04-cake.md) | CAKE validation: spec-tier coverage, dispatcher instrumentation, host-fairness empirical anchor, and the Linux-netns cross-validation backend. |
| III-04A | [III-04A-cake-flent-figure-pack.md](III-04A-cake-flent-figure-pack.md) | CAKE Flent figure pack: ns-3 RRUL benchmark output figures and per-flow throughput traces generated by the Stratum CAKE client. |
| III-05 | [III-05-wireless.md](III-05-wireless.md) | Wireless extension: the substrate attached to an ns-3 802.11ax device with no module changes; demo-grade coverage with a future-work sketch. |
| III-06 | [III-06-aqm-eval.md](III-06-aqm-eval.md) | AQM-eval characterisation suite: the `aqm-eval-runner` sweeps in-tree AQMs under RFC 7928 scenarios and surfaces per-AQM throughput/latency characterisation findings. |
| III-07 | [III-07-conclusions.md](III-07-conclusions.md) | Conclusions: what was built, what the evidence established (reconstruction as verification, L4S conformance, CAKE Linux-faithfulness, composition), fidelity boundaries, and future directions. |

---

## Appendices

| File | Description |
|---|---|
| [appendix-A-rfc-conformance.md](appendix-A-rfc-conformance.md) | RFC 2697/2698/2859 conformance vectors: ground-truth test-vector tables for srTCM, trTCM, and TSW, with reuse guidance. |
| [appendix-B-validation-longform.md](appendix-B-validation-longform.md) | Validation — long-form record: the full seven-subsection write-up covering RFC conformance, cross-simulator equivalence, independent reproduction, and real-network inheritance in more depth than the paper allows. |
| [appendix-C-aqm-eval-interop.md](appendix-C-aqm-eval-interop.md) | AQM-eval interop: how to export an ns-3 simulation run as a Flent-compatible JSON archive for replay with standard bufferbloat post-processing tools. |
