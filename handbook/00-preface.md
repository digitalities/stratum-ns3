---
title: Preface
origin: 2026-written
status: filled
last-updated: 2026-06-06
---

# Preface

This handbook is the long-form reference for the Stratum ns-3 substrate:
how to use it (Part I), how it is designed (Part II), and how its behaviour
has been validated (Part III). It is a companion to the 2001 thesis, not
the thesis itself. The thesis is preserved verbatim under
`provenance/Andreozzi-2001-thesis.{pdf,txt}` and is never modified in place.

The ns-2 heritage (the 2001 ns-2.29 original and the 2026 ns-2.35 port) lives
in the DiffServ4NS heritage repository. The three-way cross-simulator
comparison that connects the two ns-2 variants to the ns-3 substrate is
documented in Part III of this book.

## Who this handbook is for

Three audiences:

- **Researchers** reproducing the DiffServ4NS results from the 2001 thesis
  or the accompanying paper (submitted to ICNS3 2026), or citing the Zenodo
  release artifact.
- **Students** learning the DiffServ architecture (RFC 2474/2475) from a
  worked, validated, runnable reference, with the benefit of reading one
  architecture expressed in three simulator generations.
- **Network-simulation practitioners** adopting the ns-3 module for their
  own DSCP-aware studies, or comparing ns-2 and ns-3 modelling choices.

## What this handbook covers

- **Part I — Using Stratum.** Task-oriented recipes for building, configuring,
  and running the three clients (DiffServ, L4S, CAKE), including
  the AQM evaluation suite, wireless support, and troubleshooting guidance.
- **Part II — Architecture and design.** The Stratum substrate top-down:
  the four-slot architecture, the three clients as peers, the
  traffic-management background, and the layout of the ns-3 module.
- **Part III — Evidence and validation.** The three-way cross-simulator
  comparison, L4S and CAKE fidelity results, the wireless demonstration,
  and the AQM characterisation suite.
- **Appendices.** RFC 2697/2698/2859 conformance vectors (Appendix A), the
  long-form validation record with full methodological depth (Appendix B), the
  AQM-eval Flent interop workflow (Appendix C), and the IPv6 dual-stack
  mechanism with worked examples (Appendix D), and the command-line reference
  generated from each example's --PrintHelp (Appendix E).

## What this handbook does not cover

Three companion artefacts carry material that sits outside the handbook's
scope:

- **The EDD spec suite** — the contract the ns-3 port is built against —
  lives under `specs/` (`01-intent.md`, `02-structural.md`, `03-quality.md`).
  The handbook cites specs but does not reproduce them.
- **The architectural decision records** (ADRs) are kept in the project's
  development history. They record *why* a design choice was made; the
  handbook records *what* the resulting system does.
- **The accompanying paper** (submitted to ICNS3 2026) is the peer-review-facing narrative.
  The handbook is the reference the paper cites.

## Reading order

The chapter table in [`handbook/README.md`](README.md) lists every chapter
with its origin and reading-order recommendation. Three common paths:

- **New to DiffServ** — start with II-03 (traffic-management background) and
  II-04 (the DiffServ model), then II-02 for the Stratum four-slot
  architecture and the three clients as peers.
- **Adopting the ns-3 module** — start with Part I; I-02 gets you to a
  running simulation in 15 minutes; II-08 covers the ns-3 module layout.
- **Reviewing the evidence** — III-02 carries the three-way cross-simulator
  comparison; appendix A carries the RFC 2697/2698 conformance vectors.

## Provenance conventions

The handbook is 2026-written. Some chapters draw structural inspiration from
the 2001 thesis; others (like this one, and the ns-3 module chapters,
appendix A, appendix B) are pure-2026. Two markers make the distinction
visible to readers and auditors:

- **YAML frontmatter `origin:`** — every chapter declares its origin at the
  top. Values are `2026-written` (pure-2026 authoring) or
  `inspired-by-thesis-§N` (2026 prose organised using the thesis's section
  skeleton). No chapter reproduces thesis text verbatim.
- **Paragraph-level HTML comment pairs** — chapters that mix thesis-inspired
  and 2026-written material wrap added paragraphs in
  `<!-- added:2026 -->` / `<!-- end added -->` pairs so post-2001 RFC
  updates and 2026 reconstruction findings are attributable at a finer
  granularity than the chapter heading. Pure-2026 chapters rely on the YAML
  field alone.

When the handbook cites the thesis, it uses standard academic citation with
a section number ("as defined in the thesis §3.3.3"), never a verbatim quote
from `provenance/Andreozzi-2001-thesis.txt`.

## Terminology

**DiffServ4NS** is the full project name, in use since the 2006 SourceForge
release (and inherited from the 2001 internal name *DiffServ+* in thesis
Figure 3.11). **Stratum** is the name of the ns-3 substrate this handbook
describes. The handbook uses "Stratum" when referring to the ns-3 substrate
and its clients, and "DiffServ4NS" when referring to the 2001/ns-2 artefact
and its lineage. An earlier in-house shorthand ("DS4") is retired from the
prose; it survives only inside the stable `DS4-P1`/`DS4-P2`/`DS4-P3` patch
identifiers of the ns-2 patch catalogue.

The accompanying paper uses the same two names with the same
split: "Stratum" for the substrate, "DiffServ4NS" for the lineage artefact.

## How to cite

The release repository ships a `CITATION.cff` at its root; a Zenodo
concept DOI for the ns-3 substrate will be registered at a future tagged
release. Cite the concept DOI when referring to "DiffServ4NS" as a moving
target; cite the version-specific DOI when reproducing a specific figure or
table. The `v1.0` tag is the first release of the ns-3 substrate.

## See also

- [`handbook/README.md`](README.md) — chapter index and reading-order table.
- `provenance/LINEAGE.md` — the 25-year history this handbook documents.
- `specs/01-intent.md` — the intent-tier specification the ns-3 port is
  built against.

## Lineage (2001 → 2026)

The Stratum substrate has a 25-year history rooted in a master's thesis
completed in 2001 at the University of Pisa, with research work carried out
at Lappeenranta University of Technology in Finland. Sergio Andreozzi
developed the original module, internally named *DiffServ+*, as part of
that thesis, building on real-network measurements from a deployed DiffServ
infrastructure published by Tiziana Ferrari (INFN-CNAF) in 2000. Those
measurements remain the founding validation ground truth for every downstream
artefact. The thesis was published at ISCC 2002 and the module made available
on the author's personal website the same year. In 2006, the code was ported
from ns-2.1b8a to the ns-2.29 API, renamed DiffServ4NS, and released on
SourceForge under GPLv2; the release preserved the original algorithms
exactly while adapting the C++ glue for the newer simulator version. The
project then went dormant for two decades. In April 2026, active development
resumed with a concurrent ns-2.35 port (backporting five latent defects and
correcting the UDP header accounting) and a full ns-3 reimplementation that
extended the DiffServ core with L4S and CAKE as co-equal clients,
giving rise to the Stratum substrate documented in this handbook. The full
provenance story, including checksums, SourceForge archive links, and
per-milestone change notes, lives in the DiffServ4NS heritage repository
under `provenance/LINEAGE.md`.
