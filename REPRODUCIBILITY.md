# Reproducibility Guide — Stratum (ns-3 module)

This guide covers full reproduction of the accompanying paper's results
from the Stratum ns-3 module.  It does not cover ns-2 simulation; those
steps live in the companion repository
[github.com/digitalities/diffserv4ns](https://github.com/digitalities/diffserv4ns).

---

## Prerequisites

- CMake ≥ 3.25, a C++23 compiler, Python 3.8+
- For Linux cross-validation (optional): a Lima VM or bare-metal Linux
  host with `sch_cake`, `iperf3`, `jq`, `bc`, `tshark`, `tcpreplay`

---

## 1. Installation

Choose one of two install paths described in `README.md`.

### Option A — Into an existing ns-3 tree (recommended)

```bash
# Clone ns-3-dev and check out the pinned release. The pin lives ONLY in
# the fetch script — query it, never hardcode it.
git clone https://gitlab.com/nsnam/ns-3-dev.git ns-3
cd ns-3
git clone https://github.com/digitalities/stratum-ns3.git contrib/stratum
git checkout "$(contrib/stratum/scripts/fetch-ns3.sh --print-pin)"
for p in contrib/stratum/patches/ns3/*.patch; do git apply "$p"; done
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
```

### Option B — Script-managed sibling clone

```bash
git clone https://github.com/digitalities/stratum-ns3.git
cd stratum-ns3
./scripts/fetch-ns3.sh   # clones the pinned ns-3 as a sibling dir, applies patches, creates the contrib/stratum symlink
cd ../ns-3
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
```

Both paths target the pinned **ns-3.48** release. The exact pinned commit
lives only in `scripts/fetch-ns3.sh` — query it with `--print-pin`;
`fetch-ns3.sh` checks it out automatically.

---

## 2. Verify the build

```bash
# From the ns-3 directory — every Stratum suite (test.py -s takes a glob):
python3 test.py -s 'stratum*' -v

# ...or include tcp-gso-egress too by looping the discovered suites:
for s in $(./ns3 run "test-runner --print-test-name-list" 2>/dev/null \
           | grep -E '^(stratum|tcp-gso-egress)'); do
  python3 test.py -s "$s"
done
```

All suites should report PASS (257 tests across 17 suites, 0 failures on
a clean build).

---

## 3. Reproduce paper results

The script `scripts/reproduce-paper.sh` orchestrates all ns-3-side
measurement steps and writes a pass/fail summary to
`output/paper/reproduction-status.md`.

### 3.1 List steps

```bash
./scripts/reproduce-paper.sh --list
```

Steps and their tiers (`smoke` = fast, `full` = complete run):

| Tier | Step |
|------|------|
| smoke | Core suites + RFC 2697/2698/2859 meter conformance vectors |
| smoke | L4S DualPI2 coupled marking + RFC 9331/9332 vectors |
| smoke | CAKE calibration suite |
| full | Scenario 1/2/3 ns-3 simulation |
| full | Scheduler GPS-convergence (Chang) sweep |
| full | Parekh-Gallager Theorem 1 latency-bound gate |
| full | AQM characterisation envelope (13 AQMs × 9 scenarios) |
| full | CAKE + L4S composition fairness |

### 3.2 Quick smoke run (suites only)

```bash
./scripts/reproduce-paper.sh --smoke
```

Expected runtime: 3–10 minutes depending on CPU speed.

### 3.3 Full ns-3-side reproduction

```bash
./scripts/reproduce-paper.sh
```

Expected runtime: 30–90 minutes.  The AQM characterisation step
(13 AQMs × 9 scenarios) is the most compute-intensive.

Results and figures are written under `output/`.

### 3.4 Environment variable

`NS3_DEV` overrides the path to the built ns-3 tree. When unset, the
scripts probe the known layouts in order: a sibling `ns-3/` (or legacy
`ns-3.48/` / `ns-3-dev/`) directory, then the parent tree when the module
is installed as `contrib/stratum`.

```bash
NS3_DEV=/path/to/ns-3 ./scripts/reproduce-paper.sh
```

---

## 4. Cross-simulator validation (optional)

The three-way ns-2 ↔ ns-3 comparison requires ns-2 baseline traces
produced by the companion repository.

### 4.1 Produce ns-2 baselines

In a separate clone of
[github.com/digitalities/diffserv4ns](https://github.com/digitalities/diffserv4ns):

```bash
# Prerequisites: Docker, ns-2.29 and ns-2.35 built via the
# companion repo's fetch and build scripts.
./scripts/reproduce-ns2-baselines.sh
```

Baseline traces land under the companion repo's `output/` tree with
this layout (the layout that `scripts/compare-three-way.py` expects):

```
<baselines-root>/output/ns2-29/example-1/
<baselines-root>/output/ns2-29/example-3-fullscale/
<baselines-root>/output/ns2-35/example-1/
<baselines-root>/output/ns2-35/example-2-fullscale/
<baselines-root>/output/ns2-35/example-2-fullscale-bulktcp/
<baselines-root>/output/ns2-35/example-3-fullscale/
<baselines-root>/output/ns2/example-2-fullscale/
```

### 4.2 Run the three-way comparison

Pass the companion repo root as `--ns2-baselines`:

```bash
./scripts/reproduce-paper.sh --ns2-baselines /path/to/diffserv4ns
```

Cross-simulator figures are written to `output/three-way-figures/`.

`--fetch-ns2-baselines` is reserved for a future pinned-tag auto-fetch
and currently exits with an error directing you to the steps above.

---

## 5. Linux cross-validation (optional)

Three paper results require a Linux host running `sch_cake`:
the host-fairness anchor (§5.4), the trace-replay probe (§5.5), and
the Stratum-bridge prototype (§7).  These steps are not automated by
`scripts/reproduce-paper.sh` because they require provisioning a
separate Linux environment.

### 5.1 Provision (macOS host, Lima)

```bash
limactl start --name=cake-host-fairness --tty=false template://ubuntu
limactl shell cake-host-fairness sudo apt-get install -y \
    iperf3 jq bc tshark tcpreplay
# Verify sch_cake is loadable:
bash scripts/cake-host-fairness-lima-harness.sh
```

### 5.2 Run

Each harness script is self-contained and documents its expected output
in its header comment.  Results are written to `output/` under the
corresponding subdirectory.

---

## 6. Determinism notes

ns-3 simulations in this module are fully deterministic for a given RNG
run index.  The test suite fixes `--RngRun=1` throughout.  To verify
repeatability, re-run any test suite twice and diff the output — results
should be bit-identical.

For the host-fairness sweep, three seeds (RNG runs 1, 2, 3) are used to
bound variance; the script reports per-seed and aggregate figures.

---

## 7. What is not reproduced by `scripts/reproduce-paper.sh`

- **Three-way ns-2 ↔ ns-3 comparison** — pass `--ns2-baselines` to enable
  (see §4 above).
- **Stratum vs Linux CAKE cross-validation** (host-fairness anchor, trace-
  replay, Stratum-bridge) — needs a Lima VM or bare-metal Linux with
  `sch_cake` (see §5 above).
