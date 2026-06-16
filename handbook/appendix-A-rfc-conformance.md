---
title: 'Appendix A: RFC 2697/2698/2859 conformance vectors'
origin: 2026-written
status: filled
last-updated: 2026-06-06
---

# Appendix A — RFC 2697/2698/2859 conformance vectors

## A.1 Why test vectors

RFC 2697 (srTCM), RFC 2698 (trTCM) and RFC 2859 (TSW) describe their
meters in pseudocode-level detail (bucket update rules, comparison
orderings, boundary conditions) but none of them ships an executable
conformance suite. An implementer has the algorithm, not a reference
it can be diffed against. Historically that gap has been filled by
ad-hoc tests per implementation, and two implementations that both
claim to pass their own tests can still disagree on an edge case that
neither tested.

The 2026 ns-3 port of DiffServ4NS closes that gap for the srTCM and
trTCM families with a framework-independent ground-truth table at
[`test/rfc-test-vectors.h`](../test/rfc-test-vectors.h).
Twenty-five vectors (5 token-bucket + 10 srTCM + 10 trTCM, 52 packet
events total) are expressed as plain C++ struct literals — no ns-3
types, no simulator state, no Tcl. Each vector fixes the meter
parameters, the initial bucket state, and for every packet in the
sequence the expected colour and the expected bucket state *after*
the meter and policer have run. Values are exact: the table was
derived by tracing the 2001 reference algorithm in the original
`dsPolicy.cc` in the DiffServ4NS heritage repository
by hand, so the comparison uses a `1e-6` tolerance only for
floating-point safety.

The TSW family (RFC 2859) is verified differently. TSW is an
EWMA-over-time mechanism, not a bucket mechanism, and its conformance
target is statistical: the colour ratios converge to closed-form
expressions of `CIR`, `PIR` and the measured arrival rate. We
therefore carry four deterministic structural test cases (§A.4)
rather than a vector table.

The vector table is reusable. A third-party ns-2, OMNeT++, or
click-router implementation can `#include "rfc-test-vectors.h"`,
feed each event to its own meter, and compare results with no
dependency on our port. §A.6 shows the struct layout.

**L4S conformance (RFC 9331 / 9332).** This appendix covers the
DiffServ meter family (RFC 2697/2698/2859). RFC 9331
(L4S ECN semantics) and RFC 9332 (DualPI2 AQM) conformance
evidence for the L4S client lives in [L4S validation](III-03-l4s.md),
which documents ECT(1) classification, CE-mark idempotence, and the
coupled-marking controller step-response.

## A.2 RFC 2697 — Single Rate Three Color Marker (srTCM)

Ten vectors exercise every code path in the RFC 2697 algorithm:
token refill, overflow from `cBucket` to `eBucket`, the colour
decision in colour-blind mode (RFC 2697 §4), and each policer
boundary.

| ID | Name | Description | Backs |
|----|------|-------------|-------|
| SR-1 | `SrTcm_AllGreenUnderCir` | CBR at CIR with full buckets; every packet GREEN from cBucket; eBucket untouched. | S-2.3 (GREEN ratio), S-2.5 |
| SR-2 | `SrTcm_BurstFitsCBucket` | 3-packet burst absorbed entirely by cBucket; eBucket never touched. | S-2.5 |
| SR-3 | `SrTcm_BurstGreenYellowRed` | Canonical GREEN→YELLOW→RED transition over 7 packets: cBucket drains, then eBucket drains, then both empty. | S-2.3, S-2.4, S-2.5 |
| SR-4 | `SrTcm_IdleOverflowToEBucket` | Idle period generates `CIR·dt` tokens; cBucket caps at CBS, overflow spills into eBucket. Core RFC 2697 §3 refill rule. | S-2.1 |
| SR-5 | `SrTcm_ExactCBucketGreen` | Packet size exactly equals cBucket → GREEN, cBucket=0. `>=0` boundary in the policer. | S-2.5 |
| SR-6 | `SrTcm_ExceedCBucketByOneYellow` | One byte more than cBucket → YELLOW. Complementary edge of SR-5. | S-2.5 |
| SR-7 | `SrTcm_BothBucketsEmptyRed` | Both buckets nearly empty; packet far exceeds both → RED; neither bucket decremented. | S-2.5 |
| SR-8 | `SrTcm_RefillTimingTwoPackets` | Two packets with an inter-arrival gap: the second packet's state must use `CIR·(t2−t1)` accumulation from the *first* packet's arrival, catching the classic "arrivalTime not updated" bug. | S-2.5 |
| SR-9 | `SrTcm_LongIdleBothCapped` | 10 s idle would generate 1.25 MB of tokens; cBucket caps at CBS, eBucket caps at EBS. | S-2.1 |
| SR-10 | `SrTcm_SpecS21Validation` | Directly instantiates S-2.1 and S-2.2 from the spec: CIR=1 Mbps, CBS=10 000, EBS=20 000, 100 ms idle, 5 000-byte packet. | S-2.1, S-2.2 |

