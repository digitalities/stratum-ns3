# Stratum

<img src="doc/stratum-icon-100x100.png" width="96" align="right" alt="Stratum logo">

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](LICENSE)
[![ns-3](https://img.shields.io/badge/ns--3-3.48_pinned-2c8ebb.svg)](https://www.nsnam.org/)
[![Tests](https://img.shields.io/badge/tests-257_passing_%2F_17_suites-success.svg)](test/)
[![Platforms](https://img.shields.io/badge/platforms-Linux_%7C_macOS-lightgrey.svg)](REPRODUCIBILITY.md)

*Stratum is a QoS **substrate** for ns-3: one composer with four pluggable
strategy slots, on which DiffServ, L4S, and CAKE are expressed as three
clients — each picking one strategy per slot.*

> **This repository is the standalone home of Stratum** — the ns-3 QoS substrate
> composing DiffServ, L4S, and CAKE, now developed in its own repository. It carries
> forward the [DiffServ4NS](https://github.com/digitalities/diffserv4ns) lineage; the
> namespace and class rename to `ns3::stratum` is recorded in the [CHANGELOG](CHANGELOG.md).

This is the active variant of the DiffServ4NS lineage. Where the ns-2
archive preserves the 2001 module as a faithful historical reference,
the ns-3 substrate is a new whole that absorbs the DiffServ algorithms
as one of three composed peer mechanisms. The ns-2 sources are at
[github.com/digitalities/diffserv4ns](https://github.com/digitalities/diffserv4ns).

## The substrate: four strategy slots

Every Stratum node builds a queueing discipline from four pluggable slots,
populated independently per node — an edge and a core node need not carry the
same strategies (RFC 2475 §2.4):

1. **Classify-and-Meter** — a meter strategy plus a classifier: *which*
   traffic, and *how much* of it.
2. **Mark-and-Route** — a DSCP tag plus a PHB table: how packets are labelled
   and steered.
3. **Per-class slot array** — up to eight inner queue discs: *where* packets
   wait.
4. **Across-slot service policy** — the scheduler / dispatcher: *who* is
   served next.

A *client* is a coherent choice of one strategy per slot — nothing more.

## Three clients, one substrate

DiffServ, L4S, and CAKE are the same substrate populated three ways:

| Client | Classify-and-Meter | Mark-and-Route | Slot array | Service policy |
|---|---|---|---|---|
| **DiffServ** (RFC 2475) | sr-TCM / tr-TCM / TSW2CM·3CM + byte-accounting | DSCP + PHB table | RED / RIO / drop-tail | PQ · WFQ · WF2Q+ · LLQ · hybrid-LLQ · WRR · SCFQ · SFQ |
| **L4S** (RFC 9331/9332) | DualPI2 ECT(1) classifier | DSCP + L4S identifier | classic + L4 inners | coupled marking probability |
| **CAKE** (`sch_cake` parity) | DSCP-to-tin (besteffort … diffserv8) | tin assignment | per-tin FqCobalt | across-tin DRR++ + optional LLQ |

Each client is a complete implementation in its own right; bringing all three
together on one substrate lets a single ns-3 edge combine DiffServ
class-awareness, low latency, and per-flow isolation — which no single
component delivers alone.

## Extensibility — the registry

External AQMs and schedulers plug in as cells in a `Registry<EntryT>`
template — no fork, no edits to the composer core. Downstream consumers (CLI
catalogues, plot palettes, handbook tables, smoke tests) auto-derive their
coverage from the registry, so adding a cell propagates everywhere without
manual fan-out. In-tree today: **13 AQM cells and 9 scheduler cells**.

## Highlights

Beyond the inherited 2001 DiffServ4NS algorithms:

- **Faithful schedulers, not approximations:** WFQ uses a Parekh-style
  snapshot *V(t)*; WF2Q+ follows Bennett-Zhang; SCFQ, SFQ, and LLQ round out
  the original PQ / WRR / hierarchical set.
- **Meters:** sr/tr-TCM and TSW2/3CM with per-meter RNG isolation for
  reproducible random streams.
- **CAKE depth:** host-pair isolation, ACK filtering (conservative +
  aggressive), optional per-tin TBF shaping, `MemLimit` byte-cap eviction,
  named link-layer-overhead and RTT presets, and `tc-cake(8)` operational
  vocabulary.
- **Monitoring:** per-DSCP frequency-distributed OWD and IPDV; per-tin
  byte / packet / drop / mark counters matching `tc -s qdisc show`;
  per-flow goodput.
- **IPv6:** DiffServ, L4S, and CAKE all work over both IPv4 and IPv6; the DS-field and ECN handling is address-family agnostic.
- **Reproducibility:** the accompanying paper's figures reproduce from this
  tree via `scripts/reproduce-paper.sh`.

## Installation

### Into an existing ns-3 tree (recommended)

```bash
# from scratch — clone ns-3, then check out the pinned release (the pin
# lives ONLY in the fetch script; query it instead of hardcoding it)
git clone https://gitlab.com/nsnam/ns-3-dev.git ns-3
cd ns-3
git clone https://github.com/digitalities/stratum-ns3.git contrib/stratum
git checkout "$(contrib/stratum/scripts/fetch-ns3.sh --print-pin)"
# apply the bundled mainline patches (required)
for p in contrib/stratum/patches/ns3/*.patch; do git apply "$p"; done
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
python3 test.py -s stratum -v
```

Already have an ns-3 tree? `cd` into it instead — it must be at the
pinned release tag (`scripts/fetch-ns3.sh --print-pin`) for the bundled
patches to apply cleanly.

### Standalone (script-managed sibling clone)

```bash
git clone https://github.com/digitalities/stratum-ns3.git
cd stratum-ns3
./scripts/fetch-ns3.sh        # clones the pinned ns-3 as a SIBLING dir (ns-3/), patches, symlinks
```

`fetch-ns3.sh` clones the pinned ns-3 revision (see `--print-pin`),
applies the patches in `patches/ns3/`, and creates the `contrib/stratum`
symlink that ns-3 expects.

### Install via the ns-3 App Store / bake

The module is being prepared for the [ns-3 App Store](https://apps.nsnam.org);
the listing is not live yet. The repository already ships a `bakeconf.xml`
module definition, so Bake works as a third install path alongside the two
above. (Bake itself must run under Python 3.11–3.13 with the `distro` and
`requests` packages installed.) Add `bakeconf.xml` to Bake's `contrib/` folder,
then:

```bash
./bake.py configure -e ns-3.48 -e stratum-1.0
./bake.py download
```

`bakeconf.xml` defines the pinned `ns-3.48` release, so `download` checks the
ns-3 tree out at the pin and places Stratum in `source/ns-3.48/contrib/stratum`.
Stratum depends on a small set of ns-3 mainline patches; apply them to the
downloaded tree before building:

```bash
NS3=source/ns-3.48      # the ns-3 tree Bake downloaded
for p in "$NS3"/contrib/stratum/patches/ns3/*.patch; do
  ( cd "$NS3" && git apply "$p" )
done
./bake.py build
```

Applying mainline patches as the first build step follows the ns-3
[external-module guide](https://www.nsnam.org/docs/contributing/html/external.html)
(section 5.2) for patch-carrying modules.

## Test suites

The module registers a suite per component area. ns-3's `test.py -s`
takes a single exact suite name, so run them individually or loop over
the module's full set:

```bash
python3 test.py -s stratum -v           # DiffServ core (+ RFC 2697/2698 vectors)
python3 test.py -s stratum-l4s -v       # L4S DualPI2 (RFC 9331/9332)
python3 test.py -s stratum-cake-q15 -v  # CAKE composition

# Run every stratum-module suite in turn:
for s in $(./ns3 run "test-runner --print-test-name-list" 2>/dev/null \
           | grep -E '^(stratum|tcp-gso-egress)'); do
  python3 test.py -s "$s"
done
```

| Component | Suites |
|---|---|
| **DiffServ core** | `stratum`, `stratum-meter-trace`, `stratum-per-flow-classifier`, `stratum-wf2qp-regression`, `stratum-q16-chang-convergence`, `stratum-q17-parekh-theorem1` |
| **L4S** | `stratum-l4s`, `stratum-count-ack-jitter`, `tcp-gso-egress` |
| **CAKE** | `stratum-cake-q15`, `stratum-cake-host-iso-phase-1`, `stratum-cake-host-fairness-smoke`, `stratum-cake-host-fairness-udp-smoke`, `stratum-flent-sink` |
| **Instrumentation** | `stratum-example-1-instrumentation`, `stratum-empirical-cdf-loader`, `stratum-trace-replay-application` |

RFC conformance vectors run inside these registered suites: RFC 2697 /
2698 metering in `stratum` and `stratum-meter-trace`, and RFC 9331 /
9332 L4S identification and coupling in `stratum-l4s`.

## Citation

A `CITATION.cff` at the repository root provides machine-readable
citation metadata for this software. A paper describing the substrate is
under peer review; its citation and DOI will be added here once it is
published. Until then, please cite the software repository and — for the
inherited DiffServ4NS lineage — the 2001 thesis below.

## The 2001 thesis

The architectural lineage starts with Sergio Andreozzi's MSc thesis
(University of Pisa, 2001) — the authoritative design document for
the original DiffServ4NS module. The thesis is a separate Zenodo
deposit:

- **Concept DOI:** [10.5281/zenodo.19662899](https://doi.org/10.5281/zenodo.19662899)
- **Version DOI:** [10.5281/zenodo.19662900](https://doi.org/10.5281/zenodo.19662900)

Key sections relevant to the substrate:

- Chapter 3.3.3 — module design specification (inherited algorithms).
- Chapter 4 — three simulation scenarios with results.
- Appendix A — validation against real-network measurements.

## Architectural lineage

The substrate is not "DiffServ4NS v3". It is a new whole that includes
the ported DiffServ algorithms as one of three peer components,
composed alongside L4S and CAKE under a single ns-3 edge queue disc.
The 2001 module and the 2026 substrate are peer artifacts that share
intellectual ancestry but differ in scope: the former implements
DiffServ on ns-2; the latter composes DiffServ + L4S + CAKE on ns-3.

For the full 25-year chain see
[github.com/digitalities/diffserv4ns](https://github.com/digitalities/diffserv4ns).

## Acknowledgements

The substrate builds on the work of others:

- **Nortel Networks** (2000): the ns-2 DiffServ module that DiffServ4NS extended, inherited here as the classical-DiffServ client (Farhan Shallwani, Jeremy Ethridge, Peter Pieda, Mandeep Baines).
- **Xuan Chen, ISI** (2001): ns-2 integration of the Nortel module.
- **Pasquale Imputato and Stefano Avallone** (2016): the ns-3 traffic-control layer this substrate builds on.
- **The GPRT group** (2026): the ns-3 DualPI2 implementation (arXiv:2603.20166) the substrate's L4S marking and coupling are validated against (Maria Eduarda Veras, Eduardo Freitas, Assis T. de Oliveira Filho, Djamel Sadok, Judith Kelner).

## Licence

GPLv2, matching both ns-3 mainline and the DiffServ4NS-0.1 ancestor.
