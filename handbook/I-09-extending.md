# Extending the substrate

External AQMs and schedulers integrate into Stratum via the `Registry<EntryT>` template — a single source of truth that the CLI catalogue, plot palettes, handbook tables, and smoke-registry tests all consume. Adding one entry to the registry propagates everywhere without manual fan-out.

These two recipes walk through the registration pattern. Both stay hands-on: you'll subclass an entry type, call `Register({...})`, and verify the new cell appears in the runner's `--aqm=list` (or scheduler equivalent) output.

> See also: [`diffserv.md`](I-03-diffserv.md), [`aqm-eval.md`](I-06-aqm-eval.md).

## Recipe: Add your own AQM cell to the registry

**You'll**: Register a new AQM dispatch name with `Registry<AqmEntry>`, expose it via the CLI, and verify it appears in `aqm-eval-runner`.

**Time**: 15 min

**You'll learn**:
- The shape of an `AqmEntry` (dispatch name, file tag, display name, family, ECN flag, factory closure)
- How `Register({...})` calls compose into the registry's enumeration
- Why the registry pattern means you don't touch the runner, the helper, or the plotting scripts when adding a new AQM

**Prerequisites**: [Quickstart](I-02-quickstart.md) (built ns-3, ran example-1); [`aqm-eval.md` recipe 1](I-06-aqm-eval.md) (ran `aqm-eval-runner --aqm=list`)

> [!NOTE]
> The current registry holds **n=13 AQM cells**. Mainline-only AQMs (PfifoFast, RED, ARED, CoDel, PIE, Cobalt, FqCoDel, FqPie, FqCobalt) and Stratum-specific composites (StratumRed, StratumL4sWred, StratumL4sCoupledOnly, StratumCake) all coexist behind the same dispatch surface.

### Run it

```bash
cd ns-3  # or your ns-3 tree (standalone flow)
./ns3 run "aqm-eval-runner --aqm=list"
```

You should see the 13 in-tree cells. After registering your AQM (steps below), the same command lists 14.

### How it works

`AqmEntry` is the minimal struct each cell registers. You provide a factory closure that constructs the queue disc:

```cpp
// In a new file under model/, e.g. stratum-aqm-my-aqm.h:
struct AqmEntry {
    std::string name;        // dispatch key passed via --aqm=
    std::string fileTag;     // sanitised name for output filenames
    std::string displayName; // pretty name for --aqm=list
    Family family;           // Single / Fq / Ds4
    bool supportsEcn;
    std::function<Ptr<QueueDisc>(DataRate)> factory;
};
```

Register the cell in `stratum-aqm-registry.cc::AqmRegistry::AqmRegistry()`:

```cpp
Register({"MyAqm",                  // --aqm=MyAqm
          "MyAqm",                  // output file tag
          "My Custom AQM",          // display name
          F::Single,                // family
          true,                     // supportsEcn
          [](DataRate r) {
              return CreateObject<MyAqmQueueDisc>();
          }});
```

That's it. The runner picks the new cell up; `--aqm=list` shows it; output CSVs filename-tag with `MyAqm`; the plotting scripts see it next to the others.

### Try changing

1. Add a runtime knob: extend your factory to read an attribute (e.g. `MyAqmQueueDisc::Mode`) and register two cells — `MyAqm-Mild` and `MyAqm-Aggressive` — that share the same C++ class but differ in attribute value. Confirm both appear in `--aqm=list`.
2. Add a configuration sanity check: in your factory closure, call `NS_ABORT_MSG_IF(...)` if the queue size is below a minimum. Re-run and confirm the abort fires when you misconfigure.
3. Write a smoke test: drop a `TestCase` into `test/` that instantiates the registry, calls `Make("MyAqm", DataRate("10Mbps"))`, and verifies the returned `Ptr<QueueDisc>` is non-null. Run with `python3 test.py -s stratum`.

> [!TIP]
> The `family` field controls plot grouping in the AQM-eval plotting scripts. Use `Single` for vanilla AQMs, `Fq` for per-flow-queue variants, `Ds4` for Stratum-specific composites — the plotter uses this to colour-code points on the ellipse diagram.

### Deep-dive

[Stratum architecture and design](II-02-stratum-architecture.md)

### Found a problem?

[File a recipe issue](https://github.com/digitalities/stratum-ns3/issues/new?template=recipe-request.yml)

---

## Recipe: Add your own scheduler cell to the registry

**You'll**: Register a new scheduler dispatch name with the substrate's scheduler registry — the analogue of the AQM registry for service-policy strategies.

**Time**: 15 min

**You'll learn**:
- The shape of a `SchedulerEntry` (dispatch name, file tag, factory closure)
- How the scheduler registry is the second `Registry<EntryT>` instantiation (n=9 cells in-tree today)
- How to verify the registration via the wireless or DiffServ scheduler-comparison example

**Prerequisites**: [Quickstart](I-02-quickstart.md); previous recipe (added an AQM cell)

### Run it

```bash
cd ns-3  # or your ns-3 tree (standalone flow)
./ns3 run "diffserv-wifi-scheduler-comparison --scheduler=list"
```

An invalid scheduler name (`list` matches nothing) triggers the example's error path, which prints the 9 in-tree scheduler cells via `SchedulerRegistry::Get().FileTags()`: **PQ / RR / WRR / WIRR / SCFQ / SFQ / WFQ / WF2Q+ / LLQ**. After registering your scheduler, the same command lists 10.

### How it works

`SchedulerEntry` mirrors `AqmEntry` with a different factory signature — schedulers don't need a `DataRate` because rate is set at the queue disc, not the scheduler:

```cpp
// In model/stratum-scheduler-registry.cc:
Register({"mysched",                                      // fileTag (dispatch key)
          "MySched",                                      // displayName
          F::FairQueue,                                   // family
          P::FairQueueShares,                             // parameterShape
          true,                                          // needsLinkBandwidth
          "My custom fair-queue scheduler",              // description
          [](const SchedulerArgs& a) -> Ptr<Scheduler> {
              return CreateObjectWithAttributes<MySchedScheduler>(
                  "NumQueues", UintegerValue(a.numQueues));
          }});
```

The dispatch loop in `diffserv-wifi-scheduler-comparison.cc` (and any other CLI that takes `--scheduler=`) consults the registry rather than a hardcoded `if (scheduler == "PQ")` ladder.

### Try changing

1. Implement two flavours of the same scheduler: register `MySched-Strict` and `MySched-Loose` from the same C++ class, differing only in an internal weight attribute. Run both via the wireless comparison example and compare per-class throughput in the output CSV.
2. Add a `--scheduler-arg=` CLI knob to forward attribute values into your factory. Verify it propagates by setting a knob that visibly changes simulation output.
3. Extend the scheduler registry to support a `family` enum (like the AQM registry's `Family::Single/Fq/Ds4`) so the plotting scripts can group schedulers by strategy class. This is a small refactor; it lifts a pattern across both registries.

> [!TIP]
> Both registries share the `Registry<EntryT>` template under `model/stratum-registry.h`. If you find yourself adding the same boilerplate to both `AqmRegistry` and `SchedulerRegistry`, that's a signal to lift it into `Registry` itself — the meter registry (a third planned instantiation) will inherit your fix.

### Deep-dive

[Stratum architecture and design](II-02-stratum-architecture.md)

### Found a problem?

[File a recipe issue](https://github.com/digitalities/stratum-ns3/issues/new?template=recipe-request.yml)