All vectors run in colour-blind mode (I-2.7). The key invariant they
collectively enforce is the RFC 2697 §3 overflow rule: tokens refill
`cBucket` first, and only when `cBucket` is at `CBS` does the excess
spill into `eBucket`. SR-4 is the minimal positive check; SR-9 is the
long-idle stress; SR-3 is the integrated end-to-end trace.

Per-vector input/output detail is inline in the struct literals in
`rfc-test-vectors.h` — each event is annotated with the expected
arithmetic (`c=3000+125>CBS→3000`, `3000-1000=2000 → GREEN`). An
implementation fix can thus be validated against a specific packet,
not against an aggregate PASS/FAIL.

Each vector is exercised inside ns-3 by a single test case
`S-2 srTCM: <Name>` (see `SrTcmVectorTestCase` in
`diffserv-test-suite.cc`).

## A.3 RFC 2698 — Two Rate Three Color Marker (trTCM)

Ten vectors cover the trTCM algorithm. The critical structural
difference from srTCM: trTCM has **two independent rates**
(`CIR`/`CBS` and `PIR`/`PBS`). The buckets refill independently:
there is no overflow channel between them. The policer ordering is
also inverted relative to srTCM: pBucket is checked *first*, so
`pBucket < size` forces RED even if cBucket has tokens.

| ID | Name | Description | Backs |
|----|------|-------------|-------|
| TR-1 | `TrTcm_AllGreenUnderCir` | Stream at CIR with full buckets; both refill faster than they drain; every packet GREEN. | S-3.1 |
| TR-2 | `TrTcm_GreenThenYellow` | Burst between CIR and PIR; small-CBS cBucket exhausts while large-PBS pBucket still has tokens → clean GREEN→YELLOW transition. | S-3.1, S-3.2, S-3.5 |
| TR-3 | `TrTcm_GreenYellowRed` | Complete GREEN→YELLOW→RED walk in 3 packets with small buckets. | S-3.1, S-3.2, S-3.3, S-3.4, S-3.5, S-3.6 |
| TR-4 | `TrTcm_YellowCBucketUnchanged` | **CRITICAL**: on YELLOW, cBucket must *not* be decremented, only pBucket. Catches the naive "decrement both unconditionally" bug. | S-3.5 |
| TR-5 | `TrTcm_GreenBothDecrement` | **CRITICAL**: on GREEN, *both* buckets must be decremented by packet size. Catches the "decrement only cBucket" bug. | S-3.6 |
| TR-6 | `TrTcm_RedNeitherDecrement` | **CRITICAL**: on RED, *neither* bucket is decremented. Catches the "decrement pBucket on RED" bug. | S-3.4 |
| TR-7 | `TrTcm_IndependentRefill` | Post-idle, pBucket reaches PBS at the PIR rate while cBucket only reaches its own cap at CIR — proves the two buckets are not coupled. | S-3.1 |
| TR-8 | `TrTcm_PBucketExactlyFitsYellow` | pBucket = packet size → YELLOW (not RED). Tests `(pBucket − size) < 0` boundary. | S-3.4 |
| TR-9 | `TrTcm_CBucketExactlyFitsGreen` | cBucket = packet size → GREEN; both buckets decremented; cBucket goes to 0. Complements TR-8. | S-3.6 |
| TR-10 | `TrTcm_LongIdlePBSCapped` | 10 s idle caps pBucket at PBS and cBucket at CBS independently. | S-3.1 |

