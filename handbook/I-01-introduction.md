---
title: Part I introduction
origin: 2026-written
status: filled
last-updated: 2026-06-07
---

# Part I: Using Stratum — introduction

Part I is a task-oriented guide to Stratum. Each chapter is a short,
self-contained set of recipes for one client or capability. The goal is a
working simulation, not a complete explanation of how the substrate works
internally — Part II covers the design.

## Where to start

Read [the quickstart](I-02-quickstart.md) first. It builds the module,
runs a DiffServ scenario, previews CAKE and L4S, and modifies
an example — all in about 15 minutes. Every other chapter in Part I
assumes a built and tested tree.

## Chapter map

| Chapter | File | Purpose |
|---|---|---|
| I-02 | [Quickstart](I-02-quickstart.md) | Build Stratum, run a DiffServ scenario, preview CAKE and L4S, and modify an example — from clone to working simulation in 15 minutes. |
| I-03 | [DiffServ recipes](I-03-diffserv.md) | DiffServ recipes: single-edge foundation, meter configuration, DSCP-based PHB tables, AQM choice, and the full multi-tier pipeline. |
| I-04 | [L4S recipes](I-04-l4s.md) | L4S recipes: ECT(1) classification, DualPI2 coupled scheduler, coupling-formula validation, and head-to-head comparison with FqCoDel. |
| I-05 | [CAKE recipes](I-05-cake.md) | CAKE recipes: substrate demo, RRUL benchmark, host-pair isolation, and TCP fairness under load using the FqCobalt-based tin model. |
| I-06 | [AQM evaluation recipes](I-06-aqm-eval.md) | AQM evaluation recipes: invoke `aqm-eval-runner` for a single AQM cell, enumerate the full catalogue from the CLI, interpret per-flow goodput/delay CSV output and the throughput-vs-latency ellipse plot, sweep the complete matrix. |
| I-07 | [Wireless recipes](I-07-wireless.md) | Attach the substrate's edge queue disc to an ns-3 802.11ax access point, then run eight of the nine registry schedulers and compare per-class throughput and latency. |
| I-08 | [Examples catalogue](I-08-examples-catalog.md) | Annotated catalogue of the runnable examples shipped under `examples/`, grouped by client. |
| I-09 | [Extending the substrate](I-09-extending.md) | How to add an external AQM or scheduler via the `Registry<EntryT>` template so that it propagates to the CLI catalogue, plot palettes, and smoke tests automatically. |
| I-10 | [Troubleshooting](I-10-troubleshooting.md) | Known gotchas, build-environment issues, and diagnostic guidance. |
| I-11 | [Feedback](I-11-feedback.md) | How to report a bug, propose a feature, or reach the maintainer. |

## Conventions

All commands in Part I run from the **ns-3 root directory** (`ns3/ns-3-dev/`),
not from inside `src/ns-3/`. Build with:

```
./ns3 build stratum
python3 test.py -s stratum -v
```

CLI flags shown in the examples catalogue (I-08) are the canonical reference
for each example's command-line options. When a recipe quotes an expected
output range, the source of that range is noted inline — either a cited
reference or the chapter that documents the validation.
