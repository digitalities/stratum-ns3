# Stratum-Bridge closed-loop

Closed-loop packet-level bridging of Linux TCP through Stratum CAKE in ns-3.
Tests whether the TCP runtime / simulator-execution model is the dominant
differentiator of the host-fairness residual between the ns-3 and Linux
CAKE implementations.

Sibling to `scripts/stratum-bridge/emit-netns.py` (static scenario-emission
half, ADR-0112): this directory is the **packet-level closed-loop half**.

## Topology

```
Lima VM (one Linux kernel, single host)

  netns src-a  ── router-a ── [veth] ── ns3-rt (ns-3 process; CAKE on tx)
  netns src-b  ── router-a              │
                                 [veth] ── router-b ── sink-a
                                                       sink-b
```

ns-3's `cake-stratum-bridge-router` example runs inside `ns3-rt`, binds raw
sockets on the two veth ends via `EmuFdNetDevice`, and routes between them
with patched-mainline `FqCobaltQueueDisc` (CAKE diffserv4 tin composition,
`HostIsolationMode=Triple`, 100 Mbps) on the forward egress and a no-op
`FifoQueueDisc` on the reverse.

## One-command reproduction (inside Lima `cake-host-fairness`)

```bash
cd /path/to/diffserv4ns
sudo bash scripts/stratum-bridge/closed-loop/run.sh
```

Wall-clock: ~15 min (3 replicas × ~5 min + setup/teardown + verdict).

## Smoke check

Before the full run, verify the bridge passes ICMP with no shaping:

```bash
sudo bash scripts/stratum-bridge/closed-loop/smoke-ping.sh
```

Expected: `SMOKE PASS: bridge passes ICMP.`

If FAIL, diagnose `/tmp/closed-loop-smoke-ns3.stderr` before attempting the
full run.

## Outputs

All under `output/stratum-bridge-closed-loop/`:

- `verdict.json` — primary verdict (VALIDATED / INVALIDATED / OUT_OF_RANGE),
  strength qualifier, CI95, bands git SHA
- `provenance-env.json` — environment manifest at run time
- `r{1,2,3}/` — per-replica:
  - `iperf3-src-{a,b}.json` — bytes-sent counters
  - `src-a.pcap`, `sink-a.pcap` — Linux-side captures
  - `ns3-enqueue.pcap`, `ns3-dequeue.pcap` — ns-3 qdisc-trace captures
  - `provenance.json` — RngRun + ns-3 commit + diffserv SHA
  - `ns3.stdout`, `ns3.stderr` — ns-3 logs

## Pre-registration audit trail

`bands.yaml` is committed to git BEFORE any data collection. The verdict
script records the bands' git SHA in `verdict.json`. Any post-hoc
adjustment shows in `git log scripts/stratum-bridge/closed-loop/bands.yaml`.

## Files

| File | Purpose |
|---|---|
| `bands.yaml` | Pre-registered acceptance bands |
| `setup-netns.sh` | Build 7-netns topology + veth + routes |
| `teardown-netns.sh` | Clean removal |
| `smoke-ping.sh` | Bridge smoke check (no qdisc, single ICMP) |
| `provenance-snapshot.sh` | Capture env manifest |
| `run.sh` | Top-level orchestration |
| `verdict.py` | Aggregate replicas → classify |
| `README.md` | This file |