TR-4, TR-5, TR-6 are the structural triad of trTCM. A policer that
gets any one of those three wrong will still pass a statistical
colour-ratio test (the aggregate shares can be right by accident) but
will fail these vectors on the exact post-state, exposing the bug on
a single packet.

Each vector is exercised inside ns-3 by a single test case
`S-3 trTCM: <Name>` (see `TrTcmVectorTestCase`).

## A.4 RFC 2859 — Time Sliding Window (TSW)

TSW is not a bucket scheme: `tC(t) = avg rate in a sliding window of
length winLen`, and colouring is probabilistic based on how far the
EWMA-estimated rate exceeds `CIR` (TSW2CM) or `PIR` (TSW3CM). Exact
per-packet ground-truth is therefore the wrong tool — a vector table
would encode the RNG seed, not the algorithm. We instead assert the
two invariants RFC 2859 actually guarantees: **EWMA convergence** and
**steady-state colour ratios**.

Four deterministic structural test cases cover TSW:

| Test case | Setup | Assertion | Backs |
|-----------|-------|-----------|-------|
| `S-4.1 TSW EWMA converges to actual rate within 5%` | CBR at 2 Mbps for 10 s, `winLen=1 s`, starting `avgRate=0`. | After 10 × winLen, `avgRate` is within 5 % of the actual 2 Mbps. | S-4.1 |
| `S-4.2 TSW2CM under CIR: all GREEN` | CIR=1 Mbps, feed 500 kbps, 1 000 packets. | Zero packets marked RED. Under-CIR must be deterministically GREEN. | S-4.2 |
| `S-4.3 TSW2CM over CIR: GREEN ratio approx 0.5` | CIR=1 Mbps, feed 2 Mbps, 10 000 packets, `avgRate` pre-seeded to feedRate so the ratio is stable from start. | GREEN ratio = CIR/avgRate = 0.5 ± 0.05. | S-4.3 |
| `S-4 TSW3CM colour ratios above PIR` | CIR=100 000 B/s, PIR=200 000 B/s, feed 400 000 B/s, 20 000 packets. | GREEN ratio 0.25 ± 0.05, YELLOW 0.25 ± 0.05, RED 0.50 ± 0.05, i.e. CIR/rate, (PIR−CIR)/rate, (rate−PIR)/rate. | S-4 (TSW3CM) |

The distinction between TSW2CM (green/red — RFC 2859 §4) and TSW3CM
(green/yellow/red — RFC 2859 §5) shows up as the shape of the
assertion: TSW2CM has two expected ratios (`greenCount == total` in
S-4.2, `greenCount/total ≈ 0.5` in S-4.3); TSW3CM has three.

TSW2CM and TSW3CM share `ApplyMeter` (the EWMA update), so S-4.1 is
run through TSW2CM but verifies the update function both families
rely on.

## A.5 Running the conformance tests

From your ns-3 tree:

```bash
cd ns-3  # or your ns-3 tree (standalone flow)
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
python3 test.py -s stratum -v
```

To filter to just the RFC vectors and TSW cases (skipping schedulers,
edge/core, and end-to-end tests):

```bash
# srTCM vectors only
python3 test.py -s stratum -v -r "S-2 srTCM"

# trTCM vectors only
python3 test.py -s stratum -v -r "S-3 trTCM"

# Token-bucket vectors
python3 test.py -s stratum -v -r "S-1 TokenBucket"

# TSW cases
python3 test.py -s stratum -v -r "S-4"
```

