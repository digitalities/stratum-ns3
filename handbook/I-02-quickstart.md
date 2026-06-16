# Quickstart — 15 minutes from clone to all three clients

By the end you will have built Stratum, run a DiffServ scenario, previewed CAKE and L4S, and modified an example to observe how scheduling choice affects per-class latency.

**Time**: 15 minutes (assuming a working C++ build environment).

## What you need

- macOS or Linux
- A working C++ toolchain (clang or gcc; ns-3 supports both)
- Python 3.9+ (for the ns-3 build driver and test runner)
- ~3 GB of free disk for the ns-3 source + build artefacts

> [!NOTE]
> This module builds against one pinned ns-3 release. The pin lives in exactly one place: the `NS3_PIN` constant in `scripts/fetch-ns3.sh` (query it with `scripts/fetch-ns3.sh --print-pin`). Either install path below lands at the pin automatically; if a fresh clone produces build failures, reset your ns-3 tree to it: `git checkout "$(<path-to>/scripts/fetch-ns3.sh --print-pin)"`.

## 1. Clone and bootstrap (2 minutes)

Two install paths lead to the same result; pick the one that suits your workflow.

### Option A — Into an existing ns-3 tree (recommended)

Clone ns-3 at the pinned release, then add Stratum as a contrib module and apply its bundled patches:

```bash
git clone https://gitlab.com/nsnam/ns-3-dev.git ns-3
cd ns-3
git clone https://github.com/digitalities/stratum-ns3.git contrib/stratum
git checkout "$(contrib/stratum/scripts/fetch-ns3.sh --print-pin)"
# apply the bundled mainline patches (required)
for p in contrib/stratum/patches/ns3/*.patch; do git apply "$p"; done
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
```

Already have an ns-3 tree? `cd` into it instead — it must be at the pinned release tag (`--print-pin` above) for the bundled patches to apply cleanly.

Option A is now built. Skip to [step 3](#3-run-the-foundation-client-5-minutes).

### Option B — Standalone (script-managed sibling clone)

Clone Stratum anywhere; the bootstrap script handles the rest:

```bash
git clone https://github.com/digitalities/stratum-ns3.git
cd stratum-ns3
./scripts/fetch-ns3.sh        # clones the pinned ns-3 as a sibling directory, patches, symlinks
```

`fetch-ns3.sh` clones the pinned ns-3 revision, applies the patches in `patches/ns3/`, and creates the `contrib/stratum` symlink that points back to the Stratum repo root. The ns-3 tree lands as a sibling directory (`ns-3/`) next to `stratum-ns3/`.

## 2. Build — Option B only (3 minutes)

Option A users are already built — jump straight to [step 3](#3-run-the-foundation-client-5-minutes).

For Option B, move into the sibling ns-3 tree and build:

```bash
cd ../ns-3
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
```

You should see `Finished executing the following commands: ./ns3 build` after a few minutes. If the build fails, check [troubleshooting.md](I-10-troubleshooting.md).

## 3. Run the foundation client (5 minutes)

```bash
./ns3 run "diffserv-example-1"
```

This runs the project's canonical scenario — a reproduction of Sergio Andreozzi's 2001 ns-2 thesis example. The topology is: three hosts sending EF (Expedited Forwarding), AF11 (Assured Forwarding), and BE (Best Effort) traffic through a DiffServ edge router that meters with sr-TCM and schedules with strict-priority queueing (PQ).

In the output you should see:
- The simulator boot and the topology being instantiated
- Periodic snapshots of per-class throughput and one-way delay
- EF achieving low delay (single-digit ms) under PQ, with BE absorbing the queueing delay
- The DSCP markings appearing on traffic flowing through the edge

This is what DiffServ looks like, configured through the substrate's four primitives:
- **Classify-and-Meter** — sr-TCM, RFC 2697
- **Mark-and-Route** — DSCP tag + PHB table
- **Slot Array** — per-PHB inner queue (drop-tail in this example)
- **Service Policy** — PQ (strict priority)

## 4. Preview CAKE (30 seconds)

```bash
./ns3 run "diffserv-cake"
```

You should see CAKE running with the `diffserv4` tin map (Best-Effort / Voice / Video / Bulk at 100% / 25% / 50% / 6.25% of link capacity, per the Linux `tc-cake(8)` defaults — the helper call is `cake::Helper::SetAsCakeDiffserv4`). The substrate composes mainline `FqCobaltQueueDisc` under a new across-tin DRR dispatcher — same four primitives, but the Service Policy is DRR instead of PQ, and each Slot is per-tin FqCobalt instead of drop-tail.

Deep-dive: [CAKE recipes](I-05-cake.md) and the [CAKE chapter](III-04-cake.md).

## 5. Preview L4S (30 seconds)

```bash
./ns3 run "diffserv-l4s-s1-latency"
```

You should see two queues running in parallel — a classic queue with PI² AQM, and an L4S queue with scalable marking — coupled through the squared coupling `p_C = (p_L/k)²` per RFC 9332 §2.1 eq. (1). Traffic with `ECT(1)` marking lands in the L4S queue; everything else lands in the classic queue.

Deep-dive: [L4S recipes](I-04-l4s.md) and [The L4S client](II-06-l4s-client.md).

## 6. Try changing something (3 minutes)

Re-run example-1 but swap the scheduler from PQ to WFQ:

```bash
./ns3 run "diffserv-example-1 --scheduler=WFQ"
```

> [!NOTE]
> The scheduler name is case-sensitive — use exactly `WFQ`, not `wfq`. Accepted values: `PQ`, `WFQ`, `SCFQ`, `SFQ`, `WF2Qp`, `LLQ`.

Compare the per-class delay output to the PQ run. Observe:

- EF no longer dominates — it shares bandwidth fairly with AF and BE
- EF's one-way delay rises (WFQ is work-conserving, not strict-priority)
- BE's delay drops (it's no longer starved under EF load)

This is the bandwidth-vs-latency trade-off classical schedulers make. WFQ treats EF as just another weighted class; PQ treats EF as preemptive. Which is correct depends on your scenario.

For more on this trade-off: the [Schedulers section of the DiffServ client chapter](II-05-diffserv-client.md) + [DiffServ recipe 1](I-03-diffserv.md).

## What's next

You've now run all three substrate clients and modified one. Go deeper:

- **[DiffServ recipes](I-03-diffserv.md)** — sr-TCM vs tr-TCM, schedulers (PQ/WFQ/WF2Q+/LLQ), Lower-Effort PHB, AQM choice under the pipeline
- **[L4S recipes](I-04-l4s.md)** — coupling formula validation, FqCoDel head-to-head, mixed-flow scenarios
- **[CAKE recipes](I-05-cake.md)** — RRUL benchmark, host-pair isolation, square-wave fairness, tin modes
- **[Wireless recipes](I-07-wireless.md)** — Stratum attached to an 802.11ax AP
- **[AQM-eval](I-06-aqm-eval.md)** — characterise 13 AQMs in 30 seconds (ellipse diagram)
- **[Extending](I-09-extending.md)** — add your own AQM or scheduler to the registry

> [!TIP]
> Each recipe ends with a "Found a problem?" link. If a recipe doesn't run cleanly, file an issue — that's the most useful feedback you can give us.

Found a problem with this quickstart? [File a recipe issue](https://github.com/digitalities/stratum-ns3/issues/new?template=recipe-request.yml) — quickstart accuracy is the highest-priority feedback we want.
