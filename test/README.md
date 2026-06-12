# stratum test suite

This directory contains the Evaluation-Driven Development (EDD) test
surface for the `stratum` ns-3 module. Tests pin behaviour against
the three-tier spec suite under `../../../specs/`:

- **I-tier** (Intent — `specs/01-intent.md`): what the module shall
  do. Capability assertions, RFC traceability, design intent.
- **S-tier** (Structural — `specs/02-structural.md`): testable
  per-component assertions linked to one or more I-specs. **One
  S-assertion → one test class** is the convention.
- **Q-tier** (Quality — `specs/03-quality.md`): end-to-end scenario
  tolerances against the original ns-2 outputs and against RFC
  conformance vectors.

## Files

| File | Suite | Scope |
|------|-------|-------|
| `diffserv-test-suite.cc` | `stratum` | The release suite. Meters (`TokenBucket`, `srTCM`, `trTCM`, `TSW`), schedulers (`PQ`, `RR`, `WRR`, `WIRR`, `SCFQ`, `SFQ`, `WFQ`, `WF2Q+`, `LLQ`), queue discs (`RIO_C`, `RIO_D`, `WRED`), edge/core composition, and Q-tier scenario replications. ~88 test classes. |
| `diffserv-cake-q15-test-suite.cc` | `stratum-cake-q15` | CAKE Q-tier replication: tin shaping, host isolation, hybrid LLQ + DRR, the integrated shaped composition, helper DSCP-map parity with `tc-cake(8)`. |
| `diffserv-cake-host-fairness-smoke-test.cc` | `stratum-cake-host-fairness-smoke` | EXTENSIVE smoke for the CAKE host-fairness sweep probe: reproduces the (4, 1) CUBIC baseline host share within a single-replica band. |
| `diffserv-cake-host-fairness-udp-smoke-test.cc` | `stratum-cake-host-fairness-udp-smoke` | QUICK smoke for the host-fairness probe under UDP CBR load: per-host share near the host-fair midpoint with the bottleneck approximately saturated. |
| `diffserv-cake-host-iso-phase-1-test-suite.cc` | `stratum-cake-host-iso-phase-1` | Host-isolation characterisation grid in pure ns-3 (mainline `FqCobaltQueueDisc` path); each case emits a machine-readable `PHASE1-CELL` stdout line for offline pivoting. |
| `l4s-routing-test.cc` | `stratum-l4s` | L4S structural checks (ECN-codepoint dispatch, slot-byte-identity, multi-slot DSCP routing, DualPI2 controller behaviour) plus the RFC 9331/9332 conformance vectors: golden PI-controller trajectory (S-L4S.13), ECN codepoint transitions (S-L4S.14), DSCP preservation (S-L4S.15). |
| `l4s-scenario-validation-test.cc` | (cases in `stratum-l4s`) | EXTENSIVE L4S scenario validation: coupling-cascade engagement, latency differentiation, DCTCP+Cubic coexistence, multi-seed JFI parity against the GPRT `DualPi2QueueDisc`, CAKE-composition fairness. |
| `per-flow-classifier-test.cc` | `stratum-per-flow-classifier` | `PerFlowPolicyClassifier`: bucket isolation, refill, passthrough, edge dispatch, wildcard rules. |
| `empirical-cdf-loader-test.cc` | `stratum-empirical-cdf-loader` | `EmpiricalCdfLoader`: file parsing, bounds, sampling determinism. |
| `diffserv-example-1-instrumentation-test-suite.cc` | `stratum-example-1-instrumentation` | Smoke-runs `diffserv-example-1` and asserts the per-class trace files are emitted with well-formed, non-empty content. |
| `diffserv-flent-sink-test-suite.cc` | `stratum-flent-sink` | `FlentCsvSink` host attribution: hostId column emission, attribution correctness, backwards compatibility without hostId. |
| `diffserv-meter-trace-test-suite.cc` | `stratum-meter-trace` | `Meter::MeterColour` trace source (NVI pattern). |
| `diffserv-q16-chang-convergence-test.cc` | `stratum-q16-chang-convergence` | Q-16 replication of Chang et al. (SIMUL 2015): fair-queueing GPS-convergence envelope across schedulers at the 10 Mbps / 10:1-weight stress point. |
| `diffserv-q17-parekh-theorem1-test.cc` | `stratum-q17-parekh-theorem1` | Q-17.1 Parekh–Gallager (1993) Theorem 1 conformance: PGPS-vs-GPS per-packet finish-time gap on the WFQ scheduler. |
| `diffserv-wf2qp-regression-test.cc` | `stratum-wf2qp-regression` | WF2Q+ no-stall regression: the virtual-time floor must be applied at every busy-set transition. |
| `ds-trace-replay-application-test.cc` | `stratum-trace-replay-application` | `TraceReplayApplication`: pcap parsing, time-aligned scheduling, multi-pcap merge, `Ipv4QueueDiscItem` 5-tuple synthesis. |
| `tcp-count-ack-jitter-test-suite.cc` | `stratum-count-ack-jitter` | Count-threshold immediate-ACK jitter (`EnableCountAckJitter` / `CountAckJitterMaxUs` attributes on `TcpSocketBase`). |
| `tcp-gso-egress-test.cc` | `tcp-gso-egress` | TCP GSO egress observability: baseline (every IP-layer emission at or below MSS size) versus GSO-enabled super-segments visible at the IP layer. |
| `rfc-test-vectors.h` + `rfc-test-vectors-runner.cc` | (data + helper) | RFC 2697/2698/2859 meter conformance vectors consumed by the meter tests in `diffserv-test-suite.cc`; the runner is an intentionally standalone stub, not wired into the build. RFC 9331/9332 conformance lives in the `stratum-l4s` suite (S-L4S.13–.15 above). |
| `cake-reference-data/` | (data) | Reference traces and expected outputs for the CAKE Q-tier replication. |
| `test-data-paths.h` | (helper) | Shared cwd-independent path resolution for test data files, so the test runner works from any invocation directory. |
| `test-manifest.txt` | (snapshot) | Source-extracted list of `AddTestCase()` invocations across all suites. Diffing this file at release-tag time flags suite-rename, test-class rename, and test add/remove. Regenerate via `scripts/regen-test-manifest.sh`. |