The convenience slash command `/rfc-conformance` runs all four
above in one go against the compiled module.

A single vector can be isolated via its full name:

```bash
python3 test.py -s stratum -v -r "S-3 trTCM: TrTcm_YellowCBucketUnchanged"
```

Each test case reports both the colour mismatch and the post-state
mismatch in its failure message (e.g. `got YELLOW c=600 p=4200, want
YELLOW c=500 p=4200`), so a failing vector points directly at the
offending bucket.

## A.6 Reusing the vector table outside ns-3

`rfc-test-vectors.h` is deliberately framework-independent: no ns-3
headers, no external dependencies, only `<cstdint>`. A third-party
re-implementation can include it and walk the table against its own
meter.

The struct layout for srTCM (trTCM is analogous with an additional
`pir_bytes_per_sec` and `pbs_bytes` and a per-event
`expected_p_bucket` field):

```cpp
enum class Colour : uint8_t { GREEN = 0, YELLOW = 1, RED = 2 };
static constexpr double NA = -1.0;   // sentinel for inapplicable fields

struct PacketEvent {
    double   arrival_time_s;     // absolute arrival time (s)
    uint32_t size_bytes;         // packet size
    Colour   expected_colour;    // expected colour decision
    double   expected_c_bucket;  // cBucket after meter+policer
    double   expected_e_bucket;  // eBucket after (srTCM); NA otherwise
    double   expected_p_bucket;  // pBucket after (trTCM); NA otherwise
};

struct SrTcmTestVector {
    const char* name;
    const char* rfc_citation;
    const char* description;
    double      cir_bytes_per_sec;
    uint32_t    cbs_bytes;
    uint32_t    ebs_bytes;
    double      initial_c_bucket;
    double      initial_e_bucket;
    double      initial_arrival_time;
    int         num_events;
    PacketEvent events[/* kMaxEvents = 16 */];
};
```

A minimal standalone runner driver is provided at
[`test/rfc-test-vectors-runner.cc`](../test/rfc-test-vectors-runner.cc).
It compiles with plain g++ and ships stub meters; swap the stubs for
the implementation under test:

```bash
g++ -std=c++17 -I test \
    test/rfc-test-vectors-runner.cc \
    -o run-vectors
./run-vectors
```

Two semantics are load-bearing when porting:

1. **Bucket values represent the state AFTER both `applyMeter` and
   `applyPolicer` have run** for the given packet. A meter that
   exposes only a mid-cycle state will mismatch every non-GREEN
   vector.
2. **A RED decision must not decrement any bucket.** The reference
   (`dsPolicy.cc:765-766` for trTCM, `dsPolicy.cc:639` for token
   bucket) returns immediately on RED with no state mutation. The
   vectors TR-6 and TB-4 fail loudly for implementations that
   decrement on RED.

If a port disagrees with any value in the table, the header's lead
comment is explicit: *"the port is wrong — fix the port, not the
vector."* The vectors were derived by manual trace against the 2001
reference; they are the contract.

## See also

- [`test/rfc-test-vectors.h`](../test/rfc-test-vectors.h) — the 25-vector ground-truth table.
- [`test/rfc-test-vectors-runner.cc`](../test/rfc-test-vectors-runner.cc) — framework-independent standalone runner.
- [`test/diffserv-test-suite.cc`](../test/diffserv-test-suite.cc) — ns-3 `TestSuite` that wraps each vector and the four TSW cases.
- [`specs/02-structural.md`](../specs/02-structural.md) — S-1, S-2, S-3, S-4 spec assertions that the vectors and TSW cases back.
- [III-03-l4s.md](III-03-l4s.md) — RFC 9331 / 9332 (L4S) conformance evidence.
- RFC 2697 — A Single Rate Three Color Marker (Heinanen & Guérin, 1999).
- RFC 2698 — A Two Rate Three Color Marker (Heinanen & Guérin, 1999).
- RFC 2859 — A Time Sliding Window Three Colour Marker (Fang, Seddigh & Nandy, 2000).
