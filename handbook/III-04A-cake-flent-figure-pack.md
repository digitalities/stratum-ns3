---
title: "CAKE Flent figure pack"
origin: 2026-written
status: filled
last-updated: 2026-06-06
---

# CAKE Flent figure pack

This appendix documents two operating-point reproductions that exercise
the CAKE prototype (described in [The CAKE client](II-07-cake-client.md))
through the Flent export pipeline introduced in
*AQM-eval interop: Flent JSON workflow*. The two subjects are:

1. **TCP square-wave fairness** — four staggered bulk flows share a single
   bottleneck shaped by the DRR dispatcher; the characteristic square-wave
   throughput stack shows whether the active-flow set splits the link
   equally.
2. **DiffServ-marked RRUL** — eight TCP flows each marked with one of the
   four DiffServ4 DSCPs traverse a bottleneck with per-tin rate caps; the
   figure shows whether each tin receives its expected share.

A third subject — **host-isolation contrast** (two hosts competing under
flowblind vs triple-isolate) — requires a research example with an
`--isolation` CLI toggle. The smoke fixture
`cake-flent-host-attribution-smoke.cc` provides coverage of the
`FlentCsvSink::AddTcpUpFlow(idx, sink, hostId)` non-empty-`hostId`
path (restoring the test coverage noted in the Flent-sink host-attribution
erratum), but it is a purpose-built smoke test, not a figure source; the
host-isolation contrast figure is deferred to a follow-up research
example.

The three-step pipeline from the sibling chapter applies to all three:

1. `FlentCsvSink` emits a per-flow CSV bundle during the simulation.
2. `ns3-csv-to-flent` converts the bundle to a Flent v4 `.flent.gz` file.
3. `flent --plot=...` renders the figure.

> **Substrate-validation scope, not parity claim.** The figures documented
> here are Stratum-only — they do **not** include a Linux `tc-cake` reference
> run for comparison. A side-by-side comparison requires either a real
> testbed running Flent natively against `tc-cake`, or a Flent Application
> API integration project that is out of scope for this appendix. The figures
> below show that Stratum emits Flent-compatible bundles and that the CAKE
> prototype produces the qualitative behaviours the figure subjects claim; a
> quantitative comparison against Linux `tc-cake` is a follow-up.

## Operating point and sweep matrix

The sweep runner (`scripts/flent-export/run-figure-pack.sh`) covers a
`(10 Mbps / 100 Mbps / 1000 Mbps) × (10 ms / 40 ms / 100 ms)` matrix per
figure. The canonical cell used in the descriptions below is `(100 Mbps,
40 ms)` at a 60-second simulation length.

```bash
# Build the two examples (assumes scripts/fetch-ns3.sh has been run)
cd ns-3  # or your ns-3 tree (standalone flow)
./ns3 build cake-tcp-4up-squarewave cake-rrul-diffserv
cd -        # return to stratum-ns3/

# Run the full sweep (writes to output/cake-flent-figure-pack/...)
bash scripts/flent-export/run-figure-pack.sh

# Render the two canonical (100 Mbps, 40 ms) figures
mkdir -p handbook/figures/III-03A-flent
flent --plot=totals \
    -i output/cake-flent-figure-pack/fig4-tcp-4up-squarewave/100Mbps-40ms.flent.gz \
    -o handbook/figures/III-03A-flent/fig4-100Mbps-40ms-totals.png
flent --plot=totals \
    -i output/cake-flent-figure-pack/fig5-rrul-diffserv/100Mbps-40ms.flent.gz \
    -o handbook/figures/III-03A-flent/fig5-100Mbps-40ms-tin-stack.png
```

The sweep matrix has 2 figures × 9 cells = 18 cells, each run ≈30 s; the full
sweep takes 10–20 minutes on a typical laptop. The `.flent.gz` bundles are
deterministic given a fixed ns-3 build revision.

## Figure 4 — TCP square-wave fairness

Four TCP bulk transfers start at `t = 0 / 5 / 10 / 15 s` and stop in
reverse order at `t = 45 / 50 / 55 / 60 s`, all traversing a single
bottleneck shaped by the DRR-based dispatcher (four tins, DiffServ4 map,
no per-tin rate cap). The DRR fairness property predicts that the active
flows split the bottleneck equally from moment to moment, producing the
characteristic square-wave stack: one flow at full rate (0–5 s), two flows
each at half rate (5–10 s), three flows each at one-third (10–15 s), and
four flows each at one-quarter (15–45 s), then the mirror sequence as flows
stop.

- **Source:** `examples/cake-tcp-4up-squarewave.cc`
- **Schema:** `tcp_4up_squarewave` (four `tcp_up_flow{0..3}.csv` timeseries
  plus one `ping_icmp.csv`)
- **Render:** `flent --plot=totals -i fig4-...flent.gz -o ...png`

The `totals` plot overlays the four per-flow throughput timeseries on a
shared axis; the square-wave staircase is visible directly in that view.

## Figure 5 — DiffServ-marked RRUL

Eight TCP flows (four downloads + four uploads) plus four UDP probes plus
one ICMP ping traverse a dumbbell bottleneck. Each TCP flow carries one of
the four DiffServ4 DSCPs — BE, BK, CS5, EF — set via the `Tos` attribute
on `BulkSendApplication`. The bottleneck is configured with
`cake::Helper::SetAsCakeDiffserv4(edge, totalRate, {.tinShaping = true})`,
which routes each flow to the matching tin and hard-caps each tin's service
rate at its share of the link:

| Tin | DSCP | Share (DiffServ4 default) | Cap at 100 Mbps |
|---|---|---|---|
| Bulk | BK (CS1) | 6.25 % | 6.25 Mbit/s |
| Best-Effort | BE (CS0) | 100 % | 100 Mbit/s |
| Video | CS5 (approx.) | 50 % | 50 Mbit/s |
| Voice | EF | 25 % | 25 Mbit/s |

The figure shows per-tin throughput under load; the claims are that each
tin receives no more than its cap (hard ceiling in tin-shaping mode) and
that busy tins with room inside their cap get their share of the available
bottleneck capacity.

- **Source:** `examples/cake-rrul-diffserv.cc`
- **Schema:** `rrul` (reused from the `cake-rrul` example; the series labels
  BE / BK / CS5 / EF are now semantically accurate because each
  `BulkSendHelper` sets `Tos=kTosByFlow[i]` before `Install`, and
  `SourceApplication::DoStartApplication()` calls `Socket::SetIpTos(m_tos)`
  before the first segment — so every IP header carries its DSCP from
  byte one)
- **Render:** `flent --plot=totals -i fig5-...flent.gz -o ...png`

The `rrul` schema's positional flow labels (flow 0 → BE, flow 1 → BK,
flow 2 → CS5, flow 3 → EF) become semantically accurate in this example,
in contrast to the generic `cake-rrul` example where the labels are
positional aliases only. See *AQM-eval interop* § Limitations for the
general caveat.

## Host-isolation and FlentCsvSink hostId coverage

The host-isolation contrast figure (flowblind vs triple-isolate side-by-side)
requires a research example with a `--isolation` CLI toggle. That example
is a follow-up item; this appendix does not yet include Figure 6.

The `FlentCsvSink::AddTcpUpFlow(idx, sink, hostId)` non-empty-`hostId`
path — the API contract that populates the `host` column in the per-flow
CSV bundle — is exercised in-tree by
`examples/cake-flent-host-attribution-smoke.cc`. This smoke
fixture uses a 5-flow A/B topology (four flows from host A, one from
host B) with `HostIsolationMode=Triple` and verifies that the `host`
column carries distinct non-empty values for each contributing host. It
is the purpose-built coverage fixture for
`diffserv-flent-sink-test-suite.cc` and is not a figure source.

For reference, the host-isolation mechanism itself — `FqCobaltQueueDisc`
with `EnableHostIsolation=true` via the local ns-3 patch — is described
in [The CAKE client](II-07-cake-client.md#implementation-overview) and
quantified in [CAKE validation](III-04-cake.md#host-fairness-empirical-anchor),
including the ≤ 4.3 pp agreement with Linux `tc-cake`
across CUBIC, NewReno, and BBR at the host-fairness operating point.

## Schemas and converter

The Flent schemas that define each figure's bundle layout live under
`scripts/flent-export/ns3_csv_to_flent/schemas/`:

| Schema | Schema file | Bundle filenames |
|---|---|---|
| `tcp_4up_squarewave` | `tcp_4up_squarewave.py` | `tcp_up_flow{0..3}.csv`, `ping_icmp.csv` |
| `rrul` (reused) | `rrul.py` | `tcp_down_flow{0..3}.csv`, `tcp_up_flow{0..3}.csv`, `udp_probe_flow{0..3}.csv`, `ping_icmp.csv` |

Each schema has a golden CSV-bundle fixture under
`scripts/flent-export/tests/fixtures/` and a round-trip pytest that verifies
`metadata.NAME`, `TITLE`, `x_values`, per-flow series, totals arithmetic,
and `raw_values` event pairs (four tests per schema file).

## Reproducing this appendix

```bash
# 1. Build (assumes scripts/fetch-ns3.sh has been run)
cd ns-3  # or your ns-3 tree (standalone flow)
./ns3 build cake-tcp-4up-squarewave cake-rrul-diffserv
cd -        # return to stratum-ns3/

# 2. Run the sweep (writes output/cake-flent-figure-pack/)
bash scripts/flent-export/run-figure-pack.sh

# 3. Render the two canonical (100 Mbps, 40 ms) figures
mkdir -p handbook/figures/III-03A-flent
flent --plot=totals \
    -i output/cake-flent-figure-pack/fig4-tcp-4up-squarewave/100Mbps-40ms.flent.gz \
    -o handbook/figures/III-03A-flent/fig4-100Mbps-40ms-totals.png
flent --plot=totals \
    -i output/cake-flent-figure-pack/fig5-rrul-diffserv/100Mbps-40ms.flent.gz \
    -o handbook/figures/III-03A-flent/fig5-100Mbps-40ms-tin-stack.png
```

Wall-clock cost: 10–20 minutes for the sweep; sub-second per render call.

## Cross-references

- Client chapter: [The CAKE client](II-07-cake-client.md) (CAKE architecture, tin profiles,
  host-isolation implementation)
- Validation chapter: [CAKE validation](III-04-cake.md) (spec-tier coverage, host-fairness anchor)
- Flent export pipeline: [Appendix C — AQM-eval interop](appendix-C-aqm-eval-interop.md) (schema contract, converter
  usage, format limitations)
- Bundle layout contract: `scripts/flent-export/SCHEMA.md`
- Converter package: `scripts/flent-export/`
- Examples: `examples/cake-tcp-4up-squarewave.cc`,
  `examples/cake-rrul-diffserv.cc`,
  `examples/cake-flent-host-attribution-smoke.cc` (hostId smoke fixture)
- Sweep runner: `scripts/flent-export/run-figure-pack.sh`
