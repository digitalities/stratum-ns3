# AQM-eval — DiffServ4NS characterisation suite scripts

This directory ships the user-facing scripts for the AQM-eval
characterisation harness:

| File | Purpose |
|---|---|
| `aqm-eval` | Single-binary CLI wrapper (Bash, no deps). Hides the ns-3 build ceremony and exposes subcommands for every common operation. **Recommended entry point.** |
| `ellipse-plot.py` | Reads the 117-cell matrix and emits the three-panel ellipse + Jain-range figure. Called by `aqm-eval plot`. |

The runner binary itself lives at
`src/ns-3/examples/aqm-eval-runner.cc`; this directory contains the
*operator surface* over that binary, not the binary itself.

## Quick start

```bash
./scripts/aqm-eval/aqm-eval --help               # subcommand catalogue
./scripts/aqm-eval/aqm-eval reproduce            # paper figure 5, end-to-end
./scripts/aqm-eval/aqm-eval list aqms            # canonical 13 (both L4S variants)
./scripts/aqm-eval/aqm-eval list scenarios       # canonical 9
```

## Subcommands

Each subcommand has its own `--help` with options and behaviour.

| Subcommand | Purpose |
|---|---|
| `setup` | `scripts/fetch-ns3.sh` + configure ns-3 + build `aqm-eval-runner`. Idempotent. |
| `run` | One (scenario, AQM) cell. Required: `--scenario`, `--aqm`. |
| `matrix` | Canonical 13-AQM × 9-scenario sweep (117 cells, ~30 s wall-clock). |
| `plot` | Render the three-panel figure from a matrix directory. |
| `bistable` | 5-RngRun FqPie sweep + verdict against the F-C bistable signature. |
| `reproduce` | Gold path — `setup` + `matrix` + `plot`, end-to-end from a clean checkout. The Zenodo-artefact one-liner. |
| `list` | Canonical scenarios / AQMs catalogue. |
| `clean` | Remove a matrix output directory (refuses paths outside `output/aqm-eval/`). |

## What the figure shows

Three panels over the canonical 117-cell matrix (13 AQMs × 9
RFC-7928 scenarios):

- **Panel A — qdelay × goodput, 1σ ellipse per AQM.** Stratum-aware
  queue discs (`StratumRed`, `StratumL4s`, `StratumCake`) sit inside the mainline
  envelope — empirical evidence for substrate composability.
- **Panel B — retx × goodput, 1σ ellipse per AQM.** Surfaces the
  L4S DualPI2 mark signature (~1 %).
- **Panel C — per-AQM Jain min..max** over the three TCP scenarios.
  Refuses to average; surfaces the four characterisation findings
  (F-A, F-C, F-D, A3-StratumCake).

## Where the documentation lives

The harness architecture, scenario topology, the four findings, and the
bistable-verification protocol are documented in the suite's design
records (ADR-0048) and summarised in the accompanying publication.

## Cross-references

- Runner source: `src/ns-3/examples/aqm-eval-runner.cc`
- Pinned ns-3 commit: `scripts/fetch-ns3.sh --print-pin` + two `internet/`
  patches under `patches/ns3/`. **No** mainline traffic-control
  AQM is patched.
- Substrate-client framing: chapters 03, 06 (DiffServ), 10 (L4S),
  11 (CAKE).
- Lineage: NITK 2017 `aqm-evaluation-suite`
  (DOI 10.1145/3067665.3067674); ellipse-plot lineage TCP Ex Machina
  (DOI 10.1145/2486001.2486020).
- RFCs: 7928 (characterisation methodology), 9331/9332 (L4S),
  8033 (PIE — note that §5.1 is "ECN Support", not FQ-PIE).