## Running tests

The build path is `ns3/ns-3-dev/`. From that directory:

```bash
# Configure once (enables the test runner and examples).
./ns3 configure --enable-tests --enable-examples

# Build the module.
./ns3 build stratum

# Run a suite at the default fullness (QUICK). One suite name per -s
# flag; the Files table above is the full suite registry (all suite
# names start with `stratum` except `tcp-gso-egress`).
python3 test.py -s stratum
python3 test.py -s stratum-cake-q15
python3 test.py -s stratum-l4s

# Run a single test class by name.
python3 test.py -s stratum -v -r SrTcmIdleAccumulationTest

# Run including the EXTENSIVE Q-tier scenarios (slower, scenario
# replications + statistical convergence checks).
"$(ls -t ./build/utils/ns3*-test-runner-default | head -1)" --suite=stratum \
    --verbose --fullness=EXTENSIVE
```

The `python3 test.py` entry point is convenient for routine work;
the test-runner binary (`build/utils/ns3*-test-runner-default` — the name carries the release version) exposes more granular flags
(suite filter, fullness, verbose output, individual test selection).

## Test fullness tiers

ns-3 classifies test cases by a `Duration` enum:

- **QUICK** — fast unit tests; arithmetic checks, single-packet
  scenarios, structural assertions. The default fullness; runs in
  under 10 seconds across the whole `stratum` suite. CI runs this
  tier on every change.
- **EXTENSIVE** — Q-tier scenario replications (`Example2ThreeClassTest`,
  `S3PerClassRatePreservationTest`, `AfDropPrecedenceQualityTest`,
  `ThreeClassCoexistenceTest`, `PerfRegressionTest`) and statistical
  assertions that need a full simulation run. Several minutes per
  case; runs at release-tag time and on architectural changes.
- **TAKES_FOREVER** — not currently used.

## Naming convention

Tests are named `BriefDescriptionTest` (camel-case, terminating
`Test`). Each test class carries a Doxygen block immediately above
its declaration with two lines only:

```cpp
/// @brief One-sentence statement of the property under test.
/// @see specs/02-structural.md S-X.Y
class SrTcmIdleAccumulationTest : public TestCase
{
  ...
};
```

The `@see` line is the binding from the test class to the spec it
pins. Q-tier tests reference `specs/03-quality.md Q-X.Y`; I-tier
references appear only on tests that span multiple S-assertions.

A handful of pre-existing classes carry the legacy `XxxTestCase`
suffix or the `TestXxx` prefix — these reflect older convention and
are gradually being renamed to the `BriefDescriptionTest` form;
new tests should always use the canonical form.

## Spec ID cross-references

Spec identifiers appear throughout the test source and the public
documentation:

| Token | Source file | Meaning |
|-------|-------------|---------|
| `I-N` | `specs/01-intent.md` | Intent assertion N (capability the module shall provide). |
| `S-X.Y` | `specs/02-structural.md` | Structural assertion Y under topic group X (one observable property of one component). |
| `Q-X.Y` | `specs/03-quality.md` | Quality assertion Y under scenario group X (end-to-end tolerance). |
| `F-A` … `F-D` | catalogue in repo root | Empirical-finding identifier surfaced during validation. |
| `N2-N`, `D2-N`, `N3-N` | `docs/HISTORICAL_BUGS.md` | Bug catalogue: ns-2 core defects, DiffServ4NS-for-ns-2 defects, ns-3 core defects respectively. |

These identifiers are stable contract tokens — they are referenced
from the paper, the handbook, and the public README. Renaming any
identifier requires updating every citing source plus this directory.

## Adding a new test

The EDD workflow is documented in the release-root
`CONTRIBUTING.md` under *Add a meter, scheduler, or queue-disc to
the substrate*; the short version is:

1. Find or write the relevant `S-` or `Q-` spec (the I-tier is
   generally fixed by the 2001 thesis or an RFC).
2. Write the test first, named for the behaviour it pins, with the
   `@brief` + `@see` Doxygen pair.
3. Implement the minimum code to make the test pass.
4. Run the full stratum test suite to check for regressions.
5. Regenerate `test-manifest.txt` (`bash scripts/regen-test-manifest.sh`).
6. Open the PR.

If the test you would like to add does not have a backing spec, open
an issue first — the spec is the contract, code without a spec
assertion is unverifiable in this project.

## Authority

Behaviour disagreements between sources are resolved in this order
(per the release-root `CLAUDE.md`):

1. The three-tier EDD spec suite (`specs/`).
2. The RFCs (2474, 2475, 2597, 2598, 2697, 2698, 2859, 3246, 9331,
   9332).
3. The 2001 thesis at `provenance/Andreozzi-2001-thesis.pdf`,
   Chapter 3.3.3.
4. The original ns-2 reference at `src/ns-2.29/diffserv/`.
5. ns-3 idioms.

If the implementation disagrees with a spec, the spec is wrong (or
needs tightening with a divergence note) — but never silently widen
a test tolerance to make a failing test pass.
