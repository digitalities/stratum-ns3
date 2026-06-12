# CAKE reference dataset (Q-15.6 calibration)

This directory holds a small JSON summary of the **Linux tc-cake**
reference dataset published alongside the CAKE paper, plus the parser
that produces it. The C++ Q-15.6 test (`../diffserv-cake-q15-test-suite.cc`)
loads the summary at compile time and gates ns-3 Stratum-CAKE outputs to be
within ±15 % of the Linux tc-cake equivalents.

## Source

* **DOI**: [10.5281/zenodo.1226887](https://doi.org/10.5281/zenodo.1226887)
* **Title**: *Piece of CAKE: A Comprehensive Queue Management Solution for Home Gateways*
* **Authors**: Toke Høiland-Jørgensen, Dave Täht, Jonathan Morton (2018)
* **Companion paper**: arXiv:1804.07617 (LANMAN 2018)
* **License**: CC-BY-SA-4.0 — derivative summary in this directory inherits the licence
* **Tarball**: `cake-paper-20180420-flent-data.tar.gz` (471 MiB, MD5 `613b2d1526491eb110af3a46e9fd4144`)
* **Linux kernel**: 4.11 endpoints / 4.14 routers
* **CAKE commit**: `16d7fed7ea0ef528d138cb7295aa51f55680ceef`

The tarball itself is **not tracked in git** — the small JSON summary
plus the parser are sufficient to regenerate calibration thresholds
on demand and to keep the repository under DOI-deposit-friendly size.

## Files

| File | Purpose |
|---|---|
| `cake-paper-summary.json` | Per-(test, bandwidth, qdisc) median + IQR across 10 reps for steady-state per-DSCP TCP throughput and per-probe latency p50/p95/p99 |
| `extract_cake_paper_summary.py` | Parser that emits the JSON from a fresh tarball download |

## Regenerating the summary

```sh
mkdir -p /tmp/cake-reference-data
curl -L -o /tmp/cake-reference-data/cake-paper-20180420-flent-data.tar.gz \
    "https://zenodo.org/api/records/1226887/files/cake-paper-20180420-flent-data.tar.gz/content"

cd /tmp/cake-reference-data
md5sum cake-paper-20180420-flent-data.tar.gz   # expect 613b2d1526491eb110af3a46e9fd4144
tar -xzf cake-paper-20180420-flent-data.tar.gz

cd <repo-root>
python3 src/ns-3/test/cake-reference-data/extract_cake_paper_summary.py \
    --root /tmp/cake-reference-data \
    --out  src/ns-3/test/cake-reference-data/cake-paper-summary.json
```

## Coverage

| Test | Bandwidths | Qdiscs |
|---|---|---|
| `rrul_diffserv` | 1Mbit-10Mbit, 10Mbit-10Mbit | cake, cake_diff3, cake_diff4, fq_codel |
| `tcp_32up_voip_diffserv` | 10Mbit-10Mbit | cake, cake_diff4, fq_codel |

The `rrul-diffserv` test marks four concurrent TCP flows as
BK (CS1) / BE (default) / CS5 / EF and runs them simultaneously
upstream and downstream, plus four UDP latency probes
(ICMP / UDP_BE / UDP_BK / UDP_EF).
Under `cake_diff4` the four DSCPs map onto three of the four
diffserv4 tins (BK→Bulk, BE→BE, CS5+EF→Voice; the Video tin is idle),
so the per-tin shares observed here are the **3-active-tin
equilibrium** rather than the four-tin Fig. 5 reading. The
calibration test reproduces the exact same traffic mix in ns-3 to
keep the comparison apples-to-apples.

## Calibration scope

Q-15.6 runs at `10Mbit-10Mbit` `cake_diff4` against the corresponding
JSON entry. The 1Mbit-10Mbit and 100Mbit-100Mbit-equivalent bands,
plus the `tcp-32up-voip-diffserv` 32-flow fairness comparison, are
captured in the JSON for v1.1 follow-ups.
