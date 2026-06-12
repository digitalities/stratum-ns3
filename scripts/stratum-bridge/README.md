# Stratum-bridge prototype: scenario-emission half

A minimal demonstration of the `Stratum-bridge` architecture sketched in
the paper's §7 (`paper/diffserv4ns-substrate.tex`). The prototype takes a
Stratum scenario configuration (YAML) and emits a self-contained Linux
netns testbed that delivers the equivalent Linux measurement, using the
existing `cake-host-fairness` Lima VM as the kernel host.

## What this proves

The paper claims the substrate's four strategy slots admit an
alternative *Linux-netns backend* per slot, and that a scenario's
configuration is enough to emit the equivalent netns testbed. This
prototype validates the scenario-emission half: the same configuration
that ns-3 reads can also be emitted as a runnable Linux measurement, and
the Linux measurement reproduces the per-protocol Linux band already
recorded in §5.4 (Fig. `cake-host-fairness-protocols`).

This is *not* a real-time bridge (no packet-level proxying between ns-3
and Linux); it is a configuration-level bridge sufficient to demonstrate
the architectural primitive.

## Scenario IR (v1)

```yaml
schema: stratum-bridge/scenario/v1
name: <slug>
description: <one-line summary>

topology:
  kind: shared-bottleneck          # only one kind supported in v1

bottleneck:
  bandwidth_mbps: <int>            # e.g. 100
  one_way_delay_ms: <int>          # netem delay applied at router-a egress

hosts:
  a:
    flow_count: <int>              # N (host-A flow count)
  b:
    flow_count: <int>              # M (host-B flow count)

qdisc:
  kind: cake                       # only cake supported in v1
  mode: triple-isolate             # cake host-isolation mode

traffic:
  protocol: cubic | newreno | bbr | udp
  duration_s: <int>                # iperf3 -t value
  replicas: [<int>, ...]           # RngRun values to sweep

backend:
  kind: linux-netns
  vm: cake-host-fairness           # Lima VM name (see scripts/cake-host-fairness-lima-harness.sh)
```

## Usage

```bash
# Emit a runnable bash script for a scenario:
python3 scripts/stratum-bridge/emit-netns.py \
  scripts/stratum-bridge/scenarios/cake-16-1-cubic.yaml \
  > /tmp/cake-16-1-cubic.sh

# Run it via the existing Lima harness:
bash scripts/cake-host-fairness-lima-harness.sh           # one-time provenance
bash /tmp/cake-16-1-cubic.sh                              # measurement
```

The emitted script produces a per-flow goodput CSV matching the format
of `output/ns3/cake-host-fairness/sweep-perflow-linux.csv` (see
`scripts/cake-host-fairness-lima-sweep.sh` for the canonical Lima
testbed; the emitter generates a single-cell equivalent).

## Validation

The (16, 1) CUBIC and (16, 16) sym-control scenarios under
`scripts/stratum-bridge/scenarios/` reproduce the Linux band already
measured on the `cake-host-fairness` Lima testbed (the sweep that
produced the recorded values below; see
`scripts/cake-host-fairness-lima-sweep.sh`):

| Scenario | Expected share_A (mean ± std) |
|---|---|
| `cake-16-1-cubic` | 0.5201 ± 0.004 |
| `cake-16-16-cubic` | 0.4999 ± 0.0002 |

## Scope

Out of scope for v1 (deferred to a follow-up paper or v2):
- Per-strategy backend selection inside the ns-3 process
  (`DsCakeHelper::SetBackend(DsBackend::Linux)` C++ API stub)
- Packet-level proxying between ns-3 and a real Linux kernel
- Topologies beyond a single shared bottleneck
- Qdiscs beyond `cake triple-isolate`

The architectural primitive — *scenario-configuration parity is
sufficient to cross the fidelity boundary* — is what this prototype
demonstrates.
